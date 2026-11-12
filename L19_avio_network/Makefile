# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 build/ 下的一个同名可执行文件。
# 本讲只用到 libavformat、libavcodec 和 libavutil，不需要滤镜、缩放和重采样。
# （avio_list_dir.c 包含了 libavcodec/avcodec.h，所以要带上 libavcodec。）
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig
#
# 想让 avio_serve 用 libavformat 的 avio_open_dir() 列目录（要打了 patch 0093 的 libavformat，见 README 第 9 步）：
#   make clean && make EXTRA_CFLAGS=-DUSE_AVIO_DIR
# （不要写 make CFLAGS=…：命令行上的 CFLAGS 会盖掉下面的 -std、pkg-config 的头文件路径等。）

FFMPEG_LIBS = libavformat libavcodec libavutil

PROGRAMS = avio_list_dir avio_http_serve_files avio_ls avio_cat avio_serve

CFLAGS += -std=c11 -Wall -Wextra -g $(EXTRA_CFLAGS)
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

# 官方示例 avio_http_serve_files.c 用了 fork()，-std=c11 下要这个宏才看得到它的声明
build/avio_http_serve_files: CFLAGS += -D_POSIX_C_SOURCE=200809L
# avio_serve.c 每个客户端一个线程
build/avio_serve: CFLAGS += -pthread
build/avio_serve: LDLIBS += -pthread

.PHONY: all clean

all: $(addprefix build/,$(PROGRAMS))

build/%: %.c
	@mkdir -p build
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build
