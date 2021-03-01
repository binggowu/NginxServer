
# .PHONY:all clean 

ifeq ($(DEBUG),true)
CC = g++ -std=c++11 -g 
VERSION = debug
else
CC = g++ -std=c++11
VERSION = release
endif

SRCS = $(wildcard *.cxx)
OBJS = $(SRCS:.cxx=.o)
DEPS = $(SRCS:.cxx=.d)

BIN := $(addprefix $(BUILD_ROOT)/,$(BIN))

# 定义存放obj文件的目录, 目录统一到一个位置, 方便后续链接.
# 注意: 下边字符串末尾不要有空格等, 否则会语法错误
LINK_OBJ_DIR = $(BUILD_ROOT)/app/link_obj
DEP_DIR      = $(BUILD_ROOT)/app/dep
$(shell mkdir -p $(LINK_OBJ_DIR))
$(shell mkdir -p $(DEP_DIR))

# 处理后形如 /mnt/hgfs/linux/nginx/app/link_obj/ngx_signal2.o /mnt/hgfs/linux/nginx/app/link_obj/ngx_signal.o
# := 在解析阶段直接赋值常量字符串(立即展开), 而 = 在运行阶段，实际使用变量时再进行求值(延迟展开)
# /mnt/hgfs/linux/nginx/app/link_obj/nginx.o   /mnt/hgfs/linux/nginx/app/link_obj/ngx_conf.o 
OBJS := $(addprefix $(LINK_OBJ_DIR)/,$(OBJS))
DEPS := $(addprefix $(DEP_DIR)/,$(DEPS))

# 找到目录中的所有.o文件
LINK_OBJ = $(wildcard $(LINK_OBJ_DIR)/*.o)
# 构建依赖关系时, app目录下这个.o文件还没构建出来, 所以LINK_OBJ是缺少这个.o的, 我们要把这个.o文件加进来
LINK_OBJ += $(OBJS)

#-------------------------------------------------------------------------------------------------------
all:$(DEPS) $(OBJS) $(BIN)

# 这里是诸多.d文件被包含进来, 每个.d文件里都记录着一个.o文件所依赖哪些.c和.h文件. 内容诸如 nginx.o: nginx.c ngx_func.h
# 我们做这个的目的: 即便.h被修改了, 也要让make重新编译我们的工程.
# 有必要先判断这些文件是否存在, 不然make可能会报一些.d文件找不到
ifneq ("$(wildcard $(DEPS))","") 
include $(DEPS)  
endif

$(BIN):$(LINK_OBJ)
	$(CC) -o $@ $^ -lpthread

$(LINK_OBJ_DIR)/%.o:%.cxx
	$(CC) -I$(INCLUDE_PATH) -o $@ -c $(filter %.cxx,$^)

# 我们现在希望当修改一个.h时, 也能够让make自动重新编译我们的项目, 所以, 我们需要指明让.o依赖于.h文件
# 那一个.o依赖于哪些.h文件, 我们可以用"gcc -MM c程序文件" 来获得这些依赖信息并重定向保存到.d文件中
# .d文件中的内容可能形如：nginx.o: nginx.c ngx_func.h
# %.d:%.c
$(DEP_DIR)/%.d:%.cxx
# .d文件中的内容形如: nginx.o: nginx.c ngx_func.h ../signal/ngx_signal.h，但现在的问题是我们的.o文件已经放到了专门的目录
# 所以我们要正确指明.o文件路径这样, 对应的.h,.c修改后, make时才能发现.
# echo 中 -n表示后续追加不换行
	@echo -n $(LINK_OBJ_DIR)/ > $@
#  >>表示追加
	$(CC) -I$(INCLUDE_PATH) -MM $^ >> $@

# 上行处理后，.d文件中内容应该就如：/mnt/hgfs/linux/nginx/app/link_obj/nginx.o: nginx.c ngx_func.h ../signal/ngx_signal.h
