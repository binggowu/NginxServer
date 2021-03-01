#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"

// ----------------------------
// 网络中连接 有关的函数
// ----------------------------

// 建立新连接
// 1. 调用accept4()获取cfd;
// 2. 从连接池中获取连接;
// 3. 调用ngx_epoll_oper_event(), 将该事件(cfd+连接)上epoll树(LT模式, 非ET模式)
// 调用: ngx_worker_process_cycle() -> ngx_process_events_and_timers() -> CSocekt::ngx_epoll_process_events()
void CSocekt::ngx_event_accept(lpngx_connection_t oldc)
{
    struct sockaddr mysockaddr; // 远端服务器的socket地址
    socklen_t socklen = sizeof(mysockaddr);
    int err;
    int level;
    int s;
    static int use_accept4 = 1; // 1: 使用accept4()函数
    lpngx_connection_t newc;

    // ngx_log_stderr(0, "这是几个\n"); // 这里会惊群, epoll技术本身有 惊群 的问题

    do
    {
        if (use_accept4)
        {
            // 参数SOCK_NONBLOCK: 返回一个非阻塞的socket, 节省一次ioctl调用
            s = accept4(oldc->fd, &mysockaddr, &socklen, SOCK_NONBLOCK);
        }
        else
        {
            s = accept(oldc->fd, &mysockaddr, &socklen);
        }

        // 惊群: 有时候不一定完全惊动所有4个worker进程, 可能只惊动其中2个等等, 其中一个成功其余的accept4()都会返回-1, 即报错 11: Resource temporarily unavailable.
        // 参考资料: https://blog.csdn.net/russell_tao/article/details/7204260
        // accept4()可以认为基本解决惊群问题, 但似乎并没有完全解决, 有时候还会惊动其他的worker进程. 在linux2.6内核上, accept系统调用已经不存在惊群了.

        if (s == -1)
        {
            err = errno;

            // 对accept(), send()/recv()而言, 事件未发生时, errno通常被设置成EAGAIN(再来一次)或者EWOULDBLOCK(期待阻塞), EAGAIN和EWOULDBLOCK是一样的.
            if (err == EAGAIN) // accept()没准备好
            {
                // 除非用一个循环不断的accept(), 不然一般不会有这个错误.
                return;
            }

            level = NGX_LOG_ALERT;
            if (err == ECONNABORTED) // 发生在对方意外关闭套接字后, 您的主机中的软件放弃了一个已建立的连接. 由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)
            {
                level = NGX_LOG_ERR;
            }
            else if (err == EMFILE || err == ENFILE) // EMFILE: 进程的fd已用尽, 已达到系统所允许单一进程所能打开的文件/套接字总数. 
            // 可参考：https://blog.csdn.net/sdn_prc/article/details/28661661 以及 https://bbs.csdn.net/topics/390592927
            // ulimit -n, 看看文件描述符限制, 如果是1024的话, 需要改大; 打开的文件句柄数过多, 把系统的fd软限制和硬限制都抬高.
            // ENFILE 这个errno的存在，表明一定存在system-wide的resource limits，而不仅仅有process-specific的resource limits。按照常识，process-specific的resource limits，一定受限于system-wide的resource limits.
            {
                level = NGX_LOG_CRIT;
            }
            // ngx_log_error_core(level,errno,"CSocekt::ngx_event_accept()中accept4()失败!");

            if (use_accept4 && err == ENOSYS) // accept4()函数没实现, 标记不使用accept4()函数, 改用accept()函数重试.
            {
                use_accept4 = 0;
                continue;
            }

            if (err == ECONNABORTED) // 对方关闭套接字, 可以忽略
            {
            }

            if (err == EMFILE || err == ENFILE)
            {
                // 官方做法: 先把读事件从listen socket上移除, 然后再弄个定时器, 定时器到了则继续执行该函数, 但是定时器到了有个标记, 会把读事件增加到listen socket上去.
            }

            return;
        }

        // 执行到此处, 表示accept4()/accept()成功.

        if (m_onlineUserCount >= m_worker_connections) // 用户连接数过多, 关闭该用户socket.
        {
            close(s);
            return;
        }

        // 如果某些恶意用户连上来发了1条数据就断, 不断连接, 会导致频繁调用 ngx_get_connection() 使用我们短时间内产生大量连接, 危及本服务器安全
        if (m_connectionList.size() > (m_worker_connections * 5))
        {
            // 比如你允许同时最大2048个连接, 但连接池却有了 2048*5 这么大的容量, 这肯定是表示短时间内 产生大量连接/断开, 因为我们的延迟回收机制, 这里连接还在垃圾池里没有被回收

            if (m_freeconnectionList.size() < m_worker_connections)
            {
                // 整个连接池这么大了, 而空闲连接却这么少了, 所以认为是短时间内产生大量连接, 发一个包后就断开, 所以必须断开新入用户的连接

                close(s);
                return;
            }
        }

        newc = ngx_get_connection(s);
        if (newc == NULL)
        {
            if (close(s) == -1)
            {
                ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_event_accept()中close(%d)失败!", s);
            }
            return;
        }

        // 成功的拿到了连接池中的一个连接

        memcpy(&newc->s_sockaddr, &mysockaddr, socklen);

        // 因为accept()返回的socket是阻塞的, 所以在此处设置非阻塞. 如果设置失败, 调用ngx_close_connection关闭连接.
        if (!use_accept4)
        {
            if (setnonblocking(s) == false)
            {
                ngx_close_connection(newc);
                return;
            }
        }

        newc->listening = oldc->listening; // 和监听socket的对象关联, 方便找到该连接的对应监听端口

        //newc->w_ready = 1;                                    // 标记可以写，新连接写事件肯定是ready的；【从连接池拿出一个连接时这个连接的所有成员都是0】

        newc->rhandler = &CSocekt::ngx_read_request_handler;  // 读处理函数，其实官方nginx中是ngx_http_wait_request_handler()
        newc->whandler = &CSocekt::ngx_write_request_handler; // 设置数据发送时的写处理函数。

        // EPOLLRDHUP: TCP连接的远端关闭或者半关闭
        // 事件类型为EPOLL_CTL_ADD时不需要这个参数
        if (ngx_epoll_oper_event(s, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP, 0, newc) == -1)
        {
            ngx_close_connection(newc);
            return;
        }

        /*
        else
        {
            //打印下发送缓冲区大小
            int           n;
            socklen_t     len;
            len = sizeof(int);
            getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
            ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040

            n = 0;
            getsockopt(s,SOL_SOCKET,SO_RCVBUF, &n, &len);
            ngx_log_stderr(0,"接收缓冲区的大小为%d!",n); //374400

            int sendbuf = 2048;
            if (setsockopt(s, SOL_SOCKET, SO_SNDBUF,(const void *) &sendbuf,n) == 0)
            {
                ngx_log_stderr(0,"发送缓冲区大小成功设置为%d!",sendbuf); 
            }

             getsockopt(s,SOL_SOCKET,SO_SNDBUF, &n, &len);
            ngx_log_stderr(0,"发送缓冲区的大小为%d!",n); //87040
        }
        */

        if (m_ifkickTimeCount == 1)
        {
            AddToTimerQueue(newc);
        }

        ++m_onlineUserCount; // 连入用户数量+1
        break;

    } while (1);

    return;
}
