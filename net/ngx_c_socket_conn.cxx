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
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

//---------------------------------------------------------------
// 和网络中 连接/连接池 有关的函数
//---------------------------------------------------------------

// 构造函数
ngx_connection_s::ngx_connection_s()
{
    iCurrsequence = 0;
    pthread_mutex_init(&logicPorcMutex, NULL); // 互斥量初始化
}

// 析构函数
ngx_connection_s::~ngx_connection_s()
{
    pthread_mutex_destroy(&logicPorcMutex); // 互斥量释放
}

// 分配一个连接时, 成员变量的初始化
// 调用: CSocekt::initconnection(), CSocekt::ngx_get_connection()
void ngx_connection_s::GetOneToUse()
{
    ++iCurrsequence;

    fd = -1;

    curStat = _PKG_HD_INIT;
    precvbuf = dataHeadInfo;
    irecvlen = sizeof(COMM_PKG_HEADER);
    precvMemPointer = NULL;

    iThrowsendCount = 0;
    psendMemPointer = NULL;

    events = 0; // epoll事件, 先给0

    lastPingTime = time(NULL);

    FloodkickLastTime = 0;
    FloodAttackCount = 0;

    iSendCount = 0;
}

// 回收一个连接时的一些收尾工作: 释放收/发缓冲区.
void ngx_connection_s::PutOneToFree()
{
    ++iCurrsequence;

    if (precvMemPointer != NULL)
    {
        CMemory::GetInstance()->FreeMemory(precvMemPointer);
        precvMemPointer = NULL;
    }

    if (psendMemPointer != NULL)
    {
        CMemory::GetInstance()->FreeMemory(psendMemPointer);
        psendMemPointer = NULL;
    }

    iThrowsendCount = 0; // 设置不设置感觉都行
}

//---------------------------------------------------------------

// 初始化连接池 (m_connectionList/m_freeconnectionList), 初始容量为 m_worker_connections, 如果不够用, 会在 ngx_get_connection()中扩容连接池.
// 连接池作用: 把客户端连接(socket)和连接池中的一个连接对象(ngx_connection_t)绑到一起, 连接对象可以记录很多有关该客户端连接的信息.
// 调用: CSocekt::ngx_epoll_init()
void CSocekt::initconnection()
{
    lpngx_connection_t p_Conn;
    CMemory *p_memory = CMemory::GetInstance();

    int ilenconnpool = sizeof(ngx_connection_t);
    for (int i = 0; i < m_worker_connections; ++i)
    {
        p_Conn = (lpngx_connection_t)p_memory->AllocMemory(ilenconnpool, true);
        p_Conn = new (p_Conn) ngx_connection_t(); // 定位new用法, 手工调用构造函数
        p_Conn->GetOneToUse();

        m_connectionList.push_back(p_Conn);
        m_freeconnectionList.push_back(p_Conn);
    }

    return;
}

// 回收连接池(m_connectionList), 释放所占内存.
// 调用: CSocekt::Shutdown_subproc()
void CSocekt::clearconnection()
{
    lpngx_connection_t p_Conn;
    CMemory *p_memory = CMemory::GetInstance();

    while (!m_connectionList.empty())
    {
        p_Conn = m_connectionList.front();
        m_connectionList.pop_front();
        p_Conn->~ngx_connection_t(); // 先析构, 再释放内存.
        p_memory->FreeMemory(p_Conn);
    }
}

// 从连接池中获取一个空闲连接, 如果没有空间连接就扩容连接池.
// 调用: CSocekt::ngx_event_accept()(被cfd所使用), CSocekt::ngx_epoll_init()(被lfd所使用)
lpngx_connection_t CSocekt::ngx_get_connection(int isock)
{
    CLock lock(&m_connectionMutex);

    lpngx_connection_t p_Conn;
    if (m_freeconnectionList.empty()) // 没空闲连接
    {
        CMemory *p_memory = CMemory::GetInstance();
        p_Conn = (lpngx_connection_t)p_memory->AllocMemory(sizeof(ngx_connection_t), true);
        p_Conn = new (p_Conn) ngx_connection_t();
        p_Conn->GetOneToUse();

        m_connectionList.push_back(p_Conn); // 加入到连接池, 统一管理.
    }
    else // 有空闲连接
    {
        p_Conn = m_freeconnectionList.front();
        m_freeconnectionList.pop_front();
        p_Conn->GetOneToUse();
    }

    p_Conn->fd = isock;
    return p_Conn;
}

// 归还连接到到连接池中
void CSocekt::ngx_free_connection(lpngx_connection_t pConn)
{
    CLock lock(&m_connectionMutex);
    pConn->PutOneToFree();
    m_freeconnectionList.push_back(pConn);

    return;
}

// 延迟回收: 用户已经接入进来开始干活, 干活过程中发生失败.
// 将要回收的连接仍进一个队列, 后续有专门的线程会回收这个队列中的连接.
// 调用: CSocekt::zdClosesocketProc()
void CSocekt::inRecyConnectQueue(lpngx_connection_t pConn)
{
    // (1) 判断是否被仍进
    bool iffind = false;

    CLock lock(&m_recyconnqueueMutex);
    for (auto iter = m_recyconnectionList.begin(); iter != m_recyconnectionList.end(); ++iter)
    {
        if ((*iter) == pConn)
        {
            iffind = true;
            break;
        }
    }

    if (iffind) // 已经在队列中, 不必再入了
    {
        return;
    }

    // (2) 仍进去.

    pConn->inRecyTime = time(NULL); // 记录回收时间
    ++pConn->iCurrsequence;
    m_recyconnectionList.push_back(pConn);
    ++m_total_recyconnection_n; // 待释放连接队列大小+1
    --m_onlineUserCount;        // 连入用户数量-1

    return;
}

// 清理 待释放连接队列 中的连接.
// (1) 从队列中erase();
// (2) ngx_free_connection 对应的pConn.
void *CSocekt::ServerRecyConnectionThread(void *threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
    CSocekt *pSocketObj = pThread->_pThis;

    time_t currtime;
    int err;
    std::list<lpngx_connection_t>::iterator pos, posend;
    lpngx_connection_t p_Conn;

    while (1)
    {
        usleep(200 * 1000); // 为简化问题, 我们直接每次休息200毫秒

        if (pSocketObj->m_total_recyconnection_n > 0)
        {
            currtime = time(NULL);

            err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);
            if (err != 0)
            {
                ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock()失败，返回的错误码为%d!", err);
            }

        lblRRTD:
            pos = pSocketObj->m_recyconnectionList.begin();
            posend = pSocketObj->m_recyconnectionList.end();
            for (; pos != posend; ++pos)
            {
                p_Conn = (*pos);
                if (((p_Conn->inRecyTime + pSocketObj->m_RecyConnectionWaitTime) > currtime) && (g_stopEvent == 0))
                {
                    continue; // 没到释放的时间
                }
                // 到释放的时间了

                // 凡是到释放时间的, iThrowsendCount都应该为0, 这里我们加点日志判断下.
                if (p_Conn->iThrowsendCount > 0) // 判断条件为>0, 不建议==0, 详细见 CSocekt::zdClosesocketProc().
                {
                    ngx_log_stderr(0, "CSocekt::ServerRecyConnectionThread()中到释放时间却发现p_Conn.iThrowsendCount!=0，这个不该发生");
                }

                // 流程走到这里, 表示可以释放.
                --pSocketObj->m_total_recyconnection_n;      // 待释放连接队列大小-1
                pSocketObj->m_recyconnectionList.erase(pos); // 迭代器虽然失效, 但pos所指内容在p_Conn里保存着
                pSocketObj->ngx_free_connection(p_Conn);     // 归还参数pConn所代表的连接到到连接池中

                // 迭代器已经失效, 需要重新遍历
                goto lblRRTD;
            }

            err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex);
            if (err != 0)
            {
                ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock()失败，返回的错误码为%d!", err);
            }
        }

        if (g_stopEvent == 1) // 要退出整个程序, 即使 没到释放的时间, 也要强制清除.
        {
            if (pSocketObj->m_total_recyconnection_n > 0)
            {
                err = pthread_mutex_lock(&pSocketObj->m_recyconnqueueMutex);
                if (err != 0)
                {
                    ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()中pthread_mutex_lock2()失败，返回的错误码为%d!", err);
                }

            lblRRTD2:
                pos = pSocketObj->m_recyconnectionList.begin();
                posend = pSocketObj->m_recyconnectionList.end();
                for (; pos != posend; ++pos)
                {
                    p_Conn = (*pos);
                    --pSocketObj->m_total_recyconnection_n;
                    pSocketObj->m_recyconnectionList.erase(pos);
                    pSocketObj->ngx_free_connection(p_Conn);

                    goto lblRRTD2;
                }

                err = pthread_mutex_unlock(&pSocketObj->m_recyconnqueueMutex);
                if (err != 0)
                {
                    ngx_log_stderr(err, "CSocekt::ServerRecyConnectionThread()pthread_mutex_unlock2()失败，返回的错误码为%d!", err);
                }
            }

            break; // 退出while (1)
        }
    }

    return (void *)0;
}

// 立刻回收, 用户刚刚连入时(CSocekt::ngx_event_accept)产生失败
// (1) ngx_free_connection 释放该pConn.
// (2) close对应的fd.
void CSocekt::ngx_close_connection(lpngx_connection_t pConn)
{
    ngx_free_connection(pConn);
    if (pConn->fd != -1)
    {
        close(pConn->fd);
        pConn->fd = -1;
    }
    return;
}
