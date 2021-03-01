#ifndef __NGX_SOCKET_H__
#define __NGX_SOCKET_H__

#include <vector>
#include <list>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <pthread.h>
#include <semaphore.h>
#include <atomic>
#include <map>

#include "ngx_comm.h"

#define NGX_LISTEN_BACKLOG 511 // 已完成连接队列, nginx官方是511
#define NGX_MAX_EVENTS 512	   // epoll_wait()一次最多接收的事件个数, nginx官方是512

typedef struct ngx_listening_s ngx_listening_t, *lpngx_listening_t;
typedef struct ngx_connection_s ngx_connection_t, *lpngx_connection_t;
typedef class CSocekt CSocekt;

typedef void (CSocekt::*ngx_event_handler_pt)(lpngx_connection_t c); // 定义成员函数指针

// ------------------------------------------------ 监听端口 --------------------------------------------------

// 和 监听端口 有关的结构
struct ngx_listening_s
{
	int port;					   // 监听的端口号
	int fd;						   // 监听文件描述符(lfd)
	lpngx_connection_t connection; // 连接池中的一个连接
};

// -------------------------------------------------- 连接池 --------------------------------------------------

// 连接池 有关的结构
struct ngx_connection_s
{
	// 二段式构造和析构

	ngx_connection_s();
	void GetOneToUse();

	void PutOneToFree();
	virtual ~ngx_connection_s();

	// 和连接有关

	int fd;						 // socket
	lpngx_listening_t listening; // 针对fd为lfd
	struct sockaddr s_sockaddr;	 // 保存对方地址信息用的, 调用ngx_sock_ntop()可以转化为字符串

	// 和epoll事件有关

	ngx_event_handler_pt rhandler; // 读事件回调函数, 对于lfd就是CSocekt::ngx_event_accept, 对于cfd就是CSocekt::ngx_read_request_handler
	ngx_event_handler_pt whandler; // 写事件回调函数, 对于cfd是CSocekt::ngx_write_request_handler.
	uint32_t events;

	// 和收包有关

	unsigned char curStat;			   // 当前的收包状态, 有4种, 详见ngx_comm.h
	char dataHeadInfo[_DATA_BUFSIZE_]; // 保存收到的包头
	char *precvbuf;					   // 还要继续 接收数据缓冲区的头指针, 初始指向 dataHeadInfo 首地址
	unsigned int irecvlen;			   // 还要继续 收多少数据, 初始为 sizeof(COMM_PKG_HEADER)
	char *precvMemPointer;			   // new出来, 用于收包(消息体+包头+包体)的内存首地址
	// 解决收包不全的问题, 如包头8字节, 但目前只收到3字节, 此时irecvlen就是5, precvbuf指向dataHeadInfo中的第4个位置, 以便后续继续收包头.

	// 和发包有关

	std::atomic<int> iThrowsendCount; // 发送消息, 如果发送缓冲区满, 则用这个变量标记
	char *psendbuf;					  // 发送数据的缓冲区的头指针, 开始其实是包头+包体
	unsigned int isendlen;			  // 要发送多少数据
	char *psendMemPointer;			  // 发送完成后释放用的, 整个数据(消息体+包头+包体)的头指针

	// 和逻辑处理有关

	pthread_mutex_t logicPorcMutex;

	// 连接延迟回收

	time_t inRecyTime; // 入 待释放连接队列 的时间

	// 和心跳包有关

	time_t lastPingTime; // 上次ping的时间, 即上次发送心跳包的时间

	// 和网络安全有关

	uint64_t iCurrsequence; // 序号, 每次分配出去时+1, 可以用来检测错包/废包

	uint64_t FloodkickLastTime; // Flood攻击上次收到包的时间
	int FloodAttackCount;		// Flood攻击在该时间内收到包的次数统计

	std::atomic<int> iSendCount; // 当前client在 发消息队列 中消息的数目, 若client只发不收, 则可能造成此数过大, 依据此数做出踢出处理.

	// 连接池 有关

	lpngx_connection_t next; // 单向链表, 指向下一个节点, 用于把空闲的连接对象串起来

	// unsigned   instance:1;  // 位域, 失效标志位, 0有效, 1失效. 官方nginx提供, 到底有什么用, ngx_epoll_process_events()中详解.
	// char     addr_text[100]; // 地址的文本信息, 100足够, 一般其实如果是ipv4地址, 255.255.255.255, 其实只需要20字节就够
};

// ---------------------------------------------- 消息头 --------------------------------------------------

// 消息头, 引入的目的是当收到数据包时, 额外记录一些内容以备将来使用
typedef struct _STRUC_MSG_HEADER
{
	lpngx_connection_t pConn; // 对应的"连接"
	uint64_t iCurrsequence;	  // 收到数据包时, 对应连接(pConn)的序号, 将来能用于比较连接是否已经作废
} STRUC_MSG_HEADER, *LPSTRUC_MSG_HEADER;

// ---------------------------------------------- CSocekt --------------------------------------------------

class CSocekt
{
public:
	CSocekt();
	virtual ~CSocekt();
	virtual bool Initialize();		   // 初始化函数, 在父进程中执行
	virtual bool Initialize_subproc(); // 初始化函数, 在子进程中执行
	virtual void Shutdown_subproc();   // 关闭退出函数, 在子进程中执行
	void printTDInfo();				   // 打印统计信息

public:
	virtual void threadRecvProcFunc(char *pMsgBuf); // 处理客户端请求, 因为将来可以考虑自己来写子类继承本类
	virtual void procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time);

public:

	int ngx_epoll_init();
	int ngx_epoll_process_events(int timer);
	int ngx_epoll_oper_event(int fd, uint32_t eventtype, uint32_t flag, int bcaction, lpngx_connection_t pConn);


protected:

	// 数据发送相关

	void msgSend(char *psendbuf);
	void zdClosesocketProc(lpngx_connection_t p_Conn);

private:
	void ReadConf();					//专门用于读各种配置项
	bool ngx_open_listening_sockets();	//监听必须的端口【支持多个端口】
	void ngx_close_listening_sockets(); //关闭监听套接字
	bool setnonblocking(int sockfd);	//设置非阻塞套接字

	// 一些业务处理函数handler

	void ngx_event_accept(lpngx_connection_t oldc);			  // 建立新连接
	void ngx_read_request_handler(lpngx_connection_t pConn);  // 设置数据来时的读处理函数
	void ngx_write_request_handler(lpngx_connection_t pConn); // 设置数据发送时的写处理函数

	ssize_t recvproc(lpngx_connection_t pConn, char *buff, ssize_t buflen); //接收从客户端来的数据专用函数

	void ngx_wait_request_handler_proc_p1(lpngx_connection_t pConn, bool &isflood);

	// 包头收完整后的处理，我们称为包处理阶段1：写成函数，方便复用
	void ngx_wait_request_handler_proc_plast(lpngx_connection_t pConn, bool &isflood);

	// 收到一个完整包后的处理, 放到一个函数中, 方便调用

	void clearMsgSendQueue(); // 处理发送消息队列

	ssize_t sendproc(lpngx_connection_t c, char *buff, ssize_t size); //将数据发送到客户端

	size_t ngx_sock_ntop(struct sockaddr *sa, int port, u_char *text, size_t len);

	// 连接池 相关

	void initconnection();
	lpngx_connection_t ngx_get_connection(int isock);
	void clearconnection();
	void ngx_free_connection(lpngx_connection_t pConn);
	void inRecyConnectQueue(lpngx_connection_t pConn);	 // 将要回收的连接 入待释放连接队列
	void ngx_close_connection(lpngx_connection_t pConn); // 立刻回收, 用户刚刚连入时(CSocekt::ngx_event_accept)产生失败

	// 和时间相关的函数

	void AddToTimerQueue(lpngx_connection_t pConn);
	time_t GetEarliestTime();
	LPSTRUC_MSG_HEADER RemoveFirstTimer();
	LPSTRUC_MSG_HEADER GetOverTimeTimer(time_t cur_time);
	void DeleteFromTimerQueue(lpngx_connection_t pConn);
	void clearAllFromTimerQueue();

	// 和网络安全有关

	bool TestFlood(lpngx_connection_t pConn); // 测试flood攻击是否成立, 成立返回true, 否则返回false

	// 线程相关函数

	static void *ServerSendQueueThread(void *threadData);		  // 发送数据 的线程
	static void *ServerRecyConnectionThread(void *threadData);	  // 回收连接 的线程
	static void *ServerTimerQueueMonitorThread(void *threadData); // 时间队列监视线程, 处理到期不发心跳包的用户踢出的线程

protected:
	// 和网络通讯有关

	size_t m_iLenPkgHeader; // 包长, sizeof(COMM_PKG_HEADER);
	size_t m_iLenMsgHeader; // 消息体长, sizeof(STRUC_MSG_HEADER);

	// 时间相关

	int m_ifTimeOutKick; // 当时间到达 Sock_MaxWaitTime 指定的时间时, 直接把客户端踢出去, 只有当Sock_WaitTimeEnable = 1时, 本项才有用
	int m_iWaitTime;	 // 多少秒检测一次是否 心跳超时, 只有当Sock_WaitTimeEnable = 1时, 本项才有用

private:
	// CThreadPool类中也有这个ThreadItem, 完全一样.
	struct ThreadItem
	{
		pthread_t _Handle; // 线程句柄
		CSocekt *_pThis;   // 记录线程池的指针
		bool ifrunning;	   // 标记是否正式启动起来, 启动起来后, 才允许调用StopAll()来释放

		// 构造函数
		ThreadItem(CSocekt *pthis) : _pThis(pthis), ifrunning(false) {}
		// 析构函数
		~ThreadItem() {}
	};

	int m_worker_connections; // 每个 worker 进程允许同时连入的客户端数, 从nginx.conf中读取: Initialize() -> ReadConf()

	int m_ListenPortCount; // 要监听的端口数量, 从nginx.conf中读取: Initialize() -> ReadConf()

	int m_epollhandle;							 // epoll_create()返回的句柄
	struct epoll_event m_events[NGX_MAX_EVENTS]; // 用于在epoll_wait()中保存所发生的事件

	// 和连接池有关的

	std::list<lpngx_connection_t> m_connectionList;		// 连接池
	std::list<lpngx_connection_t> m_freeconnectionList; // 空闲连接列表

	// 连接池(m_connectionList/m_freeconnectionList) 相关互斥量
	pthread_mutex_t m_connectionMutex;

	// 连接回收

	std::list<lpngx_connection_t> m_recyconnectionList; // 待释放连接队列
	std::atomic<int> m_total_recyconnection_n;			// 待释放连接队列 大小
	pthread_mutex_t m_recyconnqueueMutex;				// 待释放连接队列 的互斥量
	int m_RecyConnectionWaitTime;						// 等待这么些秒后才回收连接

	std::vector<lpngx_listening_t> m_ListenSocketList; // 监听套接字队列

	// 消息队列

	std::list<char *> m_MsgSendQueue;		 // 发消息队列
	std::atomic<int> m_iSendMsgQueueCount;	 // 发消息队列 大小
	pthread_mutex_t m_sendMessageQueueMutex; // 发消息队列 互斥量

	// 多线程相关

	std::vector<ThreadItem *> m_threadVector; // 发包线程/连接的延迟回收线程/时间队列监视和处理线程
	sem_t m_semEventSendQueue;				  // 发包线程的信号量

	// 定时器相关

	pthread_mutex_t m_timequeueMutex;						   // 互斥量
	std::multimap<time_t, LPSTRUC_MSG_HEADER> m_timerQueuemap; // 时间队列, 保存用户连接建立(三次握手成功)的时间.
	size_t m_cur_size_;										   // 时间队列的大小
	time_t m_timer_value_;									   // 时间队列中最早的时间

	// 在线用户相关

	std::atomic<int> m_onlineUserCount; // 当前在线用户数

	// 网络安全相关

	int m_ifkickTimeCount;			  // 是否开启踢人时钟，1：开启   0：不开启
	int m_floodAkEnable;			  // Flood攻击检测是否开启, 1开启, 0不开启. 对应配置项 Sock_FloodAttackKickEnable
	unsigned int m_floodTimeInterval; // 每次收到数据包的时间间隔(单位ms). 对应的配置项 Sock_FloodTimeInterval
	int m_floodKickCount;			  // Sock_FloodTimeInterval 条件的累计次数. 对应的配置项 Sock_FloodKickCounter

	// 统计用途

	time_t m_lastprintTime;		// 上次打印统计信息的时间(10秒钟打印一次)
	int m_iDiscardSendPkgCount; // 丢弃的发送数据包数量
};

#endif
