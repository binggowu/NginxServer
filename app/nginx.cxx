#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/time.h>

#include "ngx_macro.h"        // 各种宏定义
#include "ngx_func.h"         // 各种函数声明
#include "ngx_c_conf.h"       // 和配置文件处理相关的类, 名字带c_表示和类有关
#include "ngx_c_socket.h"     // 和socket通讯相关
#include "ngx_c_memory.h"     // 和内存分配释放等相关
#include "ngx_c_threadpool.h" // 和多线程有关
#include "ngx_c_crc32.h"      // 和crc32校验算法有关
#include "ngx_c_slogic.h"     // 和socket通讯相关

static void freeresource();

// 和设置标题相关的变量

size_t g_argvneedmem = 0; // 参数argv所需要的内存大小
size_t g_envneedmem = 0;  // 环境变量所占内存大小
int g_os_argc;            // 参数 argc
char **g_os_argv;         // 参数 argv
char *gp_envmem = NULL;   // 指向自己分配的env环境变量的内存, 在 ngx_init_setproctitle() 中会被new

// socket, 线程池相关

CLogicSocket g_socket;    // socket全局对象
CThreadPool g_threadpool; // 线程池全局对象

// 和进程本身有关的全局量

int g_daemonized = 0; // 守护进程标记，标记是否启用了守护进程模式，0：未启用，1：启用了
int g_stopEvent;      // 整个程序退出的标识, 0不退出, 1退出, 未使用.
pid_t ngx_pid;        // 当前进程的pid
pid_t ngx_parent;     // 父进程的pid
int ngx_process;      // 进程类型, 比如master,worker进程等

// 标记子进程状态变化, 一般是子进程发来SIGCHLD信号表示退出, 未使用.
// 调用: ngx_signal_handler()[ngx_signal.cxx]
sig_atomic_t ngx_reap;

int main(int argc, char *const *argv)
{
    int exitcode = 0; // 退出代码, 0表示正常退出, 1/-1表示异常, 2表示系统找不到指定文件, 如nginx.conf
    int i;            // 临时用

    // (1) 无伤大雅也不需要释放的放最上边
    g_stopEvent = 0;
    ngx_pid = getpid();
    ngx_parent = getppid();

    // 标题 相关的变量
    g_argvneedmem = 0;
    for (i = 0; i < argc; i++)
    {
        g_argvneedmem += strlen(argv[i]) + 1; // +1: 末尾有'\0', 是占实际内存位置的, 要算进来
    }
    for (i = 0; environ[i]; i++) // environ[i]是否为空作为环境变量结束标记
    {
        g_envneedmem += strlen(environ[i]) + 1; // +1: 末尾有'\0', 是占实际内存位置的, 要算进来
    }
    g_os_argc = argc;
    g_os_argv = (char **)argv;

    ngx_log.fd = -1; // -1: 表示日志文件尚未打开, 因为后边 ngx_log_stderr 要用, 所以这里先给-1
    ngx_process = NGX_PROCESS_MASTER;
    ngx_reap = 0;

    // (2) 单例类初始化
    CConfig *p_config = CConfig::GetInstance();
    CMemory::GetInstance();
    CCRC32::GetInstance();

    if (p_config->Load("nginx.conf") == false)
    {
        ngx_log_stderr(0, "配置文件[%s]载入失败, 退出!", "nginx.conf");
        exitcode = 2;
        goto lblexit;
    }

    // (3) 一些必须事先准备好的资源, log, signal, socket初始化
    ngx_log_init();

    if (ngx_init_signals() != 0)
    {
        exitcode = 1;
        goto lblexit;
    }

    if (g_socket.Initialize() == false)
    {
        exitcode = 1;
        goto lblexit;
    }

    // (4) 一些不好归类的其他类别的代码，准备放这里
    ngx_init_setproctitle();

    // (5) 创建守护进程
    if (p_config->GetIntDefault("Daemon", 0) == 1) // 1: 按守护进程方式运行
    {
        int cdaemonresult = ngx_daemon();
        if (cdaemonresult == -1) // fork()失败
        {
            exitcode = 1;
            goto lblexit;
        }

        if (cdaemonresult == 1) // 父进程
        {
            freeresource();
            exitcode = 0;
            return exitcode; // 整个进程直接在这里退出
        }

        // 走到这里, 成功创建了守护进程.
        g_daemonized = 1; // 标记是否启用了守护进程模式, 0未启用, 1启用.
    }

    // (6) 主工作流程, 父子进程在正常工作期间都在这个函数里循环.
    ngx_master_process_cycle();

    // 释放资源
lblexit:
    ngx_log_stderr(0, "程序退出，再见了!");
    freeresource();
    return exitcode;
}

// 专门在程序执行末尾释放资源的函数
// 1. 设置"可执行程序标题"为环境变量分配的内存
// 2. 关闭日志文件
void freeresource()
{
    // 1. 设置"可执行程序标题"为环境变量分配的内存
    if (gp_envmem)
    {
        delete[] gp_envmem;
        gp_envmem = NULL;
    }

    // 2. 关闭日志文件
    if (ngx_log.fd != STDERR_FILENO && ngx_log.fd != -1)
    {
        close(ngx_log.fd);
        ngx_log.fd = -1; // 防止被再次 close()
    }
}
