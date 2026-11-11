# L02 开发环境与命令行工具

本讲把后面 20 讲要用的环境一次搭好：从源码编译 FFmpeg，编译并运行官方示例，
写出第一个链接 FFmpeg 的程序，并熟悉 ffmpeg、ffprobe、ffplay 三个命令行工具。

完成标志：`hello_ffmpeg` 输出“环境检查通过。”。

## FFmpeg 由什么组成

| 库 | 负责什么 | 主要在哪几讲使用 |
| --- | --- | --- |
| libavutil | 公共基础：内存、日志、数学、像素与采样格式、选项系统 | 全程 |
| libavformat | 容器的解封装与封装，以及文件和网络 I/O | L03–L04、L15–L19 |
| libavcodec | 编码器与解码器 | L05–L08、L13–L14 |
| libswscale | 图像缩放和像素格式转换 | L09 |
| libswresample | 音频重采样、采样格式和声道布局转换 | L10 |
| libavfilter | 滤镜图 | L11–L12 |
| libavdevice | 采集和播放设备 | 综合项目 |

三个命令行工具都建立在这些库之上：`ffmpeg` 负责转换，`ffprobe` 负责分析，`ffplay` 负责播放。

## 第 1 步：安装依赖

```bash
sudo apt update && sudo apt install -y build-essential git pkg-config nasm cmake \
    libx264-dev libsdl2-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev \
    libva-dev libdrm-dev
```

| 软件包 | 用途 |
| --- | --- |
| build-essential、git、pkg-config、cmake | 编译器和构建工具 |
| nasm | 编译 FFmpeg 里的 x86 汇编优化代码 |
| libx264-dev | H.264 编码器（L13 起使用） |
| libsdl2-dev | 没有它就不会生成 ffplay |
| libfreetype-dev、libharfbuzz-dev、libfontconfig-dev | drawtext 文字水印滤镜（L12） |
| libva-dev、libdrm-dev | VAAPI 硬件加速（L20–L21） |

## 第 2 步：编译 FFmpeg

直接运行脚本：

```bash
scripts/build_ffmpeg.sh
```

脚本做的事情等价于下面这几条命令。建议第一次手动敲一遍：

```bash
mkdir -p ~/ffmpeg_src && cd ~/ffmpeg_src
git init -q && git remote add origin https://github.com/FFmpeg/FFmpeg.git
git fetch --depth 1 origin a337e8ddaa095a3cf82564272ec518cced09641e
git checkout --detach FETCH_HEAD

./configure \
    --prefix="$HOME/ffmpeg_build" \
    --enable-gpl --enable-libx264 \
    --enable-libfreetype --enable-libharfbuzz --enable-libfontconfig \
    --enable-shared --disable-static --enable-rpath \
    --disable-stripping

make -j"$(nproc)"
make install
make -j"$(nproc)" examples
```

在 4 核的机器上，configure 约 15 秒，make 约 4 分钟。编译后源码目录约 610 MB，
安装目录约 145 MB。

configure 选项的含义：

| 选项 | 含义 |
| --- | --- |
| `--prefix=$HOME/ffmpeg_build` | 安装到自己的目录，不覆盖系统自带的 FFmpeg，也不需要 root 权限 |
| `--enable-gpl` | 允许使用 GPL 许可的组件。加上后整个 FFmpeg 的许可证从 LGPL 变为 GPL |
| `--enable-libx264` | 启用 x264 编码器。x264 是 GPL 许可，所以必须同时指定 `--enable-gpl` |
| `--enable-libfreetype` 等三项 | 启用 drawtext 滤镜所需的字体库 |
| `--enable-shared --disable-static` | 生成动态库（.so）而不是静态库，自己的程序链接快、体积小 |
| `--enable-rpath` | 把库的安装路径写进可执行文件，运行时不必设置 `LD_LIBRARY_PATH` |
| `--disable-stripping` | 安装时保留调试符号，方便用 gdb 跟进 FFmpeg 内部 |

SDL2 和 VAAPI 不需要选项，configure 检测到开发库就会自动启用。configure 结束时会打印
启用的组件清单，最后一行是许可证：

```
License: GPL version 2 or later
```

## 第 3 步：设置环境变量并验证

把下面两行加入 `~/.bashrc`，然后重新打开终端：

```bash
export PATH="$HOME/ffmpeg_build/bin:$PATH"
export PKG_CONFIG_PATH="$HOME/ffmpeg_build/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
```

验证用的是自己编译的版本：

```bash
which ffmpeg          # 应该是 ~/ffmpeg_build/bin/ffmpeg
ffmpeg -version       # 第一行应该是 ffmpeg version git-2026-11-11-a337e8d
pkg-config --modversion libavcodec    # 63.28.100
```

## 第 4 步：生成素材并运行官方示例

```bash
cd <本仓库目录>
scripts/gen_samples.sh
```

`make examples` 生成的可执行文件在 `~/ffmpeg_src/doc/examples/` 下。先跑三个，确认整条工具链正常：

```bash
E=~/ffmpeg_src/doc/examples
mkdir -p out

# 打印容器元数据（L03 的主角）
$E/show_metadata samples/demo_1080p30.mp4

# 不重新编码，把 MP4 换成 MKV（L04 的主角）
$E/remux samples/small_360p25.mp4 out/remux.mkv
ffprobe -v error -show_entries format=format_name:stream=codec_name -of csv=p=0 out/remux.mkv

# 把 MPEG-1 裸流解码成一张张 PGM 灰度图（L05 的主角），应得到 125 张
$E/decode_video samples/video_360p25.m1v out/frame
ls out/frame-* | wc -l
```

关于示例的几点说明：

- 在本课程的配置下，`make examples` 会生成 21 个示例。`qsv_decode` 和 `qsv_transcode`
  需要 Intel 的 QSV 开发库，没有安装所以被跳过。
- 这个版本的 `make examples` 不会生成 `avio_http_serve_files`。它可以用另一种方式编译：
  `make install` 会把示例源码和一个独立的 Makefile 装到 `~/ffmpeg_build/share/ffmpeg/examples/`，
  把这个目录复制出来执行 `make` 即可（这个 Makefile 用的正是 pkg-config）。
- **已知问题**：这个版本的 `transcode` 示例运行时会报 `Cannot set output pixel format` 并退出。
  原因是示例代码还在使用 buffersink 滤镜已经删除的旧选项名。L16 会讲到并给出修正后的版本。

## 第 5 步：第一个链接 FFmpeg 的程序

[hello_ffmpeg.c](hello_ffmpeg.c) 会打印各个库的版本、许可证和 configure 选项，
统计支持的格式与编解码器数量，并检查后续课程要用到的组件是否齐全。

方式一，在本目录用 Makefile：

```bash
cd L02_environment
make run
```

方式二，在仓库根目录用 CMake（会编译所有课次）：

```bash
cmake -S . -B build
cmake --build build
./build/L02_environment/hello_ffmpeg
```

预期输出（数字会随 FFmpeg 版本和 configure 选项变化）：

```
FFmpeg 版本：git-2026-11-11-a337e8d
许可证：GPL version 2 or later

库             头文件       运行库
libavutil       61. 20.100   61. 20.100
libavcodec      63. 28.100   63. 28.100
libavformat     63. 18.100   63. 18.100
libavdevice     63.  6.100   63.  6.100
libavfilter     12. 12.100   12. 12.100
libswscale      10.  6.100   10.  6.100
libswresample    7.  7.100    7.  7.100

configure 选项：
--prefix=/home/you/ffmpeg_build --enable-gpl --enable-libx264 ...

解封装格式 364，封装格式 188
解码器 523，编码器 189
滤镜 491
输入协议 26，输出协议 21

[ OK ] H.264 解码器 h264
[ OK ] H.264 编码器 libx264 (L13)
[ OK ] AAC 编码器 aac (L14)
[ OK ] MJPEG 编码器 mjpeg (L13)
[ OK ] MP4 封装 mp4 (L15)
[ OK ] 滤镜 scale (L12)
[ OK ] 滤镜 drawtext (L12)

环境检查通过。
```

两种构建方式都靠 pkg-config 找到 FFmpeg。看一眼它给出的参数：

```bash
pkg-config --cflags --libs libavformat
# -I/home/you/ffmpeg_build/include -L/home/you/ffmpeg_build/lib -Wl,-rpath,/home/you/ffmpeg_build/lib -lavformat
```

`-I` 告诉编译器头文件在哪里，`-L` 和 `-l` 告诉链接器库在哪里，`-Wl,-rpath` 让程序运行时能找到 .so 文件。

## 命令行工具速查

`ffprobe` 可以从三个层次观察一个文件，后面每一讲都会用它来核对自己程序的结果：

```bash
# 流：有几路流，各自的编码和参数
ffprobe -v error -show_streams samples/small_360p25.mp4

# 包：解封装后、解码前的压缩数据（对应代码里的 AVPacket）
ffprobe -v error -select_streams v:0 \
    -show_entries packet=pts,dts,duration,size,flags -of csv=p=0 samples/small_360p25.mp4 | head -8

# 帧：解码后的原始数据（对应代码里的 AVFrame）
ffprobe -v error -select_streams v:0 \
    -show_entries frame=key_frame,pts,pict_type -of csv=p=0 samples/small_360p25.mp4 | head -8
```

其他常用命令：

```bash
ffmpeg -hide_banner -buildconf            # 这份 FFmpeg 的 configure 选项
ffmpeg -hide_banner -encoders | grep 264  # 查找编码器
ffmpeg -hide_banner -h encoder=libx264    # 某个编码器支持的全部选项
ffmpeg -hide_banner -filters              # 全部滤镜
ffmpeg -i in.mp4 -c copy out.mkv          # 换容器，不重新编码
ffmpeg -i in.mp4 -c:v libx264 -crf 23 -c:a aac out.mp4   # 重新编码
ffplay -autoexit samples/demo_1080p30.mp4 # 播放
```

## 常见问题

**`Package libavcodec was not found in the pkg-config search path`**
没有设置 `PKG_CONFIG_PATH`，或者设置后没有重新打开终端。

**CMake 警告“找到的 libavcodec 版本低于课程使用的 63.x”**
pkg-config 找到的是系统自带的 FFmpeg 开发包。检查 `PKG_CONFIG_PATH` 是否把
`$HOME/ffmpeg_build/lib/pkgconfig` 放在最前面。

**`error while loading shared libraries: libavcodec.so.63`**
程序运行时找不到动态库。通常是 configure 时漏了 `--enable-rpath`。
临时解决办法是 `export LD_LIBRARY_PATH=$HOME/ffmpeg_build/lib`。

**`ffmpeg -version` 显示的是 6.x**
`PATH` 里系统目录排在前面，运行的是 apt 安装的版本。用 `which ffmpeg` 确认。

**configure 报 `nasm not found or too old`**
没有安装 nasm。

**configure 报 `libx264 is gpl and --enable-gpl is not specified`**
启用 x264 时必须同时加上 `--enable-gpl`。

**没有生成 ffplay**
configure 时没有检测到 SDL2。安装 `libsdl2-dev` 后重新 configure 和 make。

## 练习

1. 在 `~/ffmpeg_src` 里只运行 `./configure`（不带任何选项，也不必接着 make），看最后一行的许可证是什么。
   再加上 `--enable-libx264` 但不加 `--enable-gpl` 试一次。做完后记得用第 2 步的完整选项重新 configure。
2. 统计 `samples/small_360p25.mp4` 里 I、P、B 帧各有多少。再对比视频流前 8 个包的 pts 和 dts，
   它们的顺序一样吗？
3. 把 `samples/small_360p25.mp4` 直接交给 `decode_video` 会发生什么？为什么？
4. 临时执行 `unset PKG_CONFIG_PATH`，再到 `L02_environment` 目录执行 `make clean && make`，读懂报错信息。

<details>
<summary>参考答案</summary>

1. 不带选项时是 `License: LGPL version 2.1 or later`。只加 `--enable-libx264` 时 configure 直接报错：
   `libx264 is gpl and --enable-gpl is not specified.`。FFmpeg 本身是 LGPL，启用 GPL 组件后
   整体变为 GPL，这会影响用它开发的程序的分发义务，所以 configure 要求明确确认。
2. 用下面的命令统计，结果是 I 帧 5 个、P 帧 46 个、B 帧 74 个，共 125 帧：

   ```bash
   ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 \
       samples/small_360p25.mp4 | sort | uniq -c
   ```

   包的 dts 是递增的（-1024、-512、0、512……），pts 却是 0、1024、512、2048、1536……。
   包按解码顺序存放，而 B 帧要参考它后面的帧，所以解码顺序和显示顺序不同。L04 会详细讲。
3. 报错 `Invalid frame dimensions 0x0` 和 `Error sending a packet for decoding`，一张图也不输出。
   `decode_video` 不做解封装，它把输入当作 MPEG-1 视频裸流直接送给解码器，而 MP4 是容器。
   L05 讲裸流和 parser，L07 把解封装和解码接起来。
4. pkg-config 报 `Package libavdevice was not found in the pkg-config search path`，
   于是编译命令里没有 `-I`、`-L`、`-l` 参数，编译器找不到头文件。

</details>
