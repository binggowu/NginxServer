#ifndef __NGX_THREADPOOL_H__
#define __NGX_THREADPOOL_H__

#include <vector>
#include <pthread.h>
#include <atomic>

// 线程池相关类
class CThreadPool
{
public:
    CThreadPool();
    ~CThreadPool();

public:
    bool Create(int threadNum); // 创建该线程池中的所有线程
    void StopAll();             // 使线程池中的所有线程退出

    void inMsgRecvQueueAndSignal(char *buf);
    void Call();

    int getRecvMsgQueueCount() // 获取接收消息队列大小
    {
        return m_MsgRecvQueue.size();
    }

private:
    static void *ThreadFunc(void *threadData); // 新线程的线程回调函数

    void clearMsgRecvQueue(); // 清理 收消息队列

private:
    // 线程池中的线程结构体, CSocekt类中也有这个ThreadItem, 完全一样.
    struct ThreadItem
    {
        pthread_t _Handle;   // 线程句柄
        CThreadPool *_pThis; // 线程池的指针
        bool ifrunning;      // 线程是否启动起来(只有线程运行到pthread_cond_wait()时, 线程才算启动起来), 启动起来才允许调用StopAll()来释放. 如果线程刚刚Create()就StopAll()可能会报错, 所以引入 ifrunning 标识.

        ThreadItem(CThreadPool *pthis) : _pThis(pthis), ifrunning(false) {}
    };

private:
    static pthread_mutex_t m_pthreadMutex; // 线程同步 互斥量
    static pthread_cond_t m_pthreadCond;   // 线程同步 条件变量

    static bool m_shutdown; // 线程退出标志, false不退出, true退出. 初始值为false, 在 StopAll() 中设置为true.

    std::vector<ThreadItem *> m_threadVector; // 线程池
    int m_iThreadNum;                         // 线程池 大小

    std::atomic<int> m_iRunningThreadNum; // 正在处理任务的线程数量, 即不再被pthread_cond_wait()卡住的, 从 收消息队列 中取到消息的线程数量.

    time_t m_iLastEmgTime; // 上次发生线程不够用的时间(紧急事件), 防止日志输出的太频繁

    std::list<char *> m_MsgRecvQueue; // 收消息队列
};

#endif
