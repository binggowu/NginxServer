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

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_socket.h"
#include "ngx_c_memory.h"
#include "ngx_c_lockmutex.h"

// ---------------------
// 和网络 有关的函数放这里
// ---------------------

// 构造函数
CSocekt::CSocekt()
{
    // 配置相关
    m_worker_connections = 1;      // epoll连接最大项数
    m_ListenPortCount = 1;         // 监听一个端口
    m_RecyConnectionWaitTime = 60; // 等待这么些秒后才回收连接

    // epoll相关
    m_epollhandle = -1; //epoll返回的句柄
    //m_pconnections = NULL;       //连接池【连接数组】先给空
    //m_pfree_connections = NULL;  //连接池中空闲的连接链
    //m_pread_events = NULL;       //读事件数组给空
    //m_pwrite_events = NULL;      //写事件数组给空

    // 一些和网络通讯有关的常用变量值，供后续频繁使用时提高效率
    m_iLenPkgHeader = sizeof(COMM_PKG_HEADER);  //包头的sizeof值【占用的字节数】
    m_iLenMsgHeader = sizeof(STRUC_MSG_HEADER); //消息头的sizeof值【占用的字节数】

    // 多线程相关
    //pthread_mutex_init(&m_recvMessageQueueMutex, NULL); //互斥量初始化

    // 各种队列相关
    m_iSendMsgQueueCount = 0;     //发消息队列大小
    m_total_recyconnection_n = 0; //待释放连接队列大小
    m_cur_size_ = 0;              //当前计时队列尺寸
    m_timer_value_ = 0;           //当前计时队列头部的时间值
    m_iDiscardSendPkgCount = 0;   //丢弃的发送数据包数量

    // 在线用户相关
    m_onlineUserCount = 0; // 在线用户数量
    m_lastprintTime = 0;   // 上次打印统计信息的时间，先给0

    return;
}

// 1) 读配置项; 2) 打开监听端口.
// 调用: 被子类的Initialize()调用
bool CSocekt::Initialize()
{
    ReadConf();                                // 读配置项
    if (ngx_open_listening_sockets() == false) // 打开监听端口
    {
        return false;
    }
    return true;
}

// (1) 一些互斥量的初始化
// (2) 创建 发送数据 的线程
// (3) 创建 回收连接 的线程
// 调用: ngx_worker_process_init()
bool CSocekt::Initialize_subproc()
{
    // (1) 一些互斥量的初始化

    // 发消息 互斥量初始化
    if (pthread_mutex_init(&m_sendMessageQueueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_mutex_init(&m_sendMessageQueueMutex)失败.");
        return false;
    }

    // 连接相关 互斥量初始化
    if (pthread_mutex_init(&m_connectionMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_mutex_init(&m_connectionMutex)失败.");
        return false;
    }

    // 连接回收队列 相关互斥量初始化
    if (pthread_mutex_init(&m_recyconnqueueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_mutex_init(&m_recyconnqueueMutex)失败.");
        return false;
    }

    // 和时间处理队列 有关的互斥量初始化
    if (pthread_mutex_init(&m_timequeueMutex, NULL) != 0)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_mutex_init(&m_timequeueMutex)失败.");
        return false;
    }

    // 第2个参数0, 表示信号量在线程之间共享, 非0表示在进程之间共享
    // 第3个参数0, 表示信号量的初始值, 为0时, 调用sem_wait()就会卡在那里
    if (sem_init(&m_semEventSendQueue, 0, 0) == -1)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中sem_init(&m_semEventSendQueue,0,0)失败.");
        return false;
    }

    int err;

    // (2) 创建 发送数据 的线程
    ThreadItem *pSendQueue;
    m_threadVector.push_back(pSendQueue = new ThreadItem(this));
    err = pthread_create(&pSendQueue->_Handle, NULL, ServerSendQueueThread, pSendQueue);
    if (err != 0)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_create(ServerSendQueueThread)失败.");
        return false;
    }

    // (3) 创建 回收连接 的线程
    ThreadItem *pRecyconn;
    m_threadVector.push_back(pRecyconn = new ThreadItem(this));
    err = pthread_create(&pRecyconn->_Handle, NULL, ServerRecyConnectionThread, pRecyconn);
    if (err != 0)
    {
        ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_create(ServerRecyConnectionThread)失败.");
        return false;
    }

    // (4) 时间队列监视和处理 线程
    if (m_ifkickTimeCount == 1)
    {
        ThreadItem *pTimemonitor;
        m_threadVector.push_back(pTimemonitor = new ThreadItem(this));
        err = pthread_create(&pTimemonitor->_Handle, NULL, ServerTimerQueueMonitorThread, pTimemonitor);
        if (err != 0)
        {
            ngx_log_stderr(0, "CSocekt::Initialize_subproc()中pthread_create(ServerTimerQueueMonitorThread)失败.");
            return false;
        }
    }

    return true;
}

//释放函数
CSocekt::~CSocekt()
{
    // 释放必须的内存
    // 监听端口相关内存的释放
    std::vector<lpngx_listening_t>::iterator pos;
    for (pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        delete (*pos);
    }
    m_ListenSocketList.clear();

    return;
}

// 释放资源: 线程对象, 互斥资源.
void CSocekt::Shutdown_subproc()
{
    // 把干活的线程停止掉, 注意系统应该尝试通过设置 g_stopEvent = 1来 开始让整个项目停止

    // 用到信号量的, 可能还需要调用一下sem_post
    if (sem_post(&m_semEventSendQueue) == -1) // 让ServerSendQueueThread()流程走下来干活
    {
        ngx_log_stderr(0, "CSocekt::Shutdown_subproc()中sem_post(&m_semEventSendQueue)失败.");
    }

    // (2) 释放线程对象
    std::vector<ThreadItem *>::iterator iter;
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        pthread_join((*iter)->_Handle, NULL); //等待一个线程终止
    }
    for (iter = m_threadVector.begin(); iter != m_threadVector.end(); iter++)
    {
        if (*iter)
            delete *iter;
    }
    m_threadVector.clear();

    // (3) 队列清空
    clearMsgSendQueue();
    clearconnection();
    clearAllFromTimerQueue();

    // (4) 互斥资源的回收
    pthread_mutex_destroy(&m_connectionMutex);       //连接相关互斥量释放
    pthread_mutex_destroy(&m_sendMessageQueueMutex); //发消息互斥量释放
    pthread_mutex_destroy(&m_recyconnqueueMutex);    //连接回收队列相关的互斥量释放
    pthread_mutex_destroy(&m_timequeueMutex);        //时间处理队列相关的互斥量释放
    sem_destroy(&m_semEventSendQueue);               //发消息相关线程信号量释放
}

// 清理TCP发送消息队列
void CSocekt::clearMsgSendQueue()
{
    char *sTmpMempoint;
    CMemory *p_memory = CMemory::GetInstance();

    while (!m_MsgSendQueue.empty())
    {
        sTmpMempoint = m_MsgSendQueue.front();
        m_MsgSendQueue.pop_front();
        p_memory->FreeMemory(sTmpMempoint);
    }
}

// 专门用于读各种配置项
void CSocekt::ReadConf()
{
    CConfig *p_config = CConfig::GetInstance();
    m_worker_connections = p_config->GetIntDefault("worker_connections", m_worker_connections);
    m_ListenPortCount = p_config->GetIntDefault("ListenPortCount", m_ListenPortCount);
    m_RecyConnectionWaitTime = p_config->GetIntDefault("Sock_RecyConnectionWaitTime", m_RecyConnectionWaitTime); // 等待这么些秒后才回收连接

    m_ifkickTimeCount = p_config->GetIntDefault("Sock_WaitTimeEnable", 0);
    m_iWaitTime = p_config->GetIntDefault("Sock_MaxWaitTime", m_iWaitTime);
    m_iWaitTime = (m_iWaitTime > 5) ? m_iWaitTime : 5; // 不建议低于5秒钟, 无需太频繁

    m_ifTimeOutKick = p_config->GetIntDefault("Sock_TimeOutKick", 0);

    m_floodAkEnable = p_config->GetIntDefault("Sock_FloodAttackKickEnable", 0);   // Flood攻击检测是否开启, 1开启, 0不开启
    m_floodTimeInterval = p_config->GetIntDefault("Sock_FloodTimeInterval", 100); // 每次收到数据包的时间间隔(单位ms)
    m_floodKickCount = p_config->GetIntDefault("Sock_FloodKickCounter", 10);      // Sock_FloodTimeInterval 条件的累计次数

    return;
}

// 打开监听端口(支持多个端口), 在创建worker进程之前就要执行这个函数.
// 调用: CSocekt::Initialize()
bool CSocekt::ngx_open_listening_sockets()
{
    int lfd;
    struct sockaddr_in serv_addr; // 服务器地址
    int iport;                    // 端口
    char strinfo[100];            // 临时字符串

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听本地所有的IP地址

    CConfig *p_config = CConfig::GetInstance();
    for (int i = 0; i < m_ListenPortCount; i++)
    {
        lfd = socket(AF_INET, SOCK_STREAM, 0); //系统函数，成功返回非负描述符，出错返回-1
        if (lfd == -1)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中socket()失败,i=%d.", i);
            return false;
        }

        // SO_REUSEADDR: 允许单进程绑定同一个端口到多个socket上, 只要每次绑定到一个不同的IP地址即可.
        // 解决TIME_WAIT这个状态导致bind()失败的问题.
        int reuseaddr = 1; // 1: 打开对应的设置项
        if (setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&reuseaddr, sizeof(reuseaddr)) == -1)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中setsockopt(SO_REUSEADDR)失败,i=%d.", i);
            close(lfd);
            return false;
        }

        // SO_REUSEPORT: 允许完全重复的绑定, 要求在绑定同一个IP和端口的每个socket都设置了SO_REUSEPORT.
        // 为处理惊群问题使用 REUSEPORT
        int reuseport = 1;
        if (setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, (const void *)&reuseport, sizeof(int)) == -1) // 端口复用需要内核支持
        {
            // 失败顶多是惊群, 但程序依旧可以正常运行.
            ngx_log_stderr(errno, "CSocekt::Initialize()中setsockopt(SO_REUSEPORT)失败", i);
        }

        // 设置该socket为非阻塞
        if (setnonblocking(lfd) == false)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中setnonblocking()失败,i=%d.", i);
            close(lfd);
            return false;
        }

        // 设置本服务器要监听的地址和端口, 这样客户端才能连接到该地址和端口, 并发送数据.
        strinfo[0] = 0;
        sprintf(strinfo, "ListenPort%d", i);
        iport = p_config->GetIntDefault(strinfo, 10000);
        serv_addr.sin_port = htons((in_port_t)iport);

        if (bind(lfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中bind()失败,i=%d.", i);
            close(lfd);
            return false;
        }

        if (listen(lfd, NGX_LISTEN_BACKLOG) == -1)
        {
            ngx_log_stderr(errno, "CSocekt::Initialize()中listen()失败,i=%d.", i);
            close(lfd);
            return false;
        }

        lpngx_listening_t p_listensocketitem = new ngx_listening_t;
        memset(p_listensocketitem, 0, sizeof(ngx_listening_t));
        p_listensocketitem->port = iport;
        p_listensocketitem->fd = lfd;

        ngx_log_error_core(NGX_LOG_INFO, 0, "监听%d端口成功!", iport);
        m_ListenSocketList.push_back(p_listensocketitem);
    }

    if (m_ListenSocketList.size() <= 0)
        return false;
    return true;
}

// 设置socket连接为非阻塞模式
bool CSocekt::setnonblocking(int sockfd)
{
    int nb = 1;                            // 0清除, 1设置
    if (ioctl(sockfd, FIONBIO, &nb) == -1) // FIONBIO: 设置/清除非阻塞I/O标记
    {
        return false;
    }
    return true;
}

//关闭socket，什么时候用，我们现在先不确定，先把这个函数预备在这里
void CSocekt::ngx_close_listening_sockets()
{
    for (int i = 0; i < m_ListenPortCount; i++) //要关闭这么多个监听端口
    {
        //ngx_log_stderr(0,"端口是%d,socketid是%d.",m_ListenSocketList[i]->port,m_ListenSocketList[i]->fd);
        close(m_ListenSocketList[i]->fd);
        ngx_log_error_core(NGX_LOG_INFO, 0, "关闭监听端口%d!", m_ListenSocketList[i]->port); //显示一些信息到日志中
    }                                                                                        //end for(int i = 0; i < m_ListenPortCount; i++)
    return;
}

// 将一个待发送消息入到 发消息队列 中
// 1) 判断总发消息队列大小;
// 2) 判断当前client在消息队列中消息的数目;
// 3) 增加信号量
void CSocekt::msgSend(char *psendbuf)
{
    CMemory *p_memory = CMemory::GetInstance();

    CLock lock(&m_sendMessageQueueMutex); // 开始操作队列, 需要加锁

    // 发消息队列 过大
    // 如客户端恶意不接受数据, 就会导致这个队列越来越大. 为了服务器安全, 干掉(free)一些数据的发送, 虽然有可能导致客户端出现问题, 但总比服务器不稳定要好很多
    if (m_iSendMsgQueueCount > 50000)
    {
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        return;
    }

    // 总体数据并无风险, 不会导致服务器崩溃, 要看看个体数据, 找一下恶意者了
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)psendbuf;
    lpngx_connection_t p_Conn = pMsgHeader->pConn;
    if (p_Conn->iSendCount > 400)
    {
        // 该用户收消息太慢, 或者干脆不收消息(恶意), 该用户的 发送队列 中有的数据条目数过大, 认为是恶意用户, 直接切断
        ngx_log_stderr(0, "CSocekt::msgSend()中发现某用户 %d 积压了大量待发送数据包, 切断与他的连接!", p_Conn->fd);
        m_iDiscardSendPkgCount++;
        p_memory->FreeMemory(psendbuf);
        zdClosesocketProc(p_Conn); // 直接关闭
        return;
    }

    ++p_Conn->iSendCount; // 发消息队列 中有的数据条目数+1
    m_MsgSendQueue.push_back(psendbuf);
    ++m_iSendMsgQueueCount; // 原子操作, 而 m_iSendMsgQueueCount = m_iSendMsgQueueCount + 1 不是原子操作.

    // 将信号量的值+1, 这样其他卡在 sem_wait 的就可以走下去
    if (sem_post(&m_semEventSendQueue) == -1) // 让 ServerSendQueueThread() 流程走下来干活
    {
        ngx_log_stderr(0, "CSocekt::msgSend()中sem_post(&m_semEventSendQueue)失败.");
    }
    return;
}

// 主动关闭一个连接时的要做些善后的处理函数. 这个函数是可能被多线程调用的, 但不影响本服务器程序的稳定性和正确运行性
// (1) 从时间队列中删除
// (2) 关闭cfd
// (3) 延迟回收
// 调用: CSocekt::msgSend(), CLogicSocket::procPingTimeOutChecking(), CSocekt::ngx_read_request_handler(), CSocekt::recvproc().
void CSocekt::zdClosesocketProc(lpngx_connection_t p_Conn)
{
    if (m_ifkickTimeCount == 1)
    {
        DeleteFromTimerQueue(p_Conn); // 从 时间队列 中把连接干掉
    }

    // 如下代码是没有加锁的, 所以有可能fd被关闭了2次
    if (p_Conn->fd != -1)
    {
        close(p_Conn->fd); // 这个socket关闭, 关闭后就会被从epoll红黑树中删除, 所以这之后无法收到任何epoll事件
        p_Conn->fd = -1;
    }

    // 如下代码是没有加锁的, 所以有可能 iThrowsendCount 减到-1
    if (p_Conn->iThrowsendCount > 0)
    {
        --p_Conn->iThrowsendCount; // 归 0
    }

    inRecyConnectQueue(p_Conn);
    return;
}

// 测试flood攻击是否成立, 成立返回true, 否则返回false
bool CSocekt::TestFlood(lpngx_connection_t pConn)
{
    struct timeval sCurrTime; // 当前时间
    uint64_t iCurrTime;       // 当前时间的毫秒表示
    bool reco = false;

    gettimeofday(&sCurrTime, NULL);                                   // 取得当前时间
    iCurrTime = (sCurrTime.tv_sec * 1000 + sCurrTime.tv_usec / 1000); // 转化为毫秒表示
    if ((iCurrTime - pConn->FloodkickLastTime) < m_floodTimeInterval) // 两次收到包的时间 < 100毫秒
    {
        // 发包太频繁记录
        pConn->FloodAttackCount++;
        pConn->FloodkickLastTime = iCurrTime;
    }
    else
    {
        // 既然发布不这么频繁，则恢复计数值
        pConn->FloodAttackCount = 0;
        pConn->FloodkickLastTime = iCurrTime;
    }

    // ngx_log_stderr(0,"pConn->FloodAttackCount=%d,m_floodKickCount=%d.",pConn->FloodAttackCount,m_floodKickCount);

    if (pConn->FloodAttackCount >= m_floodKickCount)
    {
        //可以踢此人的标志
        reco = true;
    }
    return reco;
}

// 打印统计信息
void CSocekt::printTDInfo()
{
    time_t currtime = time(NULL);
    if ((currtime - m_lastprintTime) > 10) // 超过10秒我们打印一次
    {
        int tmprmqc = g_threadpool.getRecvMsgQueueCount(); // 收消息队列
        m_lastprintTime = currtime;
        int tmpoLUC = m_onlineUserCount;    // 直接打印atomic类型报错
        int tmpsmqc = m_iSendMsgQueueCount; // 直接打印atomic类型报错
        ngx_log_stderr(0, "------------------------------------begin--------------------------------------");
        ngx_log_stderr(0, "当前在线人数 / 总人数: (%d/%d).", tmpoLUC, m_worker_connections);
        ngx_log_stderr(0, "连接池中空闲连接 / 总连接 / 要释放的连接: (%d/%d/%d).", m_freeconnectionList.size(), m_connectionList.size(), m_recyconnectionList.size());
        ngx_log_stderr(0, "当前时间队列大小: (%d).", m_timerQueuemap.size());
        ngx_log_stderr(0, "当前收消息队列 / 发消息队列大小分别为: (%d/%d), 丢弃的待发送数据包数量为%d.", tmprmqc, tmpsmqc, m_iDiscardSendPkgCount);
        if (tmprmqc > 100000) // 收消息队列过大, 报一下, 这个属于应该 引起警觉的, 考虑限速等等手段
        {
            ngx_log_stderr(0, "接收队列条目数量过大(%d), 要考虑限速或者增加处理线程数量了.", tmprmqc);
        }
        ngx_log_stderr(0, "-------------------------------------end---------------------------------------");
    }
    return;
}

// (1) 创建epoll树, 创建连接池(initconnection)
// (3) 为每一个监听端口(ngx_listening_t)创建连接(ngx_connection_t).
// (4) 将lfd上epoll树(ngx_epoll_oper_event).
// 调用: ngx_worker_process_init()
int CSocekt::ngx_epoll_init()
{
    // (1) 创建epoll树
    m_epollhandle = epoll_create(m_worker_connections);
    if (m_epollhandle == -1)
    {
        ngx_log_stderr(errno, "CSocekt::ngx_epoll_init() 中 epoll_create() 失败.");
        exit(2); // 直接退, 资源由系统释放.
    }

    // (2) 创建连接池(initconnection)
    initconnection();

    // (3) 为每一个监听端口(ngx_listening_t) 创建连接(ngx_get_connection).
    for (auto pos = m_ListenSocketList.begin(); pos != m_ListenSocketList.end(); ++pos)
    {
        lpngx_connection_t p_Conn = ngx_get_connection((*pos)->fd); // 从连接池中获取一个空闲连接对象
        if (p_Conn == NULL)
        {
            ngx_log_stderr(errno, "CSocekt::ngx_epoll_init()中ngx_get_connection()失败.");
            exit(2); // 刚开始连接池就为空, 致命问题, 直接退
        }

        p_Conn->listening = (*pos);  // 连接对象 和监听对象关联, 方便通过连接对象找监听对象
        (*pos)->connection = p_Conn; // 监听对象 和连接对象关联, 方便通过监听对象找连接对象

        // 对监听端口的读事件设置处理方法, 因为监听端口是用来等对方连接的发送三路握手的, 所以监听端口关心的就是读事件
        p_Conn->rhandler = &CSocekt::ngx_event_accept;

        // (4) 将lfd上epoll树(ngx_epoll_oper_event).
        // EPOLLRDHUP: TCP连接的远端关闭或者半关闭
        if (ngx_epoll_oper_event((*pos)->fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLRDHUP, 0, p_Conn) == -1)
        {
            exit(2);
        }
    }

    return 1;
}

// 主要是扩展了epoll_ctl()的能力.
// 参数:
//   eventtype: EPOLL_CTL_ADD, EPOLL_CTL_MOD, EPOLL_CTL_DEL
//   flag: EPOLLIN, EPOLLOUT, EPOLLRDHUP
//   bcaction: 用于补充 EPOLL_CTL_MOD 的动作, 0增加, 1去掉, 2完全覆盖
//   pConn: 连接, 在EPOLL_CTL_ADD时增加到红黑树中去, 将来epoll_wait时能取出来用
// 返回值: 成功1, 失败-1.
// 调用: CSocekt::ngx_epoll_init()[为lfd使用]
// 调用: CSocekt::ngx_event_accept()[为cfd使用]
// 调用: CSocekt::ngx_write_request_handler()[发送数据]
int CSocekt::ngx_epoll_oper_event(int fd, uint32_t eventtype, uint32_t flag, int bcaction, lpngx_connection_t pConn)
{
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));

    if (eventtype == EPOLL_CTL_ADD)
    {
        // ev.data.ptr = (void *)pConn;  // 不能放在这里, 理由见函数结尾处.
        ev.events = flag;
        pConn->events = flag;
    }
    else if (eventtype == EPOLL_CTL_MOD)
    {
        // 节点已经在红黑树中, 修改节点的事件信息
        ev.events = pConn->events; // 先把标记恢复回来
        if (bcaction == 0)
        {
            // 增加某个标记
            ev.events |= flag;
        }
        else if (bcaction == 1)
        {
            // 去掉某个标记
            ev.events &= ~flag;
        }
        else
        {
            // 完全覆盖某个标记
            ev.events = flag;
        }
        pConn->events = ev.events; // 记录该标记
    }
    else
    {
        // 删除红黑树中节点, 目前没这个需求, socket关闭这项会自动从红黑树移除, 所以将来再扩展
        return 1; // 先直接返回1表示成功
    }

    // 绑定ptr这个事, 只在EPOLL_CTL_ADD的时候做一次即可, 但是发现EPOLL_CTL_MOD似乎会破坏掉ev.data.ptr, 因此不管是EPOLL_CTL_ADD, 还是EPOLL_CTL_MOD, 都要重新赋值一次.
    // 找了下内核源码 SYSCALL_DEFINE4(epoll_ctl, int, epfd, int, op, int, fd, struct epoll_event __user *, event), 感觉真的会覆盖掉.
    // copy_from_user(&epds, event, sizeof(struct epoll_event))), 感觉这个内核处理这个事情太粗暴了.
    ev.data.ptr = (void *)pConn;

    if (epoll_ctl(m_epollhandle, eventtype, fd, &ev) == -1)
    {
        ngx_log_stderr(errno, "CSocekt::ngx_epoll_oper_event()中epoll_ctl(%d,%ud,%ud,%d)失败.", fd, eventtype, flag, bcaction);
        return -1;
    }
    return 1;
}

// 获取事件并处理
// 参数timer: epoll_wait()阻塞的时长, 单位毫秒, -1表示阻塞.
// 返回值: 1正常, 0错误, 一般不管是正常还是问题返回, 都应该保持进程继续运行.
// 调用: ngx_worker_process_cycle() -> ngx_process_events_and_timers()
int CSocekt::ngx_epoll_process_events(int timer)
{
    // 如果你等待的是一段时间, 并且超时了, 则返回0
    int events = epoll_wait(m_epollhandle, m_events, NGX_MAX_EVENTS, timer);
    if (events == -1)
    {
        if (errno == EINTR)
        {
            // EINTR错误的产生: 当阻塞于某个慢系统调用的一个进程捕获某个信号且相应信号处理函数返回时, 该系统调用可能返回一个EINTR错误.
            ngx_log_error_core(NGX_LOG_INFO, errno, "CSocekt::ngx_epoll_process_events()中epoll_wait()失败!");
            return 1; //正常返回
        }
        else
        {
            ngx_log_error_core(NGX_LOG_ALERT, errno, "CSocekt::ngx_epoll_process_events()中epoll_wait()失败!");
            return 0;
        }
    }

    // events为0, 如果timer>0表示超时, 返回1正常退出; 如果timer为-1, 阻塞等待竟然events为0, 不正常, 记录日志并返回0.
    if (events == 0)
    {
        if (timer != -1)
        {
            // 超时
            return 1;
        }

        // 很奇怪的错误
        ngx_log_error_core(NGX_LOG_ALERT, 0, "CSocekt::ngx_epoll_process_events()中epoll_wait()永久阻塞却没返回任何事件.");
        return 0;
    }

    lpngx_connection_t p_Conn;
    uint32_t revents; // 事件类型, 如EPOLLIN, EPOLLOUT等.
    for (int i = 0; i < events; ++i)
    {
        p_Conn = (lpngx_connection_t)(m_events[i].data.ptr);

        /*
        instance = (uintptr_t) c & 1;                             //将地址的最后一位取出来，用instance变量标识, 见ngx_epoll_add_event，该值是当时随着连接池中的连接一起给进来的
                                                                  //取得的是你当时调用ngx_epoll_add_event()的时候，这个连接里边的instance变量的值；
        p_Conn = (lpngx_connection_t) ((uintptr_t)p_Conn & (uintptr_t) ~1); //最后1位干掉，得到真正的c地址

        //仔细分析一下官方nginx的这个判断
        //过滤过期事件的；
        if(c->fd == -1)  //一个套接字，当关联一个 连接池中的连接【对象】时，这个套接字值是要给到c->fd的，
                           //那什么时候这个c->fd会变成-1呢？关闭连接时这个fd会被设置为-1，哪行代码设置的-1再研究，但应该不是ngx_free_connection()函数设置的-1
        {                 
            //比如我们用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个连接关闭，那我们应该会把c->fd设置为-1；
            //第二个事件照常处理
            //第三个事件，假如这第三个事件，也跟第一个事件对应的是同一个连接，那这个条件就会成立；那么这种事件，属于过期事件，不该处理

            //这里可以增加个日志，也可以不增加日志
            ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocekt::ngx_epoll_process_events()中遇到了fd=-1的过期事件:%p.",c); 
            continue; //这种事件就不处理即可
        }

        //过滤过期事件的；
        if(c->instance != instance)
        {
            //--------------------以下这些说法来自于资料--------------------------------------
            //什么时候这个条件成立呢？【换种问法：instance标志为什么可以判断事件是否过期呢？】
            //比如我们用epoll_wait取得三个事件，处理第一个事件时，因为业务需要，我们把这个连接关闭【麻烦就麻烦在这个连接被服务器关闭上了】，但是恰好第三个事件也跟这个连接有关；
            //因为第一个事件就把socket连接关闭了，显然第三个事件我们是不应该处理的【因为这是个过期事件】，若处理肯定会导致错误；
            //那我们上述把c->fd设置为-1，可以解决这个问题吗？ 能解决一部分问题，但另外一部分不能解决，不能解决的问题描述如下【这么离奇的情况应该极少遇到】：

            //a)处理第一个事件时，因为业务需要，我们把这个连接【假设套接字为50】关闭，同时设置c->fd = -1;并且调用ngx_free_connection将该连接归还给连接池；
            //b)处理第二个事件，恰好第二个事件是建立新连接事件，调用ngx_get_connection从连接池中取出的连接非常可能就是刚刚释放的第一个事件对应的连接池中的连接；
            //c)又因为a中套接字50被释放了，所以会被操作系统拿来复用，复用给了b)【一般这么快就被复用也是醉了】；
            //d)当处理第三个事件时，第三个事件其实是已经过期的，应该不处理，那怎么判断这第三个事件是过期的呢？ 【假设现在处理的是第三个事件，此时这个 连接池中的该连接 实际上已经被用作第二个事件对应的socket上了】；
                //依靠instance标志位能够解决这个问题，当调用ngx_get_connection从连接池中获取一个新连接时，我们把instance标志位置反，所以这个条件如果不成立，说明这个连接已经被挪作他用了；

            //--------------------我的个人思考--------------------------------------
            //如果收到了若干个事件，其中连接关闭也搞了多次，导致这个instance标志位被取反2次，那么，造成的结果就是：还是有可能遇到某些过期事件没有被发现【这里也就没有被continue】，照旧被当做没过期事件处理了；
                  //如果是这样，那就只能被照旧处理了。可能会造成偶尔某个连接被误关闭？但是整体服务器程序运行应该是平稳，问题不大的，这种漏网而被当成没过期来处理的的过期事件应该是极少发生的

            ngx_log_error_core(NGX_LOG_DEBUG,0,"CSocekt::ngx_epoll_process_events()中遇到了instance值改变的过期事件:%p.",c); 
            continue; //这种事件就不处理即可
        }
        //存在一种可能性，过期事件没被过滤完整【非常极端】，走下来的；
        */

        // 能走到这里, 我们认为这些事件都没过期, 就正常开始处理
        revents = m_events[i].events;

        /*
        if(revents & (EPOLLERR|EPOLLHUP)) //例如对方close掉套接字，这里会感应到【换句话说：如果发生了错误或者客户端断连】
        {
            //这加上读写标记，方便后续代码处理，至于怎么处理，后续再说，这里也是参照nginx官方代码引入的这段代码；
            //官方说法：if the error events were returned, add EPOLLIN and EPOLLOUT，to handle the events at least in one active handler
            //我认为官方也是经过反复思考才加上着东西的，先放这里放着吧； 
            revents |= EPOLLIN|EPOLLOUT;   //EPOLLIN：表示对应的链接上有数据可以读出（TCP链接的远端主动关闭连接，也相当于可读事件，因为本服务器小处理发送来的FIN包）
                                           //EPOLLOUT：表示对应的连接上可以写入数据发送【写准备好】            
        } */

        if (revents & EPOLLIN)
        {
            // c->r_ready = 1;                         // 标记可以读；【从连接池拿出一个连接时这个连接的所有成员都是0】

            (this->*(p_Conn->rhandler))(p_Conn); // 注意括号的运用来正确设置优先级, 防止编译出错.
                                                 // 如果是有新连接进入, 这里执行的应该是 CSocekt::ngx_event_accept()
                                                 // 如果是有发送数据接受, 则这里执行的应该是 CSocekt::ngx_read_request_handler()
        }

        if (revents & EPOLLOUT) // 如果是写事件, 对方关闭连接也触发这个. 注意上边的 if(revents & (EPOLLERR|EPOLLHUP))  revents |= EPOLLIN|EPOLLOUT; 读写标记都给加上了
        {
            if (revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) // 客户端关闭, 如果服务器端挂着一个写通知事件, 则这里个条件是可能成立的
            {
                // EPOLLERR: 对应的连接发生错误                     8     = 1000
                // EPOLLHUP: 对应的连接被挂起                       16    = 0001 0000
                // EPOLLRDHUP: 表示TCP连接的远端关闭或者半关闭连接   8192    = 0010  0000   0000   0000
                // 8221 = ‭0010 0000 0001 1101‬ = EPOLLRDHUP|EPOLLHUP|EPOLLERR

                // 我们只有投递了 写事件，但对端断开时，程序流程才走到这里，投递了写事件意味着 iThrowsendCount标记肯定被+1了，这里我们减回
                --p_Conn->iThrowsendCount;
            }
            else
            {
                (this->*(p_Conn->whandler))(p_Conn); // CSocekt::ngx_write_request_handler()
            }
        }
    }
    return 1;
}

// 发送数据 的线程, 在CSocket对象的初始化中被创建.
// epoll + LT模式, 消息没发送完毕, socket会不停触发可写事件的处理.
void *CSocekt::ServerSendQueueThread(void *threadData)
{
    ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
    CSocekt *pSocketObj = pThread->_pThis;

    int err;
    std::list<char *>::iterator pos, pos2, posend;
    char *pMsgBuf;
    LPSTRUC_MSG_HEADER pMsgHeader;
    LPCOMM_PKG_HEADER pPkgHeader;
    lpngx_connection_t p_Conn;
    unsigned short itmp;
    ssize_t sendsize;

    CMemory *p_memory = CMemory::GetInstance();

    while (g_stopEvent == 0) // 不退出
    {
        // 在CSocekt::msgSend()调用sem_post()让这里sem_wait() 走下去. 整个程序退出之前, 也要sem_post()一下, 确保卡在sem_wait()的线程也能返回.
        if (sem_wait(&pSocketObj->m_semEventSendQueue) == -1)
        {
            if (errno != EINTR)
            {
                ngx_log_stderr(errno, "CSocekt::ServerSendQueueThread()中sem_wait(&pSocketObj->m_semEventSendQueue)失败.");
            }
        }

        if (g_stopEvent != 0)
        {
            break;
        }

        if (pSocketObj->m_iSendMsgQueueCount > 0)
        {
            // 从 发送消息队列 取
            err = pthread_mutex_lock(&pSocketObj->m_sendMessageQueueMutex);
            if (err != 0)
            {
                ngx_log_stderr(err, "CSocekt::ServerSendQueueThread()中pthread_mutex_lock()失败，返回的错误码为%d!", err);
            }

            pos = pSocketObj->m_MsgSendQueue.begin();
            posend = pSocketObj->m_MsgSendQueue.end();

            while (pos != posend)
            {
                pMsgBuf = (*pos); // 拿到的每个消息都是 消息头+包头+包体, 但要注意, 我们是不发送消息头给客户端的.

                pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                                // 消息头
                pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf + pSocketObj->m_iLenMsgHeader); // 包头
                p_Conn = pMsgHeader->pConn;

                // 包过期处理.
                // 包过期: 包中的iCurrsequence与连接中的iCurrsequence不同.
                // 如果该连接已经被回收, 比如在ngx_close_connection()/inRecyConnectQueue()中都会p_Conn->iCurrsequence++.
                // 如果不相等, 肯定是客户端连接已断, 要发送的数据肯定不需要发送了.
                // 1) 从发消息队列中移除; 2) 释放消息内存.
                if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
                {
                    pos2 = pos;
                    pos++;
                    pSocketObj->m_MsgSendQueue.erase(pos2);
                    --pSocketObj->m_iSendMsgQueueCount;
                    p_memory->FreeMemory(pMsgBuf);
                    continue;
                }

                // 发送缓冲区慢了, 认为客户端不收, 此时不再发送.
                if (p_Conn->iThrowsendCount > 0)
                {
                    pos++;
                    continue;
                }
                --p_Conn->iSendCount; // 发送队列 中有的数据条目数-1

                // 走到这里, 可以发送消息

                p_Conn->psendMemPointer = pMsgBuf; // 发送后释放用的, 因为这段内存是new出来的
                pos2 = pos;
                pos++;
                pSocketObj->m_MsgSendQueue.erase(pos2);
                --pSocketObj->m_iSendMsgQueueCount;

                p_Conn->psendbuf = (char *)pPkgHeader; // 要发送的数据的缓冲区指针, 因为发送数据不一定全部都能发送出去, 我们要记录数据发送到了哪里, 需要知道下次数据从哪里开始发送
                itmp = ntohs(pPkgHeader->pkgLen);      // 包头+包体 长度, 打包时用了htons
                p_Conn->isendlen = itmp;               // 要发送多少数据, 因为发送数据不一定全部都能发送出去, 我们需要知道剩余有多少数据还没发送

                // 直接发送数据
                sendsize = pSocketObj->sendproc(p_Conn, p_Conn->psendbuf, p_Conn->isendlen);
                if (sendsize > 0)
                {
                    if (sendsize == p_Conn->isendlen) // 全部发送完毕.
                    {
                        p_memory->FreeMemory(p_Conn->psendMemPointer);
                        p_Conn->psendMemPointer = NULL;
                        p_Conn->iThrowsendCount = 0;                                                 // 这行其实可以没有, 因此此时此刻这东西就是=0的
                        ngx_log_stderr(0, "CSocekt::ServerSendQueueThread()中数据发送完毕, 很好. "); // 提示, 商用时可以干掉
                    }
                    else // 只发送了一部分数据, 肯定是发送缓冲区满了, 现在要依靠epoll驱动, 调用ngx_write_request_handler()函数发送剩余数据
                    {
                        p_Conn->psendbuf = p_Conn->psendbuf + sendsize;
                        p_Conn->isendlen = p_Conn->isendlen - sendsize;
                        ++p_Conn->iThrowsendCount; // 标记发送缓冲区满了

                        // 修改为EPOLLOUT事件
                        if (pSocketObj->ngx_epoll_oper_event(
                                p_Conn->fd,
                                EPOLL_CTL_MOD,
                                EPOLLOUT,
                                0,
                                p_Conn) == -1)
                        {
                            ngx_log_stderr(errno, "CSocekt::ServerSendQueueThread()ngx_epoll_oper_event()失败.");
                        }

                        ngx_log_stderr(errno, "CSocekt::ServerSendQueueThread()中数据没发送完毕[发送缓冲区满], 整个要发送%d, 实际发送了%d.", p_Conn->isendlen, sendsize);
                    }
                    continue; // 继续处理其他消息
                }
                // 能走到这里, 应该是有点问题的
                else if (sendsize == 0)
                {
                    //发送0个字节，首先因为我发送的内容不是0个字节的；
                    //然后如果发送 缓冲区满则返回的应该是-1，而错误码应该是EAGAIN，所以我综合认为，这种情况我就把这个发送的包丢弃了【按对端关闭了socket处理】
                    //这个打印下日志，我还真想观察观察是否真有这种现象发生
                    //ngx_log_stderr(errno,"CSocekt::ServerSendQueueThread()中sendproc()居然返回0？"); //如果对方关闭连接出现send=0，那么这个日志可能会常出现，商用时就 应该干掉
                    //然后这个包干掉，不发送了
                    p_memory->FreeMemory(p_Conn->psendMemPointer); //释放内存
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0; //这行其实可以没有，因此此时此刻这东西就是=0的
                    continue;
                }
                // 能走到这里, 继续处理问题
                else if (sendsize == -1)
                {
                    // 一个字节都没发出去, 说明发送时缓冲区当前正好是满的
                    ++p_Conn->iThrowsendCount; // 标记发送缓冲区满了，需要通过epoll事件来驱动消息的继续发送
                    //投递此事件后，我们将依靠epoll驱动调用ngx_write_request_handler()函数发送数据
                    if (pSocketObj->ngx_epoll_oper_event(
                            p_Conn->fd,    //socket句柄
                            EPOLL_CTL_MOD, //事件类型，这里是增加【因为我们准备增加个写通知】
                            EPOLLOUT,      //标志，这里代表要增加的标志,EPOLLOUT：可写【可写的时候通知我】
                            0,             //对于事件类型为增加的，EPOLL_CTL_MOD需要这个参数, 0：增加   1：去掉 2：完全覆盖
                            p_Conn         //连接池中的连接
                            ) == -1)
                    {
                        ngx_log_stderr(errno, "CSocekt::ServerSendQueueThread()中ngx_epoll_add_event()_2失败.");
                    }
                    continue;
                }
                else
                {
                    // 能走到这里的, 应该就是返回值-2了, 一般就认为对端断开了, 等待recv()来做断开socket以及回收资源
                    p_memory->FreeMemory(p_Conn->psendMemPointer);
                    p_Conn->psendMemPointer = NULL;
                    p_Conn->iThrowsendCount = 0; // 这行其实可以没有, 因此此时此刻这东西就是=0的
                    continue;
                }

            } // while(pos != posend)

            err = pthread_mutex_unlock(&pSocketObj->m_sendMessageQueueMutex);
            if (err != 0)
                ngx_log_stderr(err, "CSocekt::ServerSendQueueThread()pthread_mutex_unlock()失败, 返回的错误码为%d!", err);
        }
    }

    return (void *)0;
}
