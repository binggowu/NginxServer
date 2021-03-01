#ifndef __NGX_C_CRC32_H__
#define __NGX_C_CRC32_H__

#include <stddef.h> // NULL

// 对收发的数据包进行一个简单的校验, 以确保数据包中的内容没有被篡改过.
class CCRC32
{
private:
	CCRC32();

public:
	~CCRC32();

private:
	static CCRC32 *m_instance;

public:
	static CCRC32 *GetInstance()
	{
		if (m_instance == NULL)
		{
			// 锁
			if (m_instance == NULL)
			{
				m_instance = new CCRC32();
				static CGarhuishou cl;
			}
			// 放锁
		}
		return m_instance;
	}

	class CGarhuishou
	{
	public:
		~CGarhuishou()
		{
			if (CCRC32::m_instance)
			{
				delete CCRC32::m_instance;
				CCRC32::m_instance = NULL;
			}
		}
	};

public:
	void Init_CRC32_Table();
	unsigned int Reflect(unsigned int ref, char ch);
	int Get_CRC(unsigned char *buffer, unsigned int dwSize);

public:
	unsigned int crc32_table[256]; // Lookup table arrays
};

#endif
