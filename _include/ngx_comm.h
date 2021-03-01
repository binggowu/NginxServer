
#ifndef __NGX_COMM_H__
#define __NGX_COMM_H__

// ------------------------------------
// 收发包 有关
// ------------------------------------

#define _PKG_MAX_LENGTH 30000 // 包长(包头+包体)的最大值, 为预留一些空间(1000), 实现时包长最大值29000

// 收包状态: _PKG_HD_INIT(0) -> _PKG_HD_RECVING(1) -> _PKG_BD_INIT(2) -> _PKG_BD_RECVING(3) -> _PKG_HD_INIT(0) -> ...
// 在 ngx_connection_t.curStat 中被使用

#define _PKG_HD_INIT 0	  // 准备接收包头
#define _PKG_HD_RECVING 1 // 接收包头中
#define _PKG_BD_INIT 2	  // 包头刚好收完, 准备接收包体
#define _PKG_BD_RECVING 3 // 接收包体中.

#define _DATA_BUFSIZE_ 20 // 收包头用, 要求大于 sizeof(COMM_PKG_HEADER)

#pragma pack(1) // 所有在网络上传输的结构体, 必须都采用"1字节对齐"

// 包头结构
typedef struct _COMM_PKG_HEADER
{
	unsigned short pkgLen;	// 包长(包头+包体), unsigned short为2字节, 最大可以表示66635, 而 _PKG_MAX_LENGTH 为30000, 足够用.
	unsigned short msgCode; // 消息类型代码, 用于区别不同的命令(消息)
	int crc32;				// CRC32效验
} COMM_PKG_HEADER, *LPCOMM_PKG_HEADER;

#pragma pack() // 取消指定对齐方式, 恢复默认对齐方式

#endif
