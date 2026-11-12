# mtool 的构建。和各讲的 Makefile 一样用 pkg-config 取编译和链接参数；
# 如果 pkg-config 找不到 FFmpeg，先设置：export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig
#
# 带检查的构建（L22）：
#   make SANITIZE=address,undefined BUILD=build-asan     ASan 加 UBSan
#   make SANITIZE=thread BUILD=build-tsan                TSan（不能和 ASan 同时开）
# 要加别的编译选项用 EXTRA_CFLAGS=…，不要写 make CFLAGS=…（会盖掉下面的 -std 和头文件路径）。

FFMPEG_LIBS = libavdevice libavfilter libavformat libavcodec \
              libswresample libswscale libavutil

BUILD ?= build
SRCS  := $(wildcard src/*.c)
HDRS  := $(wildcard src/*.h)

CFLAGS += -std=c11 -D_GNU_SOURCE -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
CFLAGS += $(EXTRA_CFLAGS)
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS)) -lm -lpthread

ifneq ($(SANITIZE),)
CFLAGS  += -O1 -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
LDFLAGS += -fsanitize=$(SANITIZE)
endif

.PHONY: all clean test
all: $(BUILD)/mtool

$(BUILD)/mtool: $(SRCS) $(HDRS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ $(LDLIBS)

test: all
	BUILD=$(BUILD) tests/run_tests.sh

clean:
	$(RM) -r build build-asan build-tsan
