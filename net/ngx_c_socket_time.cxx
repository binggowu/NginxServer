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



/**********************************************
和定时器 有关的

供其他文件使用的只有: AddToTimerQueue(), DeleteFromTimerQueue(), clearAllFromTimerQueue().

 ***********************************************/

// 把用户连接建立(三次握手成功)的时间加入进 时间队列
// 调用: CSocekt::ngx_event_accept()
void CSocekt::AddToTimerQueue(lpngx_connection_t pConn)
{
	CMemory *p_memory = CMemory::GetInstance();

	time_t futtime = time(NULL);
	futtime += m_iWaitTime;

	CLock lock(&m_timequeueMutex); // 互斥, 因为要操作 m_timeQueuemap

	LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)p_memory->AllocMemory(m_iLenMsgHeader, false);
	tmpMsgHeader->pConn = pConn;
	tmpMsgHeader->iCurrsequence = pConn->iCurrsequence;
	m_timerQueuemap.insert(std::make_pair(futtime, tmpMsgHeader)); // 按key 自动排序 小->大(旧->新)
	m_cur_size_++;

	m_timer_value_ = GetEarliestTime();
	return;
}

// 从 时间队列 中取得最早的时间返回
// 注意: 调用者负责互斥, 调用者确保 m_timeQueuemap 不为空.
// 只被本文件的其他函数调用.
time_t CSocekt::GetEarliestTime()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;
	pos = m_timerQueuemap.begin();
	return pos->first;
}

// 从 时间队列 移除最早的(begin), 并返回.
// 调用者负责互斥.
// 调用: GetOverTimeTimer().
LPSTRUC_MSG_HEADER CSocekt::RemoveFirstTimer()
{
	if (m_cur_size_ <= 0)
		return NULL;

	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos;
	LPSTRUC_MSG_HEADER p_tmp;
	pos = m_timerQueuemap.begin(); // 调用者负责互斥的, 这里直接操作没问题的
	p_tmp = pos->second;
	m_timerQueuemap.erase(pos);
	--m_cur_size_;
	return p_tmp;
}

// 根据当前时间(参数), 从 时间队列 找到比这个时间更老的节点返回(只返回一个最老的), 这些节点都是时间超过了, 要处理的节点.
// 调用者负责互斥
// 调用: ServerTimerQueueMonitorThread()
LPSTRUC_MSG_HEADER CSocekt::GetOverTimeTimer(time_t cur_time)
{
	CMemory *p_memory = CMemory::GetInstance();
	LPSTRUC_MSG_HEADER ptmp;

	if (m_cur_size_ == 0 || m_timerQueuemap.empty())
		return NULL;

	time_t earliesttime = GetEarliestTime(); // 到multimap中去查询
	if (earliesttime <= cur_time)
	{
		// 这回确实是有到时间的了【超时的节点】
		ptmp = RemoveFirstTimer();

		if (m_ifTimeOutKick != 1)
		{
			// 如果不是要求超时就提出, 则才做这里的事.

			// 因为下次超时的时间我们也依然要判断, 所以还要把这个节点加回来
			time_t newinqueutime = cur_time + (m_iWaitTime);
			LPSTRUC_MSG_HEADER tmpMsgHeader = (LPSTRUC_MSG_HEADER)p_memory->AllocMemory(sizeof(STRUC_MSG_HEADER), false);
			tmpMsgHeader->pConn = ptmp->pConn;
			tmpMsgHeader->iCurrsequence = ptmp->iCurrsequence;
			m_timerQueuemap.insert(std::make_pair(newinqueutime, tmpMsgHeader)); // 自动排序 小->大
			m_cur_size_++;
		}

		if (m_cur_size_ > 0) // 这个判断条件必要, 因为以后我们可能在这里扩充别的代码
		{
			m_timer_value_ = GetEarliestTime(); // 计时队列头部时间值保存到m_timer_value_里
		}

		return ptmp;
	}
	return NULL;
}

// 把指定连接从 时间队列 中删除.
// 注意: 时间队列 中可能保存连接多次, 都要删除.
// 调用: CSocekt::zdClosesocketProc()
void CSocekt::DeleteFromTimerQueue(lpngx_connection_t pConn)
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos, posend;
	CMemory *p_memory = CMemory::GetInstance();

	CLock lock(&m_timequeueMutex);

	// 因为实际情况可能比较复杂, 将来可能还扩充代码等等, 所以如下我们遍历整个队列找一圈, 而不是找到一次就拉倒, 以免出现什么遗漏
lblMTQM:
	pos = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();
	for (; pos != posend; ++pos)
	{
		if (pos->second->pConn == pConn)
		{
			p_memory->FreeMemory(pos->second); // 释放内存
			m_timerQueuemap.erase(pos);		   // 删除节点
			--m_cur_size_;
			goto lblMTQM;
		}
	}

	if (m_cur_size_ > 0)
	{
		m_timer_value_ = GetEarliestTime();
	}
	return;
}

// 清理 时间队列 中所有内容
// 调用: CSocekt::Shutdown_subproc()
void CSocekt::clearAllFromTimerQueue()
{
	std::multimap<time_t, LPSTRUC_MSG_HEADER>::iterator pos, posend;

	CMemory *p_memory = CMemory::GetInstance();
	pos = m_timerQueuemap.begin();
	posend = m_timerQueuemap.end();
	for (; pos != posend; ++pos)
	{
		p_memory->FreeMemory(pos->second);
		--m_cur_size_;
	}
	m_timerQueuemap.clear();
}

// 时间队列监视和处理线程, 处理到期不发心跳包的用户, 踢出的线程
void *CSocekt::ServerTimerQueueMonitorThread(void *threadData)
{
	ThreadItem *pThread = static_cast<ThreadItem *>(threadData);
	CSocekt *pSocketObj = pThread->_pThis;

	time_t absolute_time, cur_time;
	int err;

	while (g_stopEvent == 0)
	{
		// 这里没互斥判断, 所以只是个初级判断, 目的至少是队列为空时避免系统损耗
		if (pSocketObj->m_cur_size_ > 0) // 队列不为空
		{
			absolute_time = pSocketObj->m_timer_value_; // 这个可是省了个互斥, 十分划算
			cur_time = time(NULL);
			if (absolute_time < cur_time) // 存在超时节点
			{
				std::list<LPSTRUC_MSG_HEADER> m_lsIdleList; // 所有超时节点
				LPSTRUC_MSG_HEADER result;

				// 加锁
				err = pthread_mutex_lock(&pSocketObj->m_timequeueMutex);
				if (err != 0)
					ngx_log_stderr(err, "CSocekt::ServerTimerQueueMonitorThread()中pthread_mutex_lock()失败，返回的错误码为%d!", err);

				// 一次性的把所有超时节点都拿过来
				while ((result = pSocketObj->GetOverTimeTimer(cur_time)) != NULL)
				{
					m_lsIdleList.push_back(result);
				}
				// 解锁
				err = pthread_mutex_unlock(&pSocketObj->m_timequeueMutex);
				if (err != 0)
					ngx_log_stderr(err, "CSocekt::ServerTimerQueueMonitorThread()pthread_mutex_unlock()失败，返回的错误码为%d!", err); // 有问题，要及时报告

				LPSTRUC_MSG_HEADER tmpmsg;
				while (!m_lsIdleList.empty())
				{
					tmpmsg = m_lsIdleList.front();
					m_lsIdleList.pop_front();
					pSocketObj->procPingTimeOutChecking(tmpmsg, cur_time); // 这里需要检查心跳超时问题
				}
			}
		}

		usleep(500 * 1000); // 为简化问题, 我们直接每次休息500毫秒
	}

	return (void *)0;
}

// 心跳包检测时间到, 该去检测心跳包是否超时的事宜,
// 本函数(父类)只是把内存释放, 子类应该重新实现该函数以实现具体的判断动作. 定义为纯虚函数是不是更好?
void CSocekt::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
	CMemory *p_memory = CMemory::GetInstance();
	p_memory->FreeMemory(tmpmsg);
}
