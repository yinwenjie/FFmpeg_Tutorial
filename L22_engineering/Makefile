# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 $(BUILD)/ 下的一个同名可执行文件，默认 build/。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig
#
# 带检查的构建（L22 第 1 步）：
#   make clean && make SANITIZE=address,undefined               # ASan + UBSan：内存错误、泄漏、未定义行为
#   make SANITIZE=thread BUILD=build-tsan                       # TSan：数据竞争（不能和 address 一起用，所以放到另一个目录）
# 去掉 fflog.h 里的锁，亲眼看多线程下的后果（第 3 步的练习）：
#   make clean && make SANITIZE=thread EXTRA_CFLAGS=-DFFLOG_NO_LOCK
# （不要写 make CFLAGS=…：命令行上的 CFLAGS 会盖掉下面的 -std、pkg-config 的头文件路径等。
#   要加编译选项，用环境变量 CFLAGS=… make，或者上面的 EXTRA_CFLAGS。）

FFMPEG_LIBS = libavformat libavcodec libavfilter libswscale libswresample libavutil

PROGRAMS = reconfig_graph log_demo err_codes version_info

BUILD    ?= build
SANITIZE ?=

CFLAGS += -std=c11 -Wall -Wextra -g -pthread $(EXTRA_CFLAGS)
ifneq ($(SANITIZE),)
CFLAGS += -O1 -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
endif
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

.PHONY: all clean

all: $(addprefix $(BUILD)/,$(PROGRAMS))

# fflog.h 是 log_demo 的头文件，改了它要重新编译
$(BUILD)/log_demo: fflog.h

$(BUILD)/%: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build build-tsan
