
### 目录结构

```bash
include/  			        # 专门存放各种头文件
    ngx_c_conf.h            # 和配置文件处理相关的类,名字带 _c_ 表示和类有关
    ngx_func.h              # 函数声明
    ngx_global.h            # 一些全局/通用定义
    ngx_signal.h            #
    ngx_macro.h             #
app/  				        # 放主应用程序.c(main()函数所在的文件)以及一些比较核心的文件
	link_obj/  		        # 临时目录, 存放临时的 .o 文件, 该目录在 common.mk 中自动创建
    dep/                    # 临时目录, 存放临时的 .d 文件, 该目录在 common.mk 中自动创建
    nginx.cxx               # 主文件, main()入口函数就放到这里
    ngx_c_conf.cxx          # 普通的源码文件, 跟主文件关系密切，又不值得单独放在一个目录
    ngx_string.cxx          # 一些和字符串处理相关的函数
    ngx_setproctitle.cxx    # 设置可执行程序标题相关函数
    ngx_log.cxx             # 和日志相关的函数放之类
    ngx_print.cxx           # 和打印格式相关的函数放这里
misc/                       # 不好归类的 .c 文件
net/                        # 网络处理相关的 .c 文件
proc/                       # 进程处理有关的 .c 文件
signal/                     # 专门用于存放和信号处理有关的1到多个.c文件

makefile                    # 编译项目的入口脚步
config.mk                   # 配置脚步, 被 makefile 包含, 定义一些可变的东西
common.mk                   # 核心, 定义了 makefile 的依赖规则, 被各个子目录下的 makefile 所包含
```


```bash
telnet ip port  # 检测 port
lsof -i:80    # 列出哪些进程在监听80端口

valgrind  # 找bug和改进程序性能工具集
valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all  ./nginx 
```

### 调用accept可能出现的问题

EAGAIN
 - 对accept, send和recv而言, 事件未发生时, errno通常被设置成EAGAIN

ECONNRESET

发生在对方意外关闭套接字后, 您的主机中的软件放弃了一个已建立的连接--由于超时或者其它失败而中止接连(用户插拔网线就可能有这个错误出现)

ECONNABORTED
 - 该错误被描述为"software caused connection abort", 即"软件引起的连接中止".
 - 原因在于当服务器和客户端在完成用于 TCP 的"三次握手"后, 
    - 客户端角度: 送了一个 RST (复位）分节.
    - 服务器角度, 该连接正在 已完成队列 排队, 等着服务进程调用 accept 时受到 RST.
 - 服务器进程一般可以忽略该错误, 直接再次调用accept.

EMFILE 和 ENFILE

ENOSYS
