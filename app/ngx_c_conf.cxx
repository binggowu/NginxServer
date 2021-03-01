#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

// --------------------------
// 配置文件 相关的
// --------------------------

// 自定义头文件放下边, 因为g++中用了-I参数，所以这里用<>也可以
#include "ngx_func.h"   //函数声明
#include "ngx_c_conf.h" //和配置文件处理相关的类,名字带c_表示和类有关

// 静态成员赋值
CConfig *CConfig::m_instance = NULL;

// 析构函数
CConfig::~CConfig()
{
    std::vector<LPCConfItem>::iterator pos;
    for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        delete (*pos);
    }
    m_ConfigItemList.clear();
    return;
}

// 装载配置文件(nginx.conf)
bool CConfig::Load(const char *pconfName)
{
    FILE *fp;
    fp = fopen(pconfName, "r");
    if (fp == NULL)
    {
        return false;
    }

    // 每行配置都不要太长, 保持<500字符内, 防止出现问题
    char linebuf[501];

    while (!feof(fp))
    {
        if (fgets(linebuf, 500, fp) == NULL)
            continue;

        if (linebuf[0] == 0)
            continue;

        // 处理注释行
        if (*linebuf == ';' || *linebuf == ' ' || *linebuf == '#' || *linebuf == '\t' || *linebuf == '\n' || *linebuf == '[')
            continue;

        // 屁股后边若有 换行, 回车, 空格 等都截取掉
        while (strlen(linebuf) > 0)
        {
            if (linebuf[strlen(linebuf) - 1] == 10 || linebuf[strlen(linebuf) - 1] == 13 || linebuf[strlen(linebuf) - 1] == 32)
                linebuf[strlen(linebuf) - 1] = 0;
            else
                break;
        }

        // 如果linebuf就是'\n', 经过上一步处理后就是0
        if (linebuf[0] == 0)
            continue;

        // 装载配置项到m_ConfigItemList中去
        char *ptmp = strchr(linebuf, '=');
        if (ptmp != NULL)
        {
            LPCConfItem p_confitem = new CConfItem; // 在CConfig的析构函数中 delete
            memset(p_confitem, 0, sizeof(CConfItem));
            strncpy(p_confitem->ItemName, linebuf, (int)(ptmp - linebuf));
            strcpy(p_confitem->ItemContent, ptmp + 1);

            Rtrim(p_confitem->ItemName);
            Ltrim(p_confitem->ItemName);
            Rtrim(p_confitem->ItemContent);
            Ltrim(p_confitem->ItemContent);

            printf("%s=%s\n", p_confitem->ItemName, p_confitem->ItemContent);

            m_ConfigItemList.push_back(p_confitem);
        }

    } // while(!feof(fp))

    fclose(fp);

    return true;
}

// 根据ItemName获取配置信息字符串，不修改不用互斥
const char *CConfig::GetString(const char *p_itemname)
{
    std::vector<LPCConfItem>::iterator pos;
    for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        if (strcasecmp((*pos)->ItemName, p_itemname) == 0)
            return (*pos)->ItemContent;
    }
    return NULL;
}

// 根据ItemName获取数字类型配置信息，不修改不用互斥
int CConfig::GetIntDefault(const char *p_itemname, const int def)
{
    std::vector<LPCConfItem>::iterator pos;
    for (pos = m_ConfigItemList.begin(); pos != m_ConfigItemList.end(); ++pos)
    {
        if (strcasecmp((*pos)->ItemName, p_itemname) == 0)
            return atoi((*pos)->ItemContent);
    }
    return def;
}
