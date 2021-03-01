
#ifndef __NGX_CONF_H__
#define __NGX_CONF_H__

#include <vector>
#include "ngx_global.h"

// 读取配置文件nginx.conf
class CConfig
{
private:
	CConfig() = default;
	CConfig(const CConfig &) = delete;
	CConfig &operator=(const CConfig &) = delete;

public:
	~CConfig();

private:
	static CConfig *m_instance;

public:
	static CConfig *GetInstance()
	{
		if (m_instance == NULL)
		{
			// 锁
			if (m_instance == NULL)
			{
				m_instance = new CConfig();
				static CGarhuishou cl;
			}
			// 放锁
		}
		return m_instance;
	}

	class CGarhuishou // 用于释放对象
	{
	public:
		~CGarhuishou()
		{
			if (CConfig::m_instance)
			{
				delete CConfig::m_instance;
				CConfig::m_instance = NULL;
			}
		}
	};

public:
	bool Load(const char *pconfName);
	const char *GetString(const char *p_itemname);
	int GetIntDefault(const char *p_itemname, const int def);

public:
	std::vector<LPCConfItem> m_ConfigItemList; // 存储配置信息的列表
};

#endif
