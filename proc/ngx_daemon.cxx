// ---------------
//守护进程相关
// ---------------
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "ngx_func.h"
#include "ngx_macro.h"
#include "ngx_c_conf.h"

// 描述: 创建守护进程
// 返回值: 失败返回-1, 子进程返回0, 父进程返回1
// (1) fork()一个子进程, 这个子进程就是master进程.
// (2) 脱离终端(setsid).
// (3) 设置mask为0, 不要让它来限制文件权限, 以免引起混乱(umask).
// (4) 以读写方式打开黑洞"/dev/null", 并将stdin, stdout, stderror重定向到这个黑洞.
int ngx_daemon()
{
    // (1) fork()一个子进程, 这个子进程就是master进程.
    switch (fork())
    {
    case -1: // 创建子进程失败
        ngx_log_error_core(NGX_LOG_EMERG, errno, "ngx_daemon()中fork()失败!");
        return -1;
    case 0: // 子进程, 就是master进程
        break;
    default: // 父进程, 直接退出, 回到主流程去释放资源
        return 1;
    }

    // 走到此处, 肯定是fork()出来的子进程

    ngx_parent = ngx_pid;
    ngx_pid = getpid();

    // (2) 脱离终端(setsid).
    if (setsid() == -1)
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, "ngx_daemon()中setsid()失败!");
        return -1;
    }

    // (3) 设置mask为0, 不要让它来限制文件权限, 以免引起混乱(umask).
    umask(0);

    // (4) 以读写方式打开黑洞"/dev/null", 并将stdin, stdout, stderror重定向到这个黑洞.
    int fd = open("/dev/null", O_RDWR);
    if (fd == -1)
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, "ngx_daemon()中open(\"/dev/null\")失败!");
        return -1;
    }
    if (dup2(fd, STDIN_FILENO) == -1)
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, "ngx_daemon()中dup2(STDIN)失败!");
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) == -1)
    {
        ngx_log_error_core(NGX_LOG_EMERG, errno, "ngx_daemon()中dup2(STDOUT)失败!");
        return -1;
    }
    if (fd > STDERR_FILENO) // fd应该是3, 这个应该成立
    {
        if (close(fd) == -1) // 释放这个文件描述符, 不然这个文件描述符会被一直占着.
        {
            ngx_log_error_core(NGX_LOG_EMERG, errno, "ngx_daemon()中close(fd)失败!");
            return -1;
        }
    }

    return 0; // 子进程返回0
}
