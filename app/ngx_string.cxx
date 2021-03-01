#include <stdio.h>
#include <string.h>

// -----------------
// 字符串处理相关
// -----------------

/* 
//截取字符串尾部空格
void Rtrim(char *string)
{
	size_t len = 0;
	if (string == NULL)
		return;

	len = strlen(string);
	while (len > 0 && string[len - 1] == ' ') //位置换一下
		string[--len] = 0;
	return;
} 
*/

/* 
//截取字符串首部空格
void Ltrim(char *string)
{
    size_t len = 0;
    len = strlen(string);
    char *p_tmp = string;
    if ((*p_tmp) != ' ') //不是以空格开头
        return;
    //找第一个不为空格的
    while ((*p_tmp) != '\0')
    {
        if ((*p_tmp) == ' ')
            p_tmp++;
        else
            break;
    }
    if ((*p_tmp) == '\0') //全是空格
    {
        *string = '\0';
        return;
    }
    char *p_tmp2 = string;
    while ((*p_tmp) != '\0')
    {
        (*p_tmp2) = (*p_tmp);
        p_tmp++;
        p_tmp2++;
    }
    (*p_tmp2) = '\0';
    return;
} 
*/

// 去除字符串右边的所有空格
void Rtrim(char *str)
{
    size_t len = 0;
    if (str == NULL)
        return;

    len = strlen(str);
    while (len > 0 && str[len - 1] == ' ')
        str[--len] = 0;
    return;
}

// 去除字符串左边的所有空格
void Ltrim(char *str)
{
    char *p_tmp = str;
    if ((*p_tmp) != ' ')
        return;

    // p_tmp指向第一个非空格字符或结尾
    while ((*p_tmp) != '\0')
    {
        if ((*p_tmp) != ' ')
            break;
        p_tmp++;
    }

    if ((*p_tmp) == '\0') // str全部为空格
    {
        *str = '\0';
        return;
    }

    char *p_tmp2 = str;
    while ((*p_tmp) != '\0')
        (*p_tmp2++) = (*p_tmp++);
    (*p_tmp2) = '\0';
    return;
}
