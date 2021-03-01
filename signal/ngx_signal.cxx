#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <sys/wait.h>

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"

// --------------------
// 和信号 有关的
// --------------------

// 封装信号的结构体
typedef struct
{
    int signo;           // 信号对应的数字编号
    const char *signame; // 信号对应的名字

    // 信号处理函数(sa.sa_sigaction)
    void (*handler)(int signo, siginfo_t *siginfo, void *ucontext);

} ngx_signal_t;

static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext);
static void ngx_process_get_status(void);

// 需要屏蔽的信号, 取一部分官方Nginx中的信号
ngx_signal_t signals[] =
    {
        // signo  signame   handler
        {SIGHUP, "SIGHUP", ngx_signal_handler},   // 终端断开信号，对于守护进程常用于reload重载配置文件通知--标识1
        {SIGINT, "SIGINT", ngx_signal_handler},   // 标识2
        {SIGTERM, "SIGTERM", ngx_signal_handler}, // 标识15
        {SIGCHLD, "SIGCHLD", ngx_signal_handler}, // 子进程退出时，父进程会收到这个信号--标识17
        {SIGQUIT, "SIGQUIT", ngx_signal_handler}, // 标识3
        {SIGIO, "SIGIO", ngx_signal_handler},     // 指示一个异步I/O事件【通用异步I/O信号】
        {SIGSYS, "SIGSYS, SIG_IGN", NULL},        // 我们想忽略这个信号，SIGSYS表示收到了一个无效系统调用，如果我们不忽略，进程会被操作系统杀死，--标识31
                                                  // 所以我们把handler设置为NULL，代表 我要求忽略这个信号，请求操作系统不要执行缺省的该信号处理动作（杀掉我）

        {0, NULL, NULL} // 信号对应的数字至少是1, 所以可以用0作为一个signals结束标记
};

// 描述: 注册signals数组中的信号
// 返回值: 0成功, -1失败
int ngx_init_signals()
{
    ngx_signal_t *sig;
    struct sigaction sa;

    for (sig = signals; sig->signo != 0; sig++)
    {
        memset(&sa, 0, sizeof(sigaction));
        if (sig->handler) // 方式1
        {
            sa.sa_sigaction = sig->handler;
            sa.sa_flags = SA_SIGINFO; // SA_SIGINFO: 让sa.sa_sigaction指定的信号处理函数生效
        }
        else // 方式2
        {
            sa.sa_handler = SIG_IGN;
        }

        sigemptyset(&sa.sa_mask); // 在信号处理函数执行时, 不阻塞任何信号
        // 参数3: 返回以往的对信号的处理方式
        if (sigaction(sig->signo, &sa, NULL) == -1)
        {
            ngx_log_error_core(NGX_LOG_EMERG, errno, "sigaction(%s) failed", sig->signame);
            return -1;
        }
    }

    return 0;
}

// 描述: 信号处理函数(sa.sa_sigaction)
static void ngx_signal_handler(int signo, siginfo_t *siginfo, void *ucontext)
{
    ngx_signal_t *sig;
    char *action; // 用于记录一个动作字符串, 以往日志文件中写

    // 遍历信号数组, 找到对应信号
    for (sig = signals; sig->signo != 0; sig++)
    {
        if (sig->signo == signo)
        {
            break;
        }
    }
    action = (char *)""; // 目前还没有什么动作

    // // 因为 ngx_reap 未使用, 所以这段if判断并没有用.
    // if (ngx_process == NGX_PROCESS_MASTER) // master进程
    // {
    //     switch (signo)
    //     {
    //     case SIGCHLD:
    //         ngx_reap = 1;
    //         break;
    //     default:
    //         break;
    //     }
    // }

    if (siginfo && siginfo->si_pid) // si_pid: 发送该信号的进程id
    {
        ngx_log_error_core(NGX_LOG_NOTICE, 0, "signal %d (%s) received from %P%s", signo, sig->signame, siginfo->si_pid, action);
    }
    else
    {
        ngx_log_error_core(NGX_LOG_NOTICE, 0, "signal %d (%s) received %s", signo, sig->signame, action);
    }

    if (signo == SIGCHLD) // 当你杀死一个子进程时, 父进程会收到这个SIGCHLD信号
    {
        ngx_process_get_status(); // 获取子进程的结束状态
    }

    return;
}

// 回收子进程, 并日志记录子进程退出状态.
// 调用: ngx_signal_handler()
static void ngx_process_get_status()
{
    pid_t pid;
    int status;
    int err;
    int one = 0; // 抄自官方nginx, 应该是标记信号正常处理过一次

    while (1)
    {
        // WNOHANG: 如果没有任何已经结束的子进程则马上返回, 不予以等待
        pid = waitpid(-1, &status, WNOHANG);

        if (pid == 0) // 不可能为0(子进程正在运行), 因为在ngx_signal_handler()中, 当收到SIGCHLD信号时该函数才被调用.
        {
            return;
        }

        if (pid == -1)
        {
            err = errno;      // 不能直接判断errno==EINTER, 必须定义中间变量err再判断.
            if (err == EINTR) // 调用被某个信号中断
            {
                continue;
            }

            if (err == ECHILD) // 没有子进程
            {
                if (one) // 已经回收过至少一个1子进程
                {
                    return;
                }
                else // 不可能
                {
                    ngx_log_error_core(NGX_LOG_INFO, err, "waitpid() failed!");
                    return;
                }
            }

            // 其他错误
            ngx_log_error_core(NGX_LOG_ALERT, err, "waitpid() failed!");
            return;
        }

        one = 1; // 标记 waitpid() 是否返回了子进程的pid

        if (WTERMSIG(status)) // 获取使子进程终止的信号
        {
            ngx_log_error_core(NGX_LOG_ALERT, 0, "pid = %P exited on signal %d!", pid, WTERMSIG(status));
        }
        else
        {
            // WEXITSTATUS()获取子进程传递给exit或者_exit参数的低八位
            ngx_log_error_core(NGX_LOG_NOTICE, 0, "pid = %P exited with code %d!", pid, WEXITSTATUS(status));
        }
    }

    return;
}
