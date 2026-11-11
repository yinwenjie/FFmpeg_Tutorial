# 用 pkg-config 取得编译和链接参数，写法与 FFmpeg 自带的
# doc/examples/Makefile.example 相同。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

FFMPEG_LIBS = libavdevice libavfilter libavformat libavcodec \
              libswresample libswscale libavutil

CFLAGS += -std=c11 -Wall -Wextra -g
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

.PHONY: all run clean

all: build/hello_ffmpeg

build/hello_ffmpeg: hello_ffmpeg.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

run: build/hello_ffmpeg
	./build/hello_ffmpeg

clean:
	$(RM) -r build
