# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 build/ 下的一个同名可执行文件。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

FFMPEG_LIBS = libavdevice libavfilter libavformat libavcodec \
              libswresample libswscale libavutil

PROGRAMS = filter_audio volume_file filter_info

CFLAGS += -std=c11 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))
# 官方示例用了 sin()，要链接数学库。
LDLIBS += -lm

# 官方示例 filter_audio.c 把字符串缓冲区声明成了 uint8_t 数组，还有一处符号比较，编译时有警告，不影响运行。
# 课程保持官方代码原样，所以只对这一个文件关掉这个警告。
build/filter_audio: CFLAGS += -Wno-pointer-sign -Wno-sign-compare

.PHONY: all clean

all: $(addprefix build/,$(PROGRAMS))

build/%: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build
