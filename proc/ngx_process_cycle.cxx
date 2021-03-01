#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

// ---------------
// 子进程相关
// ---------------

static void ngx_start_worker_processes(int threadnums);
static int ngx_spawn_process(int threadnums, const char *pprocname);
static void ngx_worker_process_cycle(int inum, const char *pprocname);
static void ngx_worker_process_init(int inum);

// 变量声明
static u_char master_process[] = "master process";

// (1) 设置进程新的信号屏蔽字, 保护"不希望由信号中断"的代码临界区 (sigprocmask)
// (2) 设置master进程标题 (ngx_setproctitle)
// (3) 创建并启动的 worker 进程 (ngx_start_worker_processes)
// (4) 阻塞主进程 (sigsuspend)
void ngx_master_process_cycle()
{
    // (1) 设置进程新的信号屏蔽字, 保护"不希望由信号中断"的代码临界区 (sigprocmask)
    sigset_t set;      // 信号集
    sigemptyset(&set); // 清空信号集

    // Nginx官方代码中有这些信号, 建议fork()子进程时学习这种写法, 防止信号的干扰.
    sigaddset(&set, SIGCHLD);  // 子进程状态改变
    sigaddset(&set, SIGALRM);  // 定时器超时
    sigaddset(&set, SIGIO);    // 异步I/O
    sigaddset(&set, SIGINT);   // 终端中断符
    sigaddset(&set, SIGHUP);   // 连接断开
    sigaddset(&set, SIGUSR1);  // 用户定义信号
    sigaddset(&set, SIGUSR2);  // 用户定义信号
    sigaddset(&set, SIGWINCH); // 终端窗口大小改变
    sigaddset(&set, SIGTERM);  // 终止
    sigaddset(&set, SIGQUIT);  // 终端退出符
    // 可以根据开发的实际需要往其中添加其他要屏蔽的信号

    // 设置进程新的信号屏蔽字(SIG_BLOCK), 即"当前信号屏蔽字"和"第二个参数指向的信号集" 的并集
    if (sigprocmask(SIG_BLOCK, &set, NULL) == -1)
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_master_process_cycle() 中 sigprocmask() 失败.");
    }
    // 即便 sigprocmask() 失败, 程序流程也继续往下走

    // (2) 设置master进程标题 (ngx_setproctitle)
    size_t size;
    int i;
    size = sizeof(master_process); // sizeof(字符串), 末尾的'\0'是被计算进来的
    size += g_argvneedmem;         // argv参数长度加进来
    if (size < 1000)               // 长度小于这个才设置标题
    {
        char title[1000] = {0};
        strcpy(title, (const char *)master_process); // "master process"
        strcat(title, " ");                          // 跟一个空格分开, 清晰一点
        for (i = 0; i < g_os_argc; i++)              // "master process ./nginx"
        {
            strcat(title, g_os_argv[i]);
        }
        ngx_setproctitle(title); // 设置标题
        ngx_log_error_core(NGX_LOG_NOTICE, 0, "%s %P 【master进程】启动并开始运行......!", title, ngx_pid);
    }

    // (3) 创建并启动的 worker 进程 (ngx_start_worker_processes)
    CConfig *p_config = CConfig::GetInstance();
    int workprocess = p_config->GetIntDefault("WorkerProcesses", 1);
    ngx_start_worker_processes(workprocess); // 创建 worker 子进程

    // (4) 阻塞主进程 (sigsuspend)
    // 创建子进程后, 父进程的执行流程会返回到这里, 子进程不会走进来
    sigemptyset(&set); // 清空信号集
    for (;;)
    {
        // sigsuspend(&mask )是一个原子操作, 包含4个步骤:
        // a) 根据给定的参数设置新的mask(因为是个空集，所以不阻塞任何信号), 并阻塞当前进程(不占用cpu),
        // b) 一旦收到信号, 便恢复原先的信号屏蔽(阻塞了多达10个信号), 从而保证下边的执行流程不会再次被其他信号截断.
        // c) 调用该信号对应的信号处理函数.
        // d) 信号处理函数返回后, sigsuspend返回, 使程序流程继续往下走.

        sigsuspend(&set); // 此时 master 进程完全靠信号驱动干活
        // sleep(1);         // 休息1秒
    }

    return;
}

// 描述: 创建并启动指定数量的 worker 进程
// 参数threadnums: 要创建的子进程数量, 实质是调用了 threadnums 次 ngx_spawn_process().
static void ngx_start_worker_processes(int threadnums)
{
    int i;
    for (i = 0; i < threadnums; i++)
    {
        ngx_spawn_process(i, "worker process");
    }
    return;
}

// 描述: 调用fork()创建子进程, 父进程退出, 子进程继续(ngx_worker_process_cycle).
// 参数inum: 进程编号, 从0开始
// 参数pprocname: 子进程名字 "worker process"
static int ngx_spawn_process(int inum, const char *pprocname)
{
    pid_t pid;

    pid = fork();
    switch (pid)
    {
    case -1: // fork()失败
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_spawn_process() 中 fork() 产生子进程num=[%d],procname=[\"%s\"]失败.", inum, pprocname);
        return -1;

    case 0: // 子进程分支
        ngx_parent = ngx_pid;
        ngx_pid = getpid();
        ngx_worker_process_cycle(inum, pprocname); // 所有worker子进程, 在这个函数里不断循环着不出来
        break;

    default: // 父进程, 直接break
        break;
    }

    // 只有父进程分支会走到这里
    return pid;
}

// 描述: worker子进程的功能函数
// 参数inum: 进程编号, 从0开始
// 参数pprocname: 子进程名字 "worker process"
// 1) 先初始化worker子进程(ngx_worker_process_init).
// 2) 然后循环处理网络事件, 定时器事件, 外提供web服务(ngx_process_events_and_timers).
static void ngx_worker_process_cycle(int inum, const char *pprocname)
{
    ngx_process = NGX_PROCESS_WORKER; // 设置进程的类型，是worker进程

    ngx_worker_process_init(inum);

    ngx_setproctitle(pprocname); // 重新为子进程设置进程名, 不要与父进程重复
    ngx_log_error_core(NGX_LOG_NOTICE, 0, "%s %P [worker进程]启动并开始运行......!", pprocname, ngx_pid);

    // worker子进程在这个循环里一直不出来
    for (;;)
    {
        ngx_process_events_and_timers(); // 处理网络事件和定时器事件
    }

    // 一般情况不会执行到这里
    g_threadpool.StopAll();      // 使线程池中的所有线程安全退出
    g_socket.Shutdown_subproc(); // socket需要释放的东西考虑释放
    return;
}

// 描述: worker子进程创建时的初始化工作
// 参数inum: 进程编号, 从0开始
// (1) 取消信号屏蔽(sigprocmask)
// (2) 创建 收消息队列 的线程池(CThreadPool::Create)
// (3) 逻辑和通讯子类的初始化
// (4) 初始化 epoll, 同时往监听 socket 上增加监听事件 (g_socket.ngx_epoll_init)
static void ngx_worker_process_init(int inum)
{
    // (1) 取消信号屏蔽(sigprocmask)
    sigset_t set;                                   // 信号集
    sigemptyset(&set);                              // 清空信号集
    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) // 原来是屏蔽那10个信号, 现在不再屏蔽任何信号
    {
        ngx_log_error_core(NGX_LOG_ALERT, errno, "ngx_worker_process_init() 中 sigprocmask() 失败.");
    }

    // (2) 创建 收消息队列 的线程池(CThreadPool::Create)
    // 线程池代码, 要比和socket相关的内容优先执行
    CConfig *p_config = CConfig::GetInstance();
    int tmpthreadnums = p_config->GetIntDefault("ProcMsgRecvWorkThreadCount", 5); // 收消息队列的"线程池"
    if (g_threadpool.Create(tmpthreadnums) == false)
    {
        exit(-2); // 此时内存没释放, 但是简单粗暴退出.
    }
    // sleep(1); // 再休息1秒, 等待线程池中所有线程的ifrunning都为true.

    // (3) 逻辑和通讯子类的初始化
    if (g_socket.Initialize_subproc() == false) // 初始化子进程需要具备的一些多线程能力相关的信息
    {
        exit(-2); // 此时内存没释放, 但是简单粗暴退出.
    }

    // (4) 初始化epoll相关内容, 同时往监听socket上增加监听事件
    g_socket.ngx_epoll_init();

    // g_socket.ngx_epoll_listenportstart(); // 往监听 socket 上增加监听事件，从而开始让监听端口履行其职责. 如果不加这行, 虽然端口能连上, 但不会触发ngx_epoll_process_events() 里边的 epoll_wait() 往下走.

    return;
}
