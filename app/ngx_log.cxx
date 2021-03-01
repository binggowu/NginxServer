#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> // uintptr_t
#include <stdarg.h> // va_start....
#include <unistd.h>
#include <sys/time.h> // gettimeofday
#include <time.h>     // localtime_r
#include <fcntl.h>
#include <errno.h>

#include "ngx_global.h"
#include "ngx_macro.h"
#include "ngx_func.h"
#include "ngx_c_conf.h"

// ---------------
// 日志相关
// ---------------

// 错误等级, 和 ngx_macro.h 里定义的日志等级宏是一一对应关系
static u_char err_levels[][20] =
    {
        {"stderr"}, //0: 控制台错误
        {"emerg"},  //1: 紧急
        {"alert"},  //2: 警戒
        {"crit"},   //3: 严重
        {"error"},  //4: 错误
        {"warn"},   //5: 警告
        {"notice"}, //6: 注意
        {"info"},   //7: 信息
        {"debug"}   //8: 调试
};

ngx_log_t ngx_log;

// void ngx_log_stderr(int err, const char *fmt, ...)
// {
//     va_list args;
//     // 为什么不需要加+1('\0')的, 因为所有的输出errstr的操作都要指定输出的长度, 而不是根据'\0'判断是否结束.
//     u_char errstr[NGX_MAX_ERROR_STR]; // 类似buf功能, 保存错误信息.
//     u_char *p, *last;
//
//     last = errstr + NGX_MAX_ERROR_STR;
//
//     p = ngx_cpymem(errstr, "nginx: ", 7);
//
//     va_start(args, fmt);                   // 使args指向起始的参数
//     p = ngx_vslprintf(p, last, fmt, args); //组合出这个字符串保存在errstr里
//     va_end(args);                          //释放args
//
//     if (err) //如果错误代码不是0，表示有错误发生
//     {
//         //错误代码和错误信息也要显示出来
//         p = ngx_log_errno(p, last, err);
//     }
//
//     //若位置不够, 那换行也要硬插入到末尾, 哪怕覆盖到其他内容
//     if (p >= (last - 1))
//     {
//         p = (last - 1) - 1; //把尾部空格留出来，这里感觉nginx处理的似乎就不对
//                             //我觉得，last-1，才是最后 一个而有效的内存，而这个位置要保存\0，所以我认为再减1，这个位置，才适合保存\n
//     }
//     *p++ = '\n'; //增加个换行符
//
//     //往标准错误【一般是屏幕】输出信息
//     write(STDERR_FILENO, errstr, p - errstr); //三章七节讲过，这个叫标准错误，一般指屏幕
//
//     if (ngx_log.fd > STDERR_FILENO) //如果这是个有效的日志文件，本条件肯定成立，此时也才有意义将这个信息写到日志文件
//     {
//         //因为上边已经把err信息显示出来了，所以这里就不要显示了，否则显示重复了
//         err = 0; //不要再次把错误信息弄到字符串里，否则字符串里重复了
//         p--;
//         *p = 0; //把原来末尾的\n干掉，因为到ngx_log_err_core中还会加这个\n
//         ngx_log_error_core(NGX_LOG_STDERR, err, (const char *)errstr);
//     }
//     return;
// }

// 描述: 往屏幕上打印错误信息, 功能类似与printf, 但功能有扩展.
// 参数err: 将错误编号和对应的错误信息一并放到组合字符串中显示, err=0表示不是错误.
// 参数fmt: 结尾会自动加'\n'
void ngx_log_stderr(int err, const char *fmt, ...)
{
    // 为什么不需要加+1('\0')的, 因为所有的输出errstr的操作都要指定输出的长度, 而不是根据'\0'判断是否结束.
    u_char errstr[NGX_MAX_ERROR_STR] = {0};   // 类似buf功能, 保存错误信息.
    u_char *begin = errstr;                   // begin指向errstr中当前要写入的位置, 类似于vector.begin()
    u_char *end = errstr + NGX_MAX_ERROR_STR; // end指向errstr最后一个有效位置的下一个位置, 类似于vector.end()

    begin = ngx_cpymem(begin, "nginx: ", 7);

    va_list args;
    va_start(args, fmt);                          // 使args指向起始的参数
    begin = ngx_vslprintf(begin, end, fmt, args); //组合出这个字符串保存在errstr里
    va_end(args);                                 //释放args

    if (err != 0) // err!=0: 有错误发生
    {
        begin = ngx_log_errno(begin, end, err);
    }

    if (begin >= end) // errstr末尾必须有一个'\n', 位置不够也要强行加进去.
    {
        begin = end - 1; // 把尾部位置空出来
    }
    *begin++ = '\n';

    write(STDERR_FILENO, errstr, begin - errstr);

    // TODO
    if (ngx_log.fd > STDERR_FILENO) //如果这是个有效的日志文件，本条件肯定成立，此时也才有意义将这个信息写到日志文件
    {
        //因为上边已经把err信息显示出来了，所以这里就不要显示了，否则显示重复了
        err = 0; //不要再次把错误信息弄到字符串里，否则字符串里重复了
        begin--;
        *begin = 0; //把原来末尾的\n干掉，因为到ngx_log_err_core中还会加这个\n
        ngx_log_error_core(NGX_LOG_STDERR, err, (const char *)errstr);
    }

    return;
}

// // 描述：给一个错误编号, 要组合出一个错误字符串: " (错误编号: 错误信息) ", 并放到给的一段内存中去.
// // 参数buf: 内存起始地址.
// // 参数last: 内存终止地址的下一个地址
// // 参数err: 错误编号
// // 返回值: 装入"错误字符串"的内存.
// u_char *ngx_log_errno(u_char *buf, u_char *last, int err)
// {
//     char *perrorinfo = strerror(err);
//     size_t len = strlen(perrorinfo);
//     //然后我还要插入一些字符串： (%d:)
//     char leftstr[10] = {0};
//     sprintf(leftstr, " (%d: ", err);
//     size_t leftlen = strlen(leftstr);
//     char rightstr[] = ") ";
//     size_t rightlen = strlen(rightstr);
//     size_t extralen = leftlen + rightlen; //左右的额外宽度
//     if ((buf + len + extralen) < last)
//     {
//         //保证整个我装得下，我就装，否则我全部抛弃 ,nginx的做法是 如果位置不够，就硬留出50个位置【哪怕覆盖掉以往的有效内容】，也要硬往后边塞，这样当然也可以；
//         buf = ngx_cpymem(buf, leftstr, leftlen);
//         buf = ngx_cpymem(buf, perrorinfo, len);
//         buf = ngx_cpymem(buf, rightstr, rightlen);
//     }
//     return buf;
// }

// 描述：给一个错误编号, 要组合出一个错误字符串: " (错误编号: 错误信息) ", 并放到给的一段内存中去.
// 参数begin: 内存起始地址, 类似于vector.bedin()
// 参数end: 内存终止地址的下一个地址, 类似于vector.end()
// 参数err: 错误编号
// 返回值: 装入"错误字符串"的内存.
u_char *ngx_log_errno(u_char *begin, u_char *end, int err)
{
    char *perrorinfo = strerror(err); // 根据资料不会返回NULL
    size_t perrorlen = strlen(perrorinfo);

    char leftstr[10] = {0};
    sprintf(leftstr, " (%d: ", err);
    size_t leftlen = strlen(leftstr);

    char rightstr[] = ") ";
    size_t rightlen = strlen(rightstr);

    if (begin + leftlen + perrorlen + rightlen < end)
    {
        begin = ngx_cpymem(begin, leftstr, leftlen);
        begin = ngx_cpymem(begin, perrorinfo, perrorlen);
        begin = ngx_cpymem(begin, rightstr, rightlen);
    }
    return begin;
}

// // 描述: 向日志文件中写日志, fmt不必要加'\n', 函数自己会加. 日志的格式: 时间-日志等级-pid-fmt内容-错误信息
// // 参数level: 日志等级, 如果这个数字比nginx.conf中的LogLevel大(不重要), 就不会写入.
// // 参数err: 错误码, 如果不是0, 就转换成对应的错误信息, 一起写到日志文件中.
// void ngx_log_error_core(int level, int err, const char *fmt, ...)
// {
//     u_char *last;
//     u_char errstr[NGX_MAX_ERROR_STR + 1]; //这个+1也是我放入进来的，本函数可以参考ngx_log_stderr()函数的写法；
//
//     memset(errstr, 0, sizeof(errstr));
//     last = errstr + NGX_MAX_ERROR_STR;
//
//     struct timeval tv;
//     struct tm tm;
//     time_t sec; //秒
//     u_char *p;  //指向当前要拷贝数据到其中的内存位置
//     va_list args;
//
//     memset(&tv, 0, sizeof(struct timeval));
//     memset(&tm, 0, sizeof(struct tm));
//
//     gettimeofday(&tv, NULL); //获取当前时间，返回自1970-01-01 00:00:00到现在经历的秒数【第二个参数是时区，一般不关心】
//
//     sec = tv.tv_sec;        //秒
//     localtime_r(&sec, &tm); //把参数1的time_t转换为本地时间，保存到参数2中去，带_r的是线程安全的版本，尽量使用
//     tm.tm_mon++;            //月份要调整下正常
//     tm.tm_year += 1900;     //年份要调整下才正常
//
//     u_char strcurrtime[40] = {0}; //先组合出一个当前时间字符串，格式形如：2019/01/08 19:57:11
//     ngx_slprintf(strcurrtime,
//                  (u_char *)-1,                   //若用一个u_char *接一个 (u_char *)-1,则 得到的结果是 0xffffffff....，这个值足够大
//                  "%4d/%02d/%02d %02d:%02d:%02d", //格式是 年/月/日 时:分:秒
//                  tm.tm_year, tm.tm_mon,
//                  tm.tm_mday, tm.tm_hour,
//                  tm.tm_min, tm.tm_sec);
//
//     p = ngx_cpymem(errstr, strcurrtime, strlen((const char *)strcurrtime)); //日期增加进来，得到形如：     2019/01/08 20:26:07
//     p = ngx_slprintf(p, last, " [%s] ", err_levels[level]);                 //日志级别增加进来，得到形如：  2019/01/08 20:26:07 [crit]
//     p = ngx_slprintf(p, last, "%P: ", ngx_pid);                             //支持%P格式，进程id增加进来，得到形如：   2019/01/08 20:50:15 [crit] 2037:
//
//     va_start(args, fmt);                   //使args指向起始的参数
//     p = ngx_vslprintf(p, last, fmt, args); //把fmt和args参数弄进去，组合出来这个字符串
//     va_end(args);                          //释放args
//
//     if (err) //如果错误代码不是0，表示有错误发生
//     {
//         //错误代码和错误信息也要显示出来
//         p = ngx_log_errno(p, last, err);
//     }
//     //若位置不够，那换行也要硬插入到末尾，哪怕覆盖到其他内容
//     if (p >= (last - 1))
//     {
//         p = (last - 1) - 1; //把尾部空格留出来，这里感觉nginx处理的似乎就不对
//                             //我觉得，last-1，才是最后 一个而有效的内存，而这个位置要保存\0，所以我认为再减1，这个位置，才适合保存\n
//     }
//     *p++ = '\n'; //增加个换行符
//
//     //这么写代码是图方便：随时可以把流程弄到while后边去；大家可以借鉴一下这种写法
//     ssize_t n;
//     while (1)
//     {
//         if (level > ngx_log.log_level)
//         {
//             //要打印的这个日志的等级太落后（等级数字太大，比配置文件中的数字大)
//             //这种日志就不打印了
//             break;
//         }
//         //磁盘是否满了的判断，先算了吧，还是由管理员保证这个事情吧；
//
//         //写日志文件
//         n = write(ngx_log.fd, errstr, p - errstr); //文件写入成功后，如果中途
//         if (n == -1)
//         {
//             //写失败有问题
//             if (errno == ENOSPC) //写失败，且原因是磁盘没空间了
//             {
//                 //磁盘没空间了
//                 //没空间还写个毛线啊
//                 //先do nothing吧；
//             }
//             else
//             {
//                 //这是有其他错误，那么我考虑把这个错误显示到标准错误设备吧；
//                 if (ngx_log.fd != STDERR_FILENO) //当前是定位到文件的，则条件成立
//                 {
//                     n = write(STDERR_FILENO, errstr, p - errstr);
//                 }
//             }
//         }
//         break;
//     } //end while
//     return;
// }

// 描述: 向日志文件中写日志, fmt不必要加'\n', 函数自己会加. 日志的格式: 时间-日志等级-pid-fmt内容-错误信息
// 参数level: 日志等级, 如果这个数字比nginx.conf中的LogLevel大(不重要), 就不会写入.
// 参数err: 错误码, 如果不是0, 就转换成对应的错误信息, 一起写到日志文件中.
void ngx_log_error_core(int level, int err, const char *fmt, ...)
{
    u_char errstr[NGX_MAX_ERROR_STR] = {0};
    u_char *begin = errstr;                   // 指向第一个位置
    u_char *end = errstr + NGX_MAX_ERROR_STR; // 指向最后一个位置的下一个位置

    // 时间
    struct timeval tv;
    struct tm tm;
    time_t sec;

    memset(&tv, 0, sizeof(tv));
    memset(&tm, 0, sizeof(tm));

    gettimeofday(&tv, NULL); // 获取当前时间, 自1970-01-01 00:00:00到现在经历的秒数
    sec = tv.tv_sec;
    localtime_r(&sec, &tm); // 把sec转换为本地时间, 保存到tm中去. 带_r的是线程安全的版本
    tm.tm_mon++;            // 月份调整
    tm.tm_year += 1900;     // 年份调整

    u_char strcurtime[40] = {0}; // 组合出一个当前时间字符串, 格式: 2019/01/08 19:57:11
    ngx_slprintf(strcurtime,
                 (u_char *)-1,                    // 0xffff ffff ffff ffff, 没什么实际作用, 就是足够大的指针, 因为此处写入固定大小的字符, 人为控制不会越界接可以了.
                 "%04d/%02d/%02d %02d:%02d:%02d", // 格式: 年/月/日 时:分:秒
                 tm.tm_year, tm.tm_mon, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);

    // 拷贝 时间/日志等级/pid
    begin = ngx_cpymem(begin, strcurtime, strlen((const char *)strcurtime));
    begin = ngx_slprintf(begin, end, " [%s] ", err_levels[level]);
    begin = ngx_slprintf(begin, end, "%P: ", ngx_pid);

    // 解析fmt/...
    va_list args;
    va_start(args, fmt);
    begin = ngx_vslprintf(begin, end, fmt, args);
    va_end(args);

    if (err != 0) // err!=0: 有错误发生
    {
        begin = ngx_log_errno(begin, end, err);
    }

    if (begin >= end) // errstr末尾必须有一个'\n', 位置不够也要强行加进去.
    {
        begin = end - 1; // 把尾部位置空出来
    }
    *begin++ = '\n';

    size_t n;
    while (1)
    {
        if (level > ngx_log.log_level)
        {
            // 当前的这个日志等级不重要, 就不写入了.
            break;
        }

        n = write(ngx_log.fd, errstr, begin - errstr);
        if (-1 == n)
        {
            // ngx_log_errno(begin, end, errno); //

            if (errno == ENOSPC)
            {
                // TODO: 磁盘没有空间了
            }
            else
            {
                if (ngx_log.fd != STDERR_FILENO)
                {
                    n = write(STDERR_FILENO, errstr, begin - errstr);
                }
            }
        }
        break;
    }

    return;
}

// void ngx_log_init()
// {
//     u_char *plogname = NULL;
//     size_t nlen;
//
//     // 从配置文件中读取和日志相关的配置信息
//     CConfig *p_config = CConfig::GetInstance();
//     plogname = (u_char *)p_config->GetString("Log");
//     if (plogname == NULL)
//     {
//         //没读到，就要给个缺省的路径文件名了
//         plogname = (u_char *)NGX_ERROR_LOG_PATH; //"logs/error.log" ,logs目录需要提前建立出来
//     }
//     ngx_log.log_level = p_config->GetIntDefault("LogLevel", NGX_LOG_NOTICE); // 缺省日志等级为6【注意】 ，如果读失败，就给缺省日志等级
//     //nlen = strlen((const char *)plogname);
//
//     //ngx_log.fd = open((const char *)plogname,O_WRONLY|O_APPEND|O_CREAT|O_DIRECT,0644);   //绕过内和缓冲区，write()成功则写磁盘必然成功，但效率可能会比较低；
//     ngx_log.fd = open((const char *)plogname, O_WRONLY | O_APPEND | O_CREAT, 0644);
//     if (ngx_log.fd == -1) //如果有错误，则直接定位到 标准错误上去
//     {
//         ngx_log_stderr(errno, "[alert] could not open error log file: open() \"%s\" failed", plogname);
//         ngx_log.fd = STDERR_FILENO; //直接定位到标准错误去了
//     }
//     return;
// }

// 日志文件(ngx_log)初始化. 
// 注意: 此处有open对应的close在main()最后的free_resource()中.
// (1) 读取配置文件(nginx.conf)中的Log项(日志文件路径)和LogLevel项(日志等级);
// (2) 打开日志文件, 获得fd并赋值给ngx_log.fd.
void ngx_log_init()
{
    const char *p_logpath = NULL;

    // (1) 读取配置文件(nginx.conf)中的日志文件路径(Log)和日志等级(LogLevel);
    CConfig *p_cnofig = CConfig::GetInstance();
    p_logpath = p_cnofig->GetString("Log");
    if (NULL == p_logpath)
    {
        p_logpath = (const char *)NGX_ERROR_LOG_PATH;
    }
    ngx_log.log_level = p_cnofig->GetIntDefault("LogLevel", NGX_LOG_NOTICE); // 缺省日志等级为NOTIC

    // (2) 打开日志文件, 获得fd.
    // O_DIRECT: 绕过内和缓冲区, write()成功则写磁盘必然成功, 但效率可能会比较低.
    // ngx_log.fd = open((const char *)plogname, O_WRONLY | O_APPEND | O_CREAT | O_DIRECT, 0644);
    ngx_log.fd = open(p_logpath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (-1 == ngx_log.fd)
    {
        ngx_log_stderr(errno, "[alter] could not open log file [%s]", p_logpath);
        ngx_log.fd = STDERR_FILENO; // 直接定位到标准错误去了
    }
    return;
}
