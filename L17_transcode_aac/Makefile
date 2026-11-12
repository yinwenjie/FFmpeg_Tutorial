# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 build/ 下的一个同名可执行文件。
# 音频转码用不到滤镜和缩放，所以只链接这四个库。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

FFMPEG_LIBS = libavformat libavcodec libswresample libavutil

PROGRAMS = transcode_aac transcode_aac_av

CFLAGS += -std=c11 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

.PHONY: all clean

all: $(addprefix build/,$(PROGRAMS))

build/%: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build
