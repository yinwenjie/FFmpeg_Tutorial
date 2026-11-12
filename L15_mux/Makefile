# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 build/ 下的一个同名可执行文件。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

FFMPEG_LIBS = libavdevice libavfilter libavformat libavcodec \
              libswresample libswscale libavutil

PROGRAMS = mux mux_av extract_h264

CFLAGS += -std=c11 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))
# 官方示例用了 sin()，要链接数学库。
LDLIBS += -lm

.PHONY: all clean

all: $(addprefix build/,$(PROGRAMS))

# 官方示例 mux.c 保持原样，它的几个函数有没用到的参数，编译时会有警告，这里关掉。
build/mux: CFLAGS += -Wno-unused-parameter

build/%: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build
