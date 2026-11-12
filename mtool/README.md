# 综合项目：命令行媒体工具箱 mtool

前 22 讲每讲教一个接口。这个项目把它们组合成一个带子命令的 C 程序，并且按 L22 的做法做成“能交出去”的样子：ASan 下无泄漏、无越界，错误都有能看懂的提示，退出码有约定，有自动化测试。

这个目录里是**参考实现**：学员先自己做，卡住或者做完以后对照着看。每个子命令对应的课次、要练的接口都写在下面。

| 子命令 | 功能 | 来自课次 | 状态 |
| --- | --- | --- | --- |
| `mtool info` | 容器、流、GOP、帧类型统计，可输出 JSON | L3、L8 | 已完成 |
| `mtool cut` | 不转码截取一段，可同时换容器 | L4、L15 | 已完成 |
| `mtool thumb` | 缩略图、拼图 | L7、L9、L13 | 后面的提交 |
| `mtool audio` | 提取音频，转 WAV 或 AAC | L6、L10、L17 | 后面的提交 |
| `mtool transcode` | 转码，带进度，可选硬件加速 | L12、L16、L20 | 后面的提交 |
| `mtool push` | 按实际速度推流 | L19 | 后面的提交 |

## 构建和运行

环境变量和 L02 一样：

```bash
export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig PATH=$HOME/ffmpeg_build/bin:$PATH LD_LIBRARY_PATH=$HOME/ffmpeg_build/lib
make -C mtool                      # 得到 mtool/build/mtool
mtool/build/mtool --version
```

```
libavutil      61.20.100（头文件 61.20.100）
libavcodec     63.28.100（头文件 63.28.100）
libavformat    63.18.100（头文件 63.18.100）
libavfilter    12.12.100（头文件 12.12.100）
libswscale     10.6.100（头文件 10.6.100）
libswresample  7.7.100（头文件 7.7.100）
许可证：GPL version 2 or later
```

每次启动，`mtool` 都先检查编译用的头文件和运行时加载的库是不是同一份（L22 第 5 步）：大版本不同，或者运行时的库比头文件旧，就直接报错退出，不会跑到一半崩溃。

CMake 构建也可以：整个仓库 `cmake -S . -B build && cmake --build build --target mtool`。带检查的构建（L22 第 1 步）：

```bash
make -C mtool SANITIZE=address,undefined BUILD=build-asan     # ASan 加 UBSan
make -C mtool SANITIZE=thread BUILD=build-tsan                # TSan，不能和 ASan 同时开
```

要加别的编译选项用 `EXTRA_CFLAGS=…`，不要写 `make CFLAGS=…`（会盖掉 Makefile 里的 `-std` 和头文件路径，L22 里量过）。

## 目录

```
mtool/
  src/main.c        解析全局选项，分发到子命令，统一退出码
  src/util.c        错误信息、打开输入、时间解析、进度、结构化输出（文本/JSON）、日志、版本检查
  src/fflog.h       L22 的日志回调，原样拷进来
  src/cmd_info.c    info
  src/cmd_cut.c     cut
  scripts/gen_testfiles.sh   生成 18 个测试文件
  tests/testfiles.tsv        每个测试文件是好是坏、有几路流
  tests/run_tests.sh         自动化测试
  tests/helpers.py           测试脚本用的几个小帮手
```

## 约定（沿用 L22）

**退出码只有三个**：0 成功；1 失败（输入打不开、文件损坏、输出写不出来……），原因写在 stderr；2 用法错误（子命令不存在、选项不认识、缺参数）。不用 FFmpeg 的错误码当退出码（负的错误码只剩低 8 位，`AVERROR_EOF` 和 `AVERROR_EXIT` 都变成 187，分不开）。

**错误提示**统一以“mtool：”开头，说明做什么时失败、FFmpeg 的错误是什么、最可能的原因：

```
$ mtool info samples/mtool/empty.mp4
mtool：错误：打不开 samples/mtool/empty.mp4：Invalid data found when processing input（文件损坏，或者不是媒体文件）
$ mtool info samples/mtool/nonexistent.mp4
mtool：错误：打不开 samples/mtool/nonexistent.mp4：No such file or directory（文件或目录不存在）
$ mtool foo
mtool：用法错误：不认识的子命令 foo
（后面是子命令列表）
```

库自己输出的日志（例如 `moov atom not found`）也会显示，默认只显示警告以上，`-q` 只显示错误，`-v`、`-vv` 更详细。

**出错时倒出最近的日志**：加全局选项 `--log-on-error`，失败时把出错之前库输出的最近几十行日志（包括平时不显示的 debug）倒出来，不需要重新跑一遍加 `-v`（L22 第 3 步）：

```
$ mtool --log-on-error info samples/mtool/truncated_nomoov.mp4
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x…] moov atom not found
mtool：错误：打不开 samples/mtool/truncated_nomoov.mp4：Invalid data found when processing input（文件损坏，或者不是媒体文件）
库一共输出了 7 行日志，其中错误 1 行、警告 0 行。
---- 出错之前库在做什么（最近 7 行，缓冲区 40 行）----
  [debug  ] [AVFormatContext @ 0x…] Opening 'samples/mtool/truncated_nomoov.mp4' for reading
  [debug  ] [file @ 0x…] Setting default whitelist 'file,crypto,data'
  [debug  ] [mov,mp4,m4a,3gp,3g2,mj2 @ 0x…] Format mov,mp4,m4a,3gp,3g2,mj2 probed with size=2048 and score=100
  [debug  ] [mov,mp4,m4a,3gp,3g2,mj2 @ 0x…] ISO: File Type Major Brand: isom
  [debug  ] [mov,mp4,m4a,3gp,3g2,mj2 @ 0x…] ISO: File Type Major Brand: isom
  [error  ] [mov,mp4,m4a,3gp,3g2,mj2 @ 0x…] moov atom not found
  [verbose] [AVIOContext @ 0x…] Statistics: 65536 bytes read, 3 seeks
----
```

（地址 `0x…` 每次不同。）

## 测试文件

验收标准要求“至少 10 个不同格式的测试文件，含旋转视频、可变帧率、无音频、损坏文件”。`mtool/scripts/gen_testfiles.sh` 用 ffmpeg 的 lavfi 虚拟输入合成 18 个文件放到 `samples/mtool/`（不需要下载，线程数固定，两次生成逐字节相同）：

```bash
mtool/scripts/gen_testfiles.sh
```

| 类别 | 文件 | 特点 |
| --- | --- | --- |
| 好 | `h264_aac.mp4` `.mkv` `.ts` `.flv` | 同样的内容，四种容器；5 秒，GOP 25 带 B 帧 |
| 好 | `mpeg4_mp2.avi` | AVI，MPEG-4 视频加 MP2 音频 |
| 好 | `rotate90.mp4` | 显示矩阵里写了旋转 90 度，像素没动 |
| 好 | `vfr.mp4` | 可变帧率：平均 15.4 帧/秒，`r_frame_rate` 25，时间戳有空洞 |
| 好 | `video_only.mp4`、`audio_only.m4a`、`mono_8k.wav` | 没有音频；没有视频；单声道 8 kHz |
| 好 | `tiny_16x14.mp4`、`long_gop.mp4` | 很小的画面（要求 17x15，libx264 只收偶数尺寸，实际 16x14）；整段只有开头一个关键帧 |
| 打不开 | `empty.mp4` `not_media.mp4` `truncated_nomoov.mp4` `bad_header.mp4` | 空文件；一行文字；MP4 只剩前 40%（moov 在文件尾，没了）；文件头 32 字节清零 |
| 损坏但能打开 | `truncated.ts` | TS 只剩前 40%，前面的包照样能解码，只是短了 |
| 损坏但能打开 | `corrupt_mid.mp4` | 第 60 个视频包和第 100 个音频包的中间被换成伪随机数；视频有 25 帧和原文件不同（错误一直传到这个 GOP 的末尾） |

清单和类别在 `tests/testfiles.tsv`，测试脚本读这张表：**好的**必须全部处理成功；**打不开的**退出码 1、有提示、不留下输出文件；**损坏的**不崩溃。

## 自动化测试

```bash
mtool/tests/run_tests.sh                                  # 全部
mtool/tests/run_tests.sh info cut                         # 只测这几个
BUILD=mtool/build-asan mtool/tests/run_tests.sh           # 测 ASan 构建
```

每一次运行都检查有没有 sanitizer 报告，每一项“算通过”的标准写死在脚本里。数字都是和 ffprobe、ffmpeg 命令行对出来的，不是“看着差不多”。当前（`info`、`cut`）共 323 项，普通构建和 ASan 加 UBSan 构建都全部通过，各约 10 秒。

## mtool info

```bash
mtool info [--json] [--no-decode] <文件>
```

输出容器（名字、时长、大小、码率、元数据）和每一路流（编码、参数、旋转、帧率、元数据），再加整个文件的统计：

- 每一路流：包数、字节数。
- 视频：关键帧数；**GOP 长度**（按包数算，包按解码顺序排，所以等于帧数）的个数、最短、平均、最长；是不是**可变帧率**，以及帧间隔的最小和最大值；解码后的帧数、**I、P、B 帧数**，解码出错的次数、解码器标记为损坏的帧数。

```
$ mtool info samples/mtool/h264_aac.mp4
  format:
    filename: samples/mtool/h264_aac.mp4
    format_name: mov,mp4,m4a,3gp,3g2,mj2
    duration: 5.000
    …
  streams:
    streams[0]:
      index: 0
      type: video
      codec: h264
      profile: High
      …
      width: 640
      height: 360
      pix_fmt: yuv420p
      avg_frame_rate: 25.000
      rotation: 0
      reorder_delay: 2
      stats:
        packets: 125
        bytes: 481605
        keyframes: 5
        gop:
          count: 5
          min: 25
          avg: 25.0
          max: 25
        variable_frame_rate: 否
        frame_interval:
          min: 0.0400
          max: 0.0400
        decoded_frames: 125
        frame_types:
          I: 5
          P: 46
          B: 74
        decode_errors: 0
        corrupt_frames: 0
```

`--json` 输出 JSON（缩进两个空格，数字是数字、布尔是布尔），能打开的 14 个测试文件（12 个好的和 2 个损坏的）的输出都用 `python3 -m json.tool` 验证过是合法的 JSON；打不开的 4 个没有输出，退出码 1。元数据里的字符串会转义引号、反斜杠和控制字符，不是合法 UTF-8 的字节换成 `?`。**不知道的值不输出**（例如容器没写时长），不输出一个假的。

### 几个值怎么判断的

- **GOP 长度**：两个关键帧包之间有多少个包。最后一个 GOP 没有“下一个关键帧”结束它，按到文件尾的包数算。`long_gop.mp4` 整个文件只有 1 个关键帧，GOP 数是 1，长度 125。
- **可变帧率**：把所有视频包的显示时间排序，相邻两个的差里，最大减最小超过“2 个时间基刻度和平均间隔的 5%”里较大的那个，就算可变。`vfr.mp4` 的间隔在 0.04 到 0.2 秒之间，判为可变；基准文件所有间隔都是 0.04 秒；MKV 的时间基是 1/1000，25 帧/秒时间戳是 40 毫秒整，29.97 帧/秒会在 33、34 之间跳，差 1 个刻度，不能算可变，所以有 2 个刻度的容差。
- **旋转**：显示矩阵（`AV_PKT_DATA_DISPLAYMATRIX`）经 `av_display_rotation_get()` 取整，和 `ffprobe` 显示的一致：`rotate90.mp4` 是 90。
- **解码出错**：`avcodec_send_packet()` 或 `avcodec_receive_frame()` 返回错误的次数（解码器通常会掩盖错误继续往下解，所以是 0 也不代表码流是好的）；**损坏的帧**是解码器自己给帧打了 `AV_FRAME_FLAG_CORRUPT` 或 `decode_error_flags` 的帧数。`corrupt_mid.mp4` 里：解码出 125 帧，损坏的 1 帧，解码出错 0 次，但有 25 帧和原文件不同。

### 和 ffprobe 对

每一个能打开的视频文件，视频的包数、关键帧数、I/P/B 帧数都和 `ffprobe -show_packets`、`-show_frames` 的结果一致（测试里逐项对，包括 `truncated.ts` 和 `corrupt_mid.mp4`）：基准文件 125 个包、5 个关键帧、I 5、P 46、B 74；`mpeg4_mp2.avi` 125 个包、11 个关键帧、I 11、P 114、B 0；`vfr.mp4` 77 个包。

`--no-decode` 不解码，只统计包：快得多，但没有 `frame_types`、`decode_errors`。读包读到一半出错（文件被截断）不算失败：报告读到的部分，stderr 上说明。

## mtool cut

```bash
mtool cut [-s 起点] [-e 终点 | -d 时长] [--video-only | --audio-only] [-f 格式] [--faststart] [-y] <输入> <输出>
```

不解码、不编码，把输入的一段原样装进输出，输出的容器由扩展名决定（`-f` 指定）。时间可以写 `90`、`1.5`、`1:30`、`00:01:30.5`。

```
$ mtool cut -y -s 2.5 -e 4 samples/mtool/h264_aac.mp4 out/b.mp4
截取完成：samples/mtool/h264_aac.mp4 → out/b.mp4（mp4）
  请求的起点 2.500 秒，实际从 2.000 秒的关键帧开始
  video 流 #0 → #0：52 个包
  audio 流 #1 → #1：86 个包
```

**起点只能落在关键帧上**（L04）：实际起点是 `-s` 之前最近的关键帧，所以输出可能比要求的长，摘要里会说实际从哪里开始。TS 没有索引，实际起点可能在 `-s` 之后（`h264_aac.ts` 的 `-s 1.5` 实际从 2.023 秒开始）。**终点按 dts 判断**，留下的每个包都能解码，所以有 B 帧时终点附近多出重排延迟的几帧（2 到 4 秒的截取是 52 个视频包，不是 50）。要精确到帧，用 `mtool transcode`。

做了的事（L04、L15）：流参数整个拷过去，`codec_tag` 清零让复用器重选；语言等流元数据和 disposition 一起带过去；旋转（显示矩阵）带到输出里（`rotate90.mp4` 截出来还是 90）；开头几个包没有 dts 的（MKV）补上；容器装不下的编码直接报错（`mpeg4_mp2.avi` 到 `.flv`：装不下 MP2），不写出一个坏文件；写文件头之前的时间基只是建议，写完文件头以后换算时间戳用复用器给的。

出错时**不留下半个输出文件**：打不开输入、容器装不下、起点超过文件长度（`-s 99` 对 5 秒的文件）、写失败，都会把这次创建的输出文件删掉。输出文件已经存在不加 `-y` 会拒绝；输入和输出是同一个文件会拒绝（否则输入会被写坏）。

### 测试怎么判断“对”

截取出来的视频包，是输入里**连续的一段**，并且第一个包是关键帧：用 `ffmpeg -c copy -f framecrc` 取每个包数据的 CRC，输出的 CRC 序列必须是输入的 CRC 序列里连续的一段。16 种输入（12 个能处理的文件各截一次，加 4 个换容器）都满足。三处例外：

- TS 里的 H.264 是 Annex B 格式（复用器加的），数据和 MP4 里的不同，所以 mp4 截成 ts 要和“mp4 拷贝成 ts”的文件（`h264_aac.ts`）比；
- WAV 的 PCM 包是按读的位置重新切的（不是输入包的边界），所以比的是 PCM 数据：输出等于输入从 1.5 秒起的一段（逐字节）；
- 取 CRC 时只取包数据，不含 `Skip Samples` 这类附加数据（MP4 复用器会给最后一个音频包加）。

## 练习

1. 给 `info` 加一个 `--frames` 选项，把每一帧的类型、pts、大小打印出来（参考 L08 的 `frame_info`）。想一想：JSON 输出里怎么表示？（数据量会很大，要不要一行一个？）
2. `cut` 的终点是按 dts 判断的，有 B 帧时会多出几帧。改成“按 pts 判断，多余的 B 帧丢掉”会遇到什么问题？（提示：丢掉一个 P 帧却留下依赖它的 B 帧。）
3. `info` 判断可变帧率的容差（2 个刻度、5%）是怎么来的？自己造一个 29.97 帧/秒的文件，看看在 1/1000 时间基的 MKV 里判断得对不对。
4. 把 `mtool info` 的统计改成不用全局数组、不把所有 pts 存下来（边读边算最小最大间隔），可以吗？（提示：包的 pts 不是单调的，有 B 帧时会先出后面的帧。）
