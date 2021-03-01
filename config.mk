
# 项目根目录, 通过export把某个变量声明为全局的[其他文件中可以用].
export BUILD_ROOT = $(shell pwd)

# 头文件路径
export INCLUDE_PATH = $(BUILD_ROOT)/_include

# 要编译的目录
BUILD_DIR = $(BUILD_ROOT)/signal/ \
			$(BUILD_ROOT)/proc/   \
			$(BUILD_ROOT)/net/    \
			$(BUILD_ROOT)/misc/   \
			$(BUILD_ROOT)/logic/   \
			$(BUILD_ROOT)/app/ 

# 是否生成调试信息
export DEBUG = true
