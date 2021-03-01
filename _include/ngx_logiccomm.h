﻿
#ifndef __NGX_LOGICCOMM_H__
#define __NGX_LOGICCOMM_H__

// -------------------------
// 业务逻辑 相关的定义
// -------------------------

#define _CMD_START	                    0  
#define _CMD_PING				   	    _CMD_START + 0   // ping命令[心跳包]
#define _CMD_REGISTER 		            _CMD_START + 5   // 注册
#define _CMD_LOGIN 		                _CMD_START + 6   // 登录

#pragma pack (1) // 1字节对齐

// 注册结构体
typedef struct _STRUCT_REGISTER
{
	int           iType;          // 类型
	char          username[56];   // 用户名 
	char          password[40];   // 密码

}STRUCT_REGISTER, *LPSTRUCT_REGISTER;

// 登录结构体
typedef struct _STRUCT_LOGIN
{
	char          username[56];   // 用户名 
	char          password[40];   // 密码

}STRUCT_LOGIN, *LPSTRUCT_LOGIN;

#pragma pack() //取消指定对齐, 恢复缺省对齐

#endif