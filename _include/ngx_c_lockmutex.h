
#ifndef __NGX_LOCKMUTEX_H__
#define __NGX_LOCKMUTEX_H__

#include <pthread.h>

// 同 lock_graud()
class CLock
{
public:
	CLock(pthread_mutex_t *pMutex)
	{
		m_pMutex = pMutex;
		pthread_mutex_lock(m_pMutex); // 加锁
	}
	~CLock()
	{
		pthread_mutex_unlock(m_pMutex); //解锁
	}

private:
	pthread_mutex_t *m_pMutex;
};

#endif
