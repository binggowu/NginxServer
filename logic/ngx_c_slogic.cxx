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
#include <pthread.h>

#include "ngx_c_conf.h"
#include "ngx_macro.h"
#include "ngx_global.h"
#include "ngx_func.h"
#include "ngx_c_memory.h"
#include "ngx_c_crc32.h"
#include "ngx_c_slogic.h"
#include "ngx_logiccomm.h"
#include "ngx_c_lockmutex.h"

// ----------------------------------
// 业务处理 有关的函数
// ----------------------------------

// 定义成员函数指针
typedef bool (CLogicSocket::*handler)(lpngx_connection_t pConn,      // 连接池中连接指针
                                      LPSTRUC_MSG_HEADER pMsgHeader, // 消息头指针
                                      char *pPkgBody,                // 包体指针
                                      unsigned short iBodyLength);   // 包体长度

// 业务逻辑 数组
static const handler statusHandler[] =
    {
        // 保留数组前5个, 以备将来增加一些基本服务器功能
        &CLogicSocket::_HandlePing, // [0]心跳包
        NULL,                       // [1]
        NULL,                       // [2]
        NULL,                       // [3]
        NULL,                       // [4]

        &CLogicSocket::_HandleRegister, // [5]注册
        &CLogicSocket::_HandleLogIn,    // [6]登录

        // 扩展...
};

#define AUTH_TOTAL_COMMANDS sizeof(statusHandler) / sizeof(handler) // 目前支持的 消息数目

// 构造函数, 为空
CLogicSocket::CLogicSocket()
{
}

// 析构函数, 为空
CLogicSocket::~CLogicSocket()
{
}

// 调用父类CSocekt::initialize()
bool CLogicSocket::Initialize()
{
    return CSocekt::Initialize();
}

/* 
描述: 处理收到的数据包
参数pMsgBuf: pConn->precvMemPointer, new出来的数据包(消息体+包头+包体).
(1) 校验crc32值, 如果crc32值错, 直接丢弃.
(2) 通过iCurrsequence过滤废包
(3) 判断"消息码"是否有效
(4) 调用"消息码"对应的成员函数来处理
调用: CThreadPool::ThreadFunc()[收消息队列的线程]
 */
void CLogicSocket::threadRecvProcFunc(char *pMsgBuf)
{
    LPSTRUC_MSG_HEADER pMsgHeader = (LPSTRUC_MSG_HEADER)pMsgBuf;                   // 消息头
    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)(pMsgBuf + m_iLenMsgHeader); // 包头
    void *pPkgBody;                                                                // 包体
    unsigned short pkglen = ntohs(pPkgHeader->pkgLen);                             // 包长(包头长+包体长)

    // (1) 校验crc32值, 如果crc32值错, 直接丢弃.
    if (m_iLenPkgHeader == pkglen) // 只有包头, 没有包体
    {
        if (pPkgHeader->crc32 != 0) // 只有包头的数据包的crc32值是0
        {
            return; // crc32值错, 直接丢弃
        }
        pPkgBody = NULL;
    }
    else // 有包体
    {
        pPkgHeader->crc32 = ntohl(pPkgHeader->crc32);
        pPkgBody = (void *)(pMsgBuf + m_iLenMsgHeader + m_iLenPkgHeader);

        // 计算crc32值
        int calccrc = CCRC32::GetInstance()->Get_CRC((unsigned char *)pPkgBody, pkglen - m_iLenPkgHeader);
        if (calccrc != pPkgHeader->crc32) // 对比CRC32值
        {
            ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc() 中 CRC 错误[服务器:%d/客户端:%d], 丢弃数据.", calccrc, pPkgHeader->crc32);

            return; // crc32值错, 直接丢弃
        }
        else
        {
            ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc()中CRC正确[服务器:%d/客户端:%d].", calccrc, pPkgHeader->crc32);
        }
    }

    // (2) 通过iCurrsequence过滤废包
    lpngx_connection_t p_Conn = pMsgHeader->pConn; // 消息头中保存着"连接"
    // 从 收到客户端发送来的包 到 服务器取线程池中的一个线程处理该包 的过程中,
    // 如果该连接以被其他tcp连接(socket)占用, 则 消息头中iCurrsequence 和 连接中的iCurrsequence 是不相等的.
    // 这说明原来的客户端和服务器的连接断了, 这种包就是废包, 不处理.
    if (p_Conn->iCurrsequence != pMsgHeader->iCurrsequence)
    {
        return;
    }

    // (3) 判断"消息码"是否有效
    unsigned short imsgCode = ntohs(pPkgHeader->msgCode); // 消息代码
    if (imsgCode >= AUTH_TOTAL_COMMANDS)                  // 发送一个不在我们服务器处理范围内的消息码
    {
        ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc() 中 imsgCode=[%d], 消息码不对.", imsgCode);
        return; // 丢弃不理这种包
    }

    if (NULL == statusHandler[imsgCode]) // 没有相关的处理函数
    {
        ngx_log_stderr(0, "CLogicSocket::threadRecvProcFunc() 中 imsgCode=[%d]消息码找不到对应的处理函数.", imsgCode);
        return; // 丢弃不理这种包
    }

    // (4) 调用"消息码"对应的成员函数来处理
    (this->*statusHandler[imsgCode])(p_Conn, pMsgHeader, (char *)pPkgBody, pkglen - m_iLenPkgHeader);
    return;
}

// 检测心跳包是否超时
// (1) 判断连接是否断了(iCurrsequence).
// (2) 判断客户端是否超时不发心跳包.
void CLogicSocket::procPingTimeOutChecking(LPSTRUC_MSG_HEADER tmpmsg, time_t cur_time)
{
    CMemory *p_memory = CMemory::GetInstance();

    if (tmpmsg->iCurrsequence == tmpmsg->pConn->iCurrsequence) // 此连接没断
    {
        lpngx_connection_t p_Conn = tmpmsg->pConn;

        if (/*m_ifkickTimeCount == 1 && */ m_ifTimeOutKick == 1) // 能调用到本函数第一个条件肯定成立， 所以第一个条件加不加无所谓， 主要是第二个条件
        {
            // 到时间直接踢出去
            zdClosesocketProc(p_Conn);
        }
        // 超时踢的判断条件: 检查超时的时间间隔 * 3 + 10
        else if ((cur_time - p_Conn->lastPingTime) > (m_iWaitTime * 3 + 10))
        {
            // 如果此时此刻该用户正好断线, 则这个socket可能立即被后续上来的连接复用. 如果真有人这么倒霉, 赶上这个点了, 那么可能错踢, 错踢就错踢.
            ngx_log_stderr(0, "超时不发心跳包, 踢出去!"); //感觉OK
            zdClosesocketProc(p_Conn);
        }

        p_memory->FreeMemory(tmpmsg); // 内存要释放
    }
    else // 此连接断了
    {
        p_memory->FreeMemory(tmpmsg); // 内存要释放
    }
    return;
}

// 服务器回复一个心跳包
// 1) 分配内存; 2) 准备数据(消息头+包头); 3) 发送.
void CLogicSocket::SendNoBodyPkgToClient(LPSTRUC_MSG_HEADER pMsgHeader, unsigned short iMsgCode)
{
    // 分配内存
    CMemory *p_memory = CMemory::GetInstance();
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader, false);

    // 拷贝消息体
    char *p_tmpbuf = p_sendbuf;
    memcpy(p_tmpbuf, pMsgHeader, m_iLenMsgHeader);
    p_tmpbuf += m_iLenMsgHeader;

    // 拷贝包头
    LPCOMM_PKG_HEADER pPkgHeader = (LPCOMM_PKG_HEADER)p_tmpbuf;
    pPkgHeader->msgCode = htons(iMsgCode);
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader);
    pPkgHeader->crc32 = 0;

    // 发送
    msgSend(p_sendbuf);

    return;
}

//---------------------------------------------- 处理各种业务逻辑 ------------------------------------------------------------

// pPkgBody: 约定这个命令[msgCode]必须带包体, 否则认为是恶意包.
bool CLogicSocket::_HandleRegister(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    if (pPkgBody == NULL) // 不带包体, 认为是恶意包
    {
        return false;
    }

    int iRecvLen = sizeof(STRUCT_REGISTER);
    if (iRecvLen != iBodyLength) // 发送过来的结构大小不对, 认为是恶意包
    {
        return false;
    }

    // 对于同一个用户, 可能同时发送来多个请求过来, 造成多个线程同时为该用户服务,
    // 以网游为例, 用户要在商店中买A物品, 又买B物品, 而用户的钱只够买A或者B中的一个, 不够同时买A和B. 如果用户发送购买命令过来买了一次A, 又买了一次B, 如果是两个线程来执行同一个用户的这两次不同的购买命令, 很可能造成这个用户购买成功了A, 又购买成功了B. 所以针对某个用户的命令, 我们一般都要互斥, 我们需要增加临界的变量于 ngx_connection_s 结构中.
    CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

    // 取得了整个发送过来的数据
    LPSTRUCT_REGISTER p_RecvInfo = (LPSTRUCT_REGISTER)pPkgBody;
    p_RecvInfo->iType = ntohl(p_RecvInfo->iType);               // 所有数值型不要忘记传输之前主机网络序, 收到后网络转主机序
    p_RecvInfo->username[sizeof(p_RecvInfo->username) - 1] = 0; // 非常关键, 防止客户端发送过来畸形包, 导致服务器直接使用这个数据出现错误.
    p_RecvInfo->password[sizeof(p_RecvInfo->password) - 1] = 0; // 非常关键, 防止客户端发送过来畸形包, 导致服务器直接使用这个数据出现错误.

    // 这里可能要考虑 根据业务逻辑, 进一步判断收到的数据的合法性,

    // 给客户端返回数据时, 一般也是返回一个结构, 这个结构内容具体由客户端/服务器协商,
    // 这里以给客户端也返回同样的 STRUCT_REGISTER 结构来举例
    // LPSTRUCT_REGISTER pFromPkgHeader =  (LPSTRUCT_REGISTER)(((char *)pMsgHeader)+m_iLenMsgHeader);	// 指向收到的包的包头, 其中数据后续可能要用到
    LPCOMM_PKG_HEADER pPkgHeader;
    CMemory *p_memory = CMemory::GetInstance();
    CCRC32 *p_crc32 = CCRC32::GetInstance();
    int iSendLen = sizeof(STRUCT_REGISTER);

    // a) 分配要发包的内存
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iSendLen, false); // 准备发送的格式: 消息头+包头+包体

    // b) 填充消息头
    memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader);

    // c) 填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + m_iLenMsgHeader);
    pPkgHeader->msgCode = _CMD_REGISTER;                    // 消息代码, 可以统一在 ngx_logiccomm.h 中定义
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);       // 转网络序
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader + iSendLen); // 包长(包头+包体)

    // d) 填充包体
    LPSTRUCT_REGISTER p_sendInfo = (LPSTRUCT_REGISTER)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader);
    // ... 根据需要填充要发回给客户端的内容, int类型要使用htonl()转, short类型要使用htons()转.

    // e) 计算包体的crc32值
    pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char *)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);

    // f) 发送数据包
    msgSend(p_sendbuf);

    return true;
}

bool CLogicSocket::_HandleLogIn(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    if (pPkgBody == NULL) // 不带包体, 认为是恶意包
    {
        return false;
    }
    int iRecvLen = sizeof(STRUCT_LOGIN);
    if (iRecvLen != iBodyLength) // 发送过来的结构大小不对, 认为是恶意包
    {
        return false;
    }

    CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都互斥

    // 取得了整个发送过来的数据
    LPSTRUCT_LOGIN p_RecvInfo = (LPSTRUCT_LOGIN)pPkgBody;
    p_RecvInfo->username[sizeof(p_RecvInfo->username) - 1] = 0;
    p_RecvInfo->password[sizeof(p_RecvInfo->password) - 1] = 0;

    LPCOMM_PKG_HEADER pPkgHeader;
    CMemory *p_memory = CMemory::GetInstance();
    CCRC32 *p_crc32 = CCRC32::GetInstance();

    // a) 分配要发包的内存
    int iSendLen = sizeof(STRUCT_LOGIN);
    char *p_sendbuf = (char *)p_memory->AllocMemory(m_iLenMsgHeader + m_iLenPkgHeader + iSendLen, false);

    // b) 填充消息头
    memcpy(p_sendbuf, pMsgHeader, m_iLenMsgHeader);

    // c) 填充包头
    pPkgHeader = (LPCOMM_PKG_HEADER)(p_sendbuf + m_iLenMsgHeader);
    pPkgHeader->msgCode = _CMD_LOGIN;
    pPkgHeader->msgCode = htons(pPkgHeader->msgCode);
    pPkgHeader->pkgLen = htons(m_iLenPkgHeader + iSendLen);

    // d) 填充包体
    LPSTRUCT_LOGIN p_sendInfo = (LPSTRUCT_LOGIN)(p_sendbuf + m_iLenMsgHeader + m_iLenPkgHeader);
    // ... 根据需要填充要发回给客户端的内容, int类型要使用htonl()转, short类型要使用htons()转.

    // e) 包体内容全部确定好后, 计算包体的crc32值
    pPkgHeader->crc32 = p_crc32->Get_CRC((unsigned char *)p_sendInfo, iSendLen);
    pPkgHeader->crc32 = htonl(pPkgHeader->crc32);
    ngx_log_stderr(0, "成功收到登录并返回结果!");

    // f) 发送数据包
    msgSend(p_sendbuf);
    return true;
}

// 接收并处理客户端发送过来的ping包
bool CLogicSocket::_HandlePing(lpngx_connection_t pConn, LPSTRUC_MSG_HEADER pMsgHeader, char *pPkgBody, unsigned short iBodyLength)
{
    // 心跳包要求没有包体, 否则认为是非法包
    if (iBodyLength != 0)
    {
        return false;
    }

    CLock lock(&pConn->logicPorcMutex); // 凡是和本用户有关的访问都考虑用互斥, 以免该用户同时发送过来两个命令达到各种作弊目的.
    pConn->lastPingTime = time(NULL);   // 更新心跳包时间

    // 服务器回复一个心跳包
    SendNoBodyPkgToClient(pMsgHeader, _CMD_PING);

    ngx_log_stderr(0, "成功收到了心跳包并返回结果!");
    return true;
}
