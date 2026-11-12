# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 build/ 下的一个同名可执行文件。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

FFMPEG_LIBS = libavdevice libavfilter libavformat libavcodec \
              libswresample libswscale libavutil

PROGRAMS = decode_audio sample_formats decode_wav

CFLAGS += -std=c11 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))
# decode_wav.c 用到了 math.h 里的 lrint()，要链接数学库。
LDLIBS += -lm

# 官方示例 decode_audio.c 带 -Wextra 编译时有一个符号比较的警告，不影响运行。
# 课程保持官方代码原样，所以只对这一个文件关掉这个警告。
build/decode_audio: CFLAGS += -Wno-sign-compare

.PHONY: all clean

all: $(addprefix build/,$(PROGRAMS))

build/%: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build
