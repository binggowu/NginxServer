#include <stdarg.h>
#include <unistd.h>

#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_threadpool.h"
#include "ngx_c_memory.h"
#include "ngx_macro.h"

//和 线程池 有关的函数放这里

// 静态成员初始化
pthread_mutex_t CThreadPool::m_pthreadMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t CThreadPool::m_pthreadCond = PTHREAD_COND_INITIALIZER;

bool CThreadPool::m_shutdown = false;

// 构造函数
CThreadPool::CThreadPool()
{
    m_iRunningThreadNum = 0;
    m_iLastEmgTime = 0;
}

// 析构函数
CThreadPool::~CThreadPool()
{
    // 资源释放在 StopAll() 里统一进行
    clearMsgRecvQueue();
}

// 清理 收消息队列
void CThreadPool::clearMsgRecvQueue()
{
    char *sTmpMempoint;
    CMemory *p_memory = CMemory::GetInstance();

    // 尾声阶段, 需要互斥?
    while (!m_MsgRecvQueue.empty())
    {
        sTmpMempoint = m_MsgRecvQueue.front();
        m_MsgRecvQueue.pop_front();
        p_memory->FreeMemory(sTmpMempoint);
    }
}

// 描述: 线程入口函数, 处理接受到的消息. 如果要让线程退出, 只需要设置m_shutdown为true即可(CThreadPool::StopAll).
// (1) 从 收消息队列 中取消息, 如果没有消息就阻塞等待.
// (2) 调用 CLogicSocket::threadRecvProcFunc() 处理消息.
void *CThreadPool::ThreadFunc(void *threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
    CThreadPool *pThreadPoolObj = pThread->_pThis; // 静态成员函数不能访问成员变量, 只能通过这种方式访问.

    CMemory *p_memory = CMemory::GetInstance();
    int err;
    while (true)
    {
        err = pthread_mutex_lock(&m_pthreadMutex);
        if (err != 0)
        {
            ngx_log_stderr(err, "CThreadPool::ThreadFunc() 中 pthread_mutex_lock() 失败, 返回的错误码为 [%d]", err);
        }

        // 必须要用 while, 避免"虚假唤醒".
        while ((pThreadPoolObj->m_MsgRecvQueue.size() == 0) && m_shutdown == false)
        {
            if (pThread->ifrunning == false)
            {
                // 标记为true了才允许调用StopAll(), 测试中发现如果Create()和StopAll()紧挨着调用, 就会导致线程混乱, 所以每个线程必须执行到这里, 才认为是启动成功了.
                pThread->ifrunning = true;
            }

            pthread_cond_wait(&m_pthreadCond, &m_pthreadMutex); // 线程池初始化时, 所有线程必然是卡在这里等待的.
        }

        // 让线程退出, 只需要设置 m_shutdown 为true.
        if (m_shutdown)
        {
            pthread_mutex_unlock(&m_pthreadMutex); // 解锁互斥量
            break;
        }

        // 取消息
        char *jobbuf = pThreadPoolObj->m_MsgRecvQueue.front();
        pThreadPoolObj->m_MsgRecvQueue.pop_front();

        err = pthread_mutex_unlock(&m_pthreadMutex);
        if (err != 0)
        {
            ngx_log_stderr(err, "CThreadPool::ThreadFunc() 中 pthread_mutex_unlock() 失败, 返回的错误码为 [%d].", err);
        }

        // 能走到这里的, 就是有消息可以处理

        ++pThreadPoolObj->m_iRunningThreadNum; // 1) 正在干活的线程数量+1
        g_socket.threadRecvProcFunc(jobbuf);   // 2) 处理消息
        p_memory->FreeMemory(jobbuf);          // 3) 处理完毕, 释放消息内存
        --pThreadPoolObj->m_iRunningThreadNum; // 4) 正在干活的线程数量-1
    }

    // 能走出来表示整个程序要结束啊, 怎么判断所有线程都结束?
    return NULL;
}

// 描述: 创建线程池中的所有线程
// (1) 创建线程池中所有线程
// (2) 确保每个线程都运行到pthread_cond_wait()
bool CThreadPool::Create(int threadNum)
{
    ThreadItem *pNew;
    int err;

    // (1) 创建线程池中所有线程
    m_iThreadNum = threadNum;
    for (int i = 0; i < m_iThreadNum; ++i)
    {
        m_threadVector.push_back(pNew = new ThreadItem(this));
        err = pthread_create(&pNew->_Handle, NULL, ThreadFunc, pNew);
        if (err != 0)
        {
            ngx_log_stderr(err, "CThreadPool::Create()创建线程%d失败，返回的错误码为%d!", i, err);
            return false;
        }
        else
        {
            ngx_log_stderr(0, "CThreadPool::Create()创建线程%d成功,线程id=%d", pNew->_Handle);
        }
    }

    // (2) 确保每个线程都运行到pthread_cond_wait(), 只有这样, 线程才能进行后续工作.
    bool allrunning;
    while (true)
    {
        allrunning = true;
        for (auto iter = m_threadVector.begin(); iter != m_threadVector.end(); ++iter)
        {
            if ((*iter)->ifrunning == false) // 线程没有真正启动起来
            {
                allrunning = false;
                usleep(100 * 1000); // 单位是微妙(1毫秒=1000微妙)
                break;
            }
        }

        if (allrunning)
        {
            break;
        }
    }

    return true;
}

// 描述: 使线程池中的所有线程安全退出
// (1) 唤醒卡在 pthread_cond_wait() 的所有线程
// (2) 调用 pthread_join() 来等待所有线程返回
// (3) 释放(delete)线程池中的线程
void CThreadPool::StopAll()
{
    if (m_shutdown == true) // 已经调用过, 就不要重复调用了
    {
        return;
    }

    m_shutdown = true;

    // (1) 唤醒卡在pthread_cond_wait()的所有线程, 一定要在改变条件状态以后再给线程发信号
    int err = pthread_cond_broadcast(&m_pthreadCond);
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::StopAll() 中 pthread_cond_broadcast() 失败, 返回的错误码为%d!", err);
        return;
    }

    // (2) 调用pthread_join()来等待所有线程返回
    std::vector<ThreadItem *>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL);
    }

    pthread_mutex_destroy(&m_pthreadMutex);
    pthread_cond_destroy(&m_pthreadCond);

    // (3) 释放(delete)线程池中的线程
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
        {
            delete *iter;
        }
    }
    m_threadVector.clear();

    ngx_log_stderr(0, "CThreadPool::StopAll() 成功返回, 线程池中线程全部正常结束.");
    return;
}

// 描述: 收到一个完整消息后入消息队列, 并触发线程池中线程来处理该消息.
// 参数buf: 实质为 pConn->precvMemPointer, new出来的, 保存"消息体+包头+包体"
// (1) 把消息("消息体+包头+包体")入消息队列.
// (2) 调用 Call() 来激发一个线程干活
// 调用: CSocekt::ngx_wait_request_handler_proc_plast()
void CThreadPool::inMsgRecvQueueAndSignal(char *buf)
{
    // (1) 把消息("消息体+包头+包体")入消息队列
    int err = pthread_mutex_lock(&m_pthreadMutex); // 加锁
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::inMsgRecvQueueAndSignal()-pthread_mutex_lock() 失败, 返回的错误码为 [%d].", err);
    }

    m_MsgRecvQueue.push_back(buf); // 入消息队列

    err = pthread_mutex_unlock(&m_pthreadMutex); // 解锁
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::inMsgRecvQueueAndSignal()-pthread_mutex_unlock() 失败, 返回的错误码为 [%d].", err);
    }

    // (2) 调用Call()来激发一个线程干活
    Call();

    return;
}

// 描述: 来任务了, 取线程池中的一个线程去干活
void CThreadPool::Call()
{
    int err = pthread_cond_signal(&m_pthreadCond); // 唤醒至少一个卡在 pthread_cond_wait() 的线程, 唤醒丢失怎么处理?
    if (err != 0)
    {
        ngx_log_stderr(err, "CThreadPool::Call()中pthread_cond_signal()失败，返回的错误码为%d!", err);
    }

    // 查看线程是否不够用
    if (m_iThreadNum == m_iRunningThreadNum)
    {
        time_t currtime = time(NULL);
        if (currtime - m_iLastEmgTime > 10) // 两次报告之间的间隔必须超过10秒, 防止日志输出的太频繁
        {
            m_iLastEmgTime = currtime; // 更新时间
            ngx_log_stderr(0, "CThreadPool::Call() 中发现线程池中当前空闲线程数量为0, 要考虑扩容线程池了.");
        }
    }

    return;
}

// 参考信号量解决方案：https://blog.csdn.net/yusiguyuan/article/details/20215591  linux多线程编程--信号量和条件变量 唤醒丢失事件
