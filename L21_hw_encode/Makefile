# 用 pkg-config 取得编译和链接参数，写法与 L02 相同。
# 每个 .c 文件编译成 $(BUILD)/ 下的一个同名可执行文件，默认 build/。
#
# 如果 pkg-config 找不到 FFmpeg，先检查环境变量：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig
#
# 带检查的构建（L22 第 1 步）：
#   make clean && make SANITIZE=address,undefined
# （不要写 make CFLAGS=…：命令行上的 CFLAGS 会盖掉下面的 -std、pkg-config 的头文件路径等。
#   要加编译选项，用环境变量 CFLAGS=… make，或者 EXTRA_CFLAGS。）
#
# 本讲不需要显卡就能编译。能不能真的用硬件编码，取决于 FFmpeg 编译时带了什么（./configure --enable-vaapi 等）
# 和机器上有没有设备，见 README 第 1 步。
# qsv_decode.c、qsv_transcode.c 要 Intel 的 oneVPL/MFX 头文件（mfxvideo.h），本讲不编译它们，只在 README 里读（选读）。

FFMPEG_LIBS = libavformat libavcodec libswscale libavutil

PROGRAMS = vaapi_encode vaapi_transcode hw_enc_info hw_encode_av nv12_linesize

BUILD    ?= build
SANITIZE ?=

CFLAGS += -std=c11 -Wall -Wextra -g $(EXTRA_CFLAGS)
ifneq ($(SANITIZE),)
CFLAGS += -O1 -fsanitize=$(SANITIZE) -fno-omit-frame-pointer
endif
CFLAGS += $(shell pkg-config --cflags $(FFMPEG_LIBS))
LDLIBS += $(shell pkg-config --libs $(FFMPEG_LIBS))

.PHONY: all clean

all: $(addprefix $(BUILD)/,$(PROGRAMS))

$(BUILD)/%: %.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $< -o $@ $(LDLIBS)

clean:
	$(RM) -r build build-*
