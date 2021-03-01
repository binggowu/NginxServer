#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "ngx_global.h"

// ------------------
// 可执行程序标题
// ------------------

// 描述: 环境变量"搬家"
// (1) new 一段 g_envneedmem 这么大的新内存.
// (2) 把 environ[i] 的指针指向的内存拷贝到新内存中.
// (3) 赋予 environ[i] 新内存的地址.
void ngx_init_setproctitle()
{
    // (1) new 一段 g_envneedmem 这么大的新内存.
    gp_envmem = new char[g_envneedmem];
    memset(gp_envmem, 0, g_envneedmem);

    char *ptmp = gp_envmem;
    for (int i = 0; environ[i]; i++)
    {
        size_t size = strlen(environ[i]) + 1;
        strcpy(ptmp, environ[i]); // (2) 把 environ[i] 的指针指向的内存拷贝到新内存中.
        environ[i] = ptmp;        // (3) 赋予 environ[i] 新内存的地址.
        ptmp += size;
    }
    return;
}

// 描述: 设置可执行程序标题
// (1) 计算新标题长度 和 argv和environ内存总和.
// (2) 设置 argv[1] = NULL, 表示argv[]中只有一个元素了.
// (3) 把新标题拷贝进来, 此时原来的命令行参数都会被覆盖掉.
// (4) 把 argv 和 environ 的剩余内存全部清0.
void ngx_setproctitle(const char *title)
{
    // (1) 计算新标题长度 和 argv和environ内存总和.
    size_t ititlelen = strlen(title);
    size_t esy = g_argvneedmem + g_envneedmem;
    if (esy <= ititlelen) // 新标题长度太长
    {
        return;
    }

    // (2) 设置 argv[1] = NULL, 表示argv[]中只有一个元素了, 防止后续argv被滥用, 因为很多判断是用 argv[] == NULL 来做结束标记判断的.
    // 其实步骤4已经有了清0操作, 所以这一步感觉没必要.
    g_os_argv[1] = NULL;

    // (3) 把新标题拷贝进来, 此时原来的命令行参数都会被覆盖掉.
    char *ptmp = g_os_argv[0];
    strcpy(ptmp, title);
    ptmp += ititlelen;

    // (4) 把 argv 和 environ 的剩余内存全部清0.
    size_t cha = esy - ititlelen;
    memset(ptmp, 0, cha);
    return;
}
