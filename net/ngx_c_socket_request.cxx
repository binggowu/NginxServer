#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>   // uintptr_t
#include <stdarg.h>   // va_start....
#include <unistd.h>   // STDERR_FILENO等
#include <sys/time.h> // gettimeofday
#include <time.h>     // localtime_r
#include <fcntl.h>    // open
#include <errno.h>
#include <sys/ioctl.h> // ioctl
#include <arpa/inet.h>
#include <pthread.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

// --------------------------------------------
// 和网络 中 客户端发送来数据/服务器端收包 有关的代码
// --------------------------------------------

// 收包思路: 先收"包头", 根据包头中的内容确定包长, 再收"包体".
// (1) 调用 recvproc 收包
// (2) 收包头
// (3) 收包体
// 调用: ngx_epoll_process_events()
void CSocekt::ngx_read_request_handler(lpngx_connection_t pConn)
{
    bool isflood = false; // 是否flood攻击

    // (1) 调用 recvproc() 收包

    ssize_t reco = recvproc(pConn, pConn->precvbuf, pConn->irecvlen);
    if (reco <= 0)
    {
        return; // 问题在 recvproc() 已经处理过了
    }

    // 走到这里, 说明成功收到了一些字节, 就要开始处理收到的数据

    // (2) 收包状态 _PKG_HD_INIT 的处理
    if (pConn->curStat == _PKG_HD_INIT)
    {
        if (reco == m_iLenPkgHeader) // 正好收到完整包头, 这里拆解包头
        {
            ngx_wait_request_handler_proc_p1(pConn, isflood); // 包头处理函数
        }
        else
        {
            // 收到的包头不完整, 我们不能预料每个包的长度, 也不能预料各种拆包/粘包情况, 所以收到不完整包头(缺包)是很可能的
            pConn->curStat = _PKG_HD_RECVING;         // 接收包头中, 包头不完整, 要继续接收包头中
            pConn->precvbuf = pConn->precvbuf + reco; // 下次要收包头数据的位置
            pConn->irecvlen = pConn->irecvlen - reco; // 下次要收包头数据的长度
        }
    }
    // (3) 收到状态 _PKG_HD_RECVING 的处理
    else if (pConn->curStat == _PKG_HD_RECVING) // 包头没有收完整时, 这个条件才会成立
    {
        if (pConn->irecvlen == reco) // 缺多少, 收多少. 此时包头收完整.
        {
            ngx_wait_request_handler_proc_p1(pConn, isflood); // 包头处理函数
        }
        else
        {
            // 包头还是没收完整, 继续收包头
            pConn->precvbuf = pConn->precvbuf + reco; // 下次要收包头数据的位置
            pConn->irecvlen = pConn->irecvlen - reco; // 下次要收包头数据的长度
        }
    }
    // (4) 收包状态 _PKG_BD_INIT 的处理
    else if (pConn->curStat == _PKG_BD_INIT)
    {
        if (reco == pConn->irecvlen)
        {
            // Flood攻击检测是否开启
            if (m_floodAkEnable == 1)
            {
                isflood = TestFlood(pConn);
            }

            // 收到的宽度等于要收的宽度，包体也收完整了
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 收到的宽度小于要收的宽度
            pConn->curStat = _PKG_BD_RECVING;
            pConn->precvbuf = pConn->precvbuf + reco;
            pConn->irecvlen = pConn->irecvlen - reco;
        }
    }
    // (5) 收到状态 _PKG_BD_RECVING 的处理
    else if (pConn->curStat == _PKG_BD_RECVING)
    {
        // 接收包体中, 包体不完整, 继续接收中
        if (pConn->irecvlen == reco)
        {
            // Flood攻击检测是否开启
            if (m_floodAkEnable == 1)
            {
                isflood = TestFlood(pConn);
            }
            // 包体收完整
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else
        {
            // 包体没收完整, 继续收
            pConn->precvbuf = pConn->precvbuf + reco;
            pConn->irecvlen = pConn->irecvlen - reco;
        }
    }

    if (isflood == true)
    {
        // 客户端flood服务器, 则直接把客户端踢掉.
        ngx_log_stderr(errno, "发现客户端flood, 干掉该客户端!");
        zdClosesocketProc(pConn);
    }

    return;
}

// 封装了recv函数, 用来收包.
// 返回值: -1, 有问题发生, 且本函数已经把问题处理完毕(释放连接池中连接, 然后直接关闭 socket)
//        >0, 实际收到的字节数
// (1) 调用 recv() 收包.
// (2) 处理 recv()返回0 的情况.
// (2) 处理 recv()返回-1 的情况.
ssize_t CSocekt::recvproc(lpngx_connection_t pConn, char *buff, ssize_t buflen)
{
    ssize_t n; // 返回值

    n = recv(pConn->fd, buff, buflen, 0); // 最后一个参数 flag 一般为0
    if (n == 0)
    {
        // 客户端关闭(完成4次挥手)
        ngx_log_stderr(0, "连接被客户端正常关闭[4路挥手关闭]!");
        zdClosesocketProc(pConn);
        return -1;
    }

    // 走这里, 客户端没断

    if (n < 0) // 这被认为有错误发生
    {
        // EAGAIN 和 EWOULDBLOCK(常用在hp上)是一样的值, 表示没收到数据.
        // 一般在 ET 模式下会出现这个错误, 因为 ET 模式下是不停的 recv, 肯定有一个时刻收到这个 errno, 但 LT 模式下一般是来事件才收, 所以不该出现这个返回值.
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // LT 模式不该出现这个errno, 而且也不是错误, 所以不当做错误处理.
            ngx_log_stderr(errno, "CSocekt::recvproc()中 errno==EAGAIN || errno==EWOULDBLOCK 成立, 出乎意料.");
            return -1;
        }

        // EINTR 错误的产生: 服务器端设置了信号捕获机制和子进程, 当在父进程阻塞于慢的系统调用时, 由父进程捕获到了一个有效信号时, 内核会致使 accept() 返回一个EINTR错误(被中断的系统调用).
        if (errno == EINTR) // Nginx官方不认为是错误
        {
            // LT 模式不该出现这个errno, 而且也不是错误, 所以不当做错误处理.
            ngx_log_stderr(errno, "CSocekt::recvproc()中errno == EINTR成立，出乎我意料！");
            return -1;
        }

        // 如下都是异常, 需要关闭客户端socket, 回收连接池中连接

        // errno参考: http://dhfapiran1.360drm.com

        if (errno == ECONNRESET)
        {
            // 如果客户端没有正常关闭 socket 连接, 却关闭了整个运行程序(直接给服务器发送rst包, 而不是4次挥手), 那么会产生这个错误.
            // 10054(WSAECONNRESET)--远程程序正在连接的时候关闭会产生这个错误--远程主机强迫关闭了一个现有的连接
            // 算常规错误吧【普通信息型】日志都不用打印，没啥意思，太普通的错误

            //... 一些大家遇到的很普通的错误信息可以往这里增加各种, 代码要慢慢完善, 一步到位不可能, 很多服务器程序经过很多年的完善才比较圆满.
        }
        else
        {
            // 能走到这里的都表示错误, 打印一下日志, 希望知道一下是啥错误
            if (errno == EBADF) // Bad file descriptor
            {
                // 因为多线程, 偶尔会干掉socket, 所以不排除产生这个错误的可能性.
            }
            else
            {
                ngx_log_stderr(errno, "CSocekt::recvproc()中发生错误, 我打印出来看看是啥错误!"); //正式运营时可以考虑这些日志打印去掉
            }
        }

        ngx_log_stderr(0,"连接被客户端 非正常关闭！");
        zdClosesocketProc(pConn);

        return -1;
    }

    return n;
}

// 收包头, 包处理阶段1
void CSocekt::ngx_wait_request_handler_proc_p1(lpngx_connection_t pConn, bool &isflood)
{
    CMemory *p_memory = CMemory::GetInstance();

    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)pConn->dataHeadInfo;
    unsigned short e_pkgLen = ntohs(pPkgHeader->pkgLen);

    // 包长不合法, 认为是恶意包/错误包
    if (e_pkgLen < m_iLenPkgHeader || e_pkgLen > (_PKG_MAX_LENGTH - 1000))
    {
        // 复原"收包相关的变量"
        pConn->curStat = _PKG_HD_INIT;
        pConn->precvbuf = pConn->dataHeadInfo;
        pConn->irecvlen = m_iLenPkgHeader;
    }
    else // 包长合法
    {
        char *pTmpBuffer = (char *)p_memory->AllocMemory(m_iLenMsgHeader + e_pkgLen, false); // 分配内存, 大小为 消息头长+包长
        pConn->precvMemPointer = pTmpBuffer;

        // 填写 消息头 内容
        LPSTRUC_MSG_HEADER ptmpMsgHeader = (LPSTRUC_MSG_HEADER)pTmpBuffer;
        ptmpMsgHeader->pConn = pConn;
        ptmpMsgHeader->iCurrsequence = pConn->iCurrsequence; // 给消息头中的 iCurrsequence赋值

        // 填写 包头 内容
        pTmpBuffer += m_iLenMsgHeader;
        memcpy(pTmpBuffer, pPkgHeader, m_iLenPkgHeader);

        // 只有包头无包体
        if (e_pkgLen == m_iLenPkgHeader)
        {
            // Flood攻击检测是否开启
            if (m_floodAkEnable == 1)
            {
                isflood = TestFlood(pConn);
            }

            // 直接入 收消息队列, 待后续业务逻辑线程去处理
            ngx_wait_request_handler_proc_plast(pConn, isflood);
        }
        else // 开始收包体
        {
            pConn->curStat = _PKG_BD_INIT;                  // 准备接收包体
            pConn->precvbuf = pTmpBuffer + m_iLenPkgHeader; // 指向包体
            pConn->irecvlen = e_pkgLen - m_iLenPkgHeader;   // 包体长
        }
    }

    return;
}

// 收包体, 包处理阶段2
void CSocekt::ngx_wait_request_handler_proc_plast(lpngx_connection_t pConn, bool &isflood) // 参数 isflood 是个引用
{
    if (isflood == false)
    {
        // 入消息队列, 并触发线程处理消息
        g_threadpool.inMsgRecvQueueAndSignal(pConn->precvMemPointer);
    }
    else
    {
        // 直接释放掉内存
        CMemory *p_memory = CMemory::GetInstance();
        p_memory->FreeMemory(pConn->precvMemPointer);
    }

    // 收到相关的复原操作
    pConn->curStat = _PKG_HD_INIT;
    pConn->precvMemPointer = NULL;
    pConn->precvbuf = pConn->dataHeadInfo;
    pConn->irecvlen = m_iLenPkgHeader;

    return;
}

// 封装send(), 并对返回值做了判断处理. 借鉴官方Nginx的ngx_unix_send()
// 发送失败时, 并不在send动作里关闭socket, 集中到recv那里处理, 否则send, recv都处理都处理连接断开的情况会乱套. 连接断开epoll会通知并且 recvproc()里会处理, 不在这里处理.
// 返回值:
//  >0, 成功发送了一些字节
//  =0, 对方连接断了
//  -1, 本方发送缓冲区满了
//  -2, 对方断开的错误
ssize_t CSocekt::sendproc(lpngx_connection_t c, char *buff, ssize_t size)
{
    ssize_t n;
    for (;;)
    {
        n = send(c->fd, buff, size, 0);
        if (n > 0) // 发送成功一些数据, 但发送了多少并不关心, 也不需要再次send
        {
            // 这里有两种情况
            // (1) n == size, 完全发完毕了
            // (2) n < size, 没发完, 肯定是发送缓冲区满了.
            return n;
        }

        if (n == 0) // send()返回0，要么你发送的字节是0, 要么对方主动关闭了连接
        {

            return 0;
        }

        if (errno == EAGAIN) // 等于EWOULDBLOCK, 表示发送缓冲区满了
        {
            return -1;
        }

        if (errno == EINTR)
        {
            ngx_log_stderr(errno, "CSocekt::sendproc()中send()失败.");
        }
        else // 其他错误码
        {
            return -2;
        }
    }
}

// 设置数据发送时的写处理函数, 当数据可写时, epoll通知我们,  中调用此函数
// 能走到这里, 数据就是没法送完毕, 要继续发送
void CSocekt::ngx_write_request_handler(lpngx_connection_t pConn)
{
    CMemory *p_memory = CMemory::GetInstance();

    ssize_t sendsize = sendproc(pConn, pConn->psendbuf, pConn->isendlen);

    if (sendsize > 0 && sendsize != pConn->isendlen) // 只发出去一部分. LT模式会不停的通知, 所以此处直接退出即可
    {
        pConn->psendbuf = pConn->psendbuf + sendsize;
        pConn->isendlen = pConn->isendlen - sendsize;
        return;
    }
    else if (sendsize == -1) // 不可能, 可以发送数据时通知我发送数据, 我发送时你却通知我发送缓冲区满.
    {
        ngx_log_stderr(errno, "CSocekt::ngx_write_request_handler()时if(sendsize == -1)成立, 这很怪异.");
        return;
    }

    if (sendsize > 0 && sendsize == pConn->isendlen) // 发送完毕
    {
        // 如果是成功的发送完毕数据, 则把写事件通知从epoll中干掉吧. 其他情况, 那就是断线了, 等着系统内核把连接从红黑树中干掉即可.

        if (ngx_epoll_oper_event(
                pConn->fd,
                EPOLL_CTL_MOD, // 修改, 减去写通知
                EPOLLOUT, 1,   // 减去EPOLLOUT事件
                pConn) == -1)
        {
            ngx_log_stderr(errno, "CSocekt::ngx_write_request_handler()中ngx_epoll_oper_event()失败.");
        }

        ngx_log_stderr(0, "CSocekt::ngx_write_request_handler()中数据发送完毕, 很好."); // 提示, 商用时可以干掉
    }

    // 数据发送完毕, 或对方断开连接.

    p_memory->FreeMemory(pConn->psendMemPointer); // 释放内存
    pConn->psendMemPointer = NULL;

    --pConn->iThrowsendCount; // 这个值恢复了, 触发下面一行的信号量才有意义
    if (sem_post(&m_semEventSendQueue) == -1)
    {
        ngx_log_stderr(0, "CSocekt::ngx_write_request_handler()中sem_post(&m_semEventSendQueue)失败.");
    }

    return;
}

// 业务逻辑处理线程主函数, 专门处理各种接收到的TCP消息. 可以定义为纯虚函数.
// pMsgBuf: 客户端发送过来的消息缓冲区, 消息格式: 消息头+包头+包体.
void CSocekt::threadRecvProcFunc(char *pMsgBuf)
{
    return;
}
