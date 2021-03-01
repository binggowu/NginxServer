
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ngx_c_memory.h"

// ---------------------------------------
//和 内存分配 有关的函数放这里
// ---------------------------------------

// 类静态成员
CMemory *CMemory::m_instance = NULL;

// 分配内存
// memCount: 分配的字节大小
// ifmemset: 是否用 0 初始化这块内存, false可以提高点效率
void *CMemory::AllocMemory(int memCount, bool ifmemset)
{
    void *tmpData = (void *)new char[memCount];
    if (ifmemset)
    {
        memset(tmpData, 0, memCount);
    }
    return tmpData;
}

// 内存释放函数
void CMemory::FreeMemory(void *point)
{
    // new 的时候是char *, 这里弄回char *, 以免出警告:  warning: deleting ‘void*’ is undefined [-Wdelete-incomplete]
    delete[]((char *)point);
}
