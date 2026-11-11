# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 build/ 下的一个同名可执行文件。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

FFMPEG_LIBS = libavdevice libavfilter libavformat libavcodec \
              libswresample libswscale libavutil

PROGRAMS = remux packet_info timebase_math remux_cut

CFLAGS += -std=c11 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

.PHONY: all clean

all: $(addprefix build/,$(PROGRAMS))

# remux.c 是官方示例的原样拷贝，里面有一处 int 和 unsigned 的比较，
# 用 -Wextra 编译会有警告。代码不能改，所以只对这个文件关掉这一项警告。
build/remux: CFLAGS += -Wno-sign-compare

build/%: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build
