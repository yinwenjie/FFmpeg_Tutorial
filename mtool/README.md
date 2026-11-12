# 综合项目：命令行媒体工具箱 mtool

前 22 讲每讲教一个接口。这个项目把它们组合成一个带子命令的 C 程序，并且按 L22 的做法做成“能交出去”的样子：ASan 下无泄漏、无越界，错误都有能看懂的提示，退出码有约定，有自动化测试。

这个目录里是**参考实现**：学员先自己做，卡住或者做完以后对照着看。每个子命令对应的课次、要练的接口都写在下面。

| 子命令 | 功能 | 来自课次 | 状态 |
| --- | --- | --- | --- |
| `mtool info` | 容器、流、GOP、帧类型统计，可输出 JSON | L3、L8 | 已完成 |
| `mtool cut` | 不转码截取一段，可同时换容器 | L4、L15 | 已完成 |
| `mtool thumb` | 缩略图、拼图 | L7、L9、L12、L13 | 已完成 |
| `mtool audio` | 提取音频，转 WAV、AAC、FLAC、MP2 | L6、L10、L14、L17 | 已完成 |
| `mtool transcode` | 转码，带进度，可选 VAAPI 硬件编码 | L12、L16、L21 | 已完成（硬件部分没有在真实设备上验证，见【待补 1】） |
| `mtool push` | 按播放速度推流到 RTMP、UDP、TCP | L15、L19 | 已完成 |

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
  src/media.c       解码器和滤镜图的公共部分（打开解码器、建 buffer→…→buffersink 的图、旋转滤镜），thumb 等用
  src/cmd_thumb.c   thumb
  src/cmd_audio.c   audio
  src/cmd_transcode.c   transcode
  src/cmd_push.c    push
  scripts/gen_testfiles.sh   生成 18 个测试文件
  scripts/check.sh           一条命令：普通加 ASan 两份构建（-Werror），各跑全部测试；CI 跑的就是它
  scripts/verify_hw.sh       有 VAAPI 设备时验证 transcode --hw vaapi（没有设备退出码 77）
  tests/testfiles.tsv        每个测试文件是好是坏、有几路流
  tests/run_tests.sh         自动化测试
  tests/helpers.py           测试脚本用的几个小帮手
.github/workflows/mtool.yml   CI（在仓库根目录下）
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
mtool/tests/run_tests.sh thumb audio                      # 只测这几个（common info cut thumb audio transcode push）
BUILD=mtool/build-asan mtool/tests/run_tests.sh           # 测 ASan 构建
```

每一次运行都检查有没有 sanitizer 报告，每一项“算通过”的标准写死在脚本里。数字都是和 ffprobe、ffmpeg 命令行对出来的，不是“看着差不多”。当前共 1360 项（`common` 12、`info` 150、`cut` 161、`thumb` 166、`audio` 335、`transcode` 320、`push` 216），普通构建和 ASan 加 UBSan 构建都全部通过，各约 85 到 90 秒（`push` 一节要按播放速度推流，占其中约 18 秒）。这个总数是在没有 `/dev/dri` 的机器上数的：有 `/dev/dri` 时，`transcode` 里 13 项“没有显卡时 `--hw vaapi` 要失败、`--hw auto` 要退回软件”的检查会跳过（那时有没有设备要看设备本身），脚本会打印一行说明；GitHub Actions 上跑出来是 1347 项，差的正好是这 13 项。一条命令做完编译加两份测试：`mtool/scripts/check.sh`（约 3 分钟）。`push` 的测试要在本机起 `ffmpeg` 当接收端：端口从 21000 到 41000 之间随机选一个起点，每个接收端往上用一个，共几十个。

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

## mtool thumb

```bash
mtool thumb [-i 间隔秒 | -k] [-w 宽 | -s 宽x高] [-m 最多张数] [--format jpg|png] [-q 2-31] [--no-rotate] [-y] <输入> <输出目录>
mtool thumb --sheet 列x行 [同上] <输入> <输出图片>
```

不加 `--sheet`，输出是一个目录，里面是 `thumb_0001.jpg`、`thumb_0002.jpg`……，每张图后面打印它在视频里的时间；加 `--sheet`，所有缩略图拼成一张图（输出文件的扩展名 `.jpg` 或 `.png` 决定格式）。

```
$ mtool thumb -y -i 1 samples/mtool/h264_aac.mp4 out/thumbs
out/thumbs/thumb_0001.jpg  0.000 秒
out/thumbs/thumb_0002.jpg  1.000 秒
out/thumbs/thumb_0003.jpg  2.000 秒
out/thumbs/thumb_0004.jpg  3.000 秒
out/thumbs/thumb_0005.jpg  4.000 秒
共 5 张，图片 320x180
$ mtool thumb -y -k samples/mtool/vfr.mp4 out/key
out/key/thumb_0001.jpg  0.000 秒
out/key/thumb_0002.jpg  1.200 秒
out/key/thumb_0003.jpg  2.840 秒
out/key/thumb_0004.jpg  4.000 秒
out/key/thumb_0005.jpg  4.800 秒
共 5 张，图片 320x180
$ mtool thumb -y --sheet 4x3 samples/mtool/h264_aac.mp4 out/sheet.jpg
拼图 out/sheet.jpg：4 列 x 3 行，每格取自视频的一帧，图片 1300x556
```

三种取法：**按间隔**（`-i`，不给就是每秒一张）取到了时间点之后的第一帧；**关键帧**（`-k`）每个关键帧一张，而且解码器设成只解关键帧（`skip_frame = AVDISCARD_NONKEY`），不解码其他帧，快得多（L8）；**拼图**不给 `-i`、`-k` 时把整个视频平均分给所有格子，每格取所在时间段中间的帧，所以 12 格盖住整个视频，不是挤在开头。

按间隔取是**按帧的时间**，不是按帧号，所以可变帧率的视频也对：`mtool thumb -i 0.437 vfr.mp4` 取的帧，和“用 ffprobe 取出每一帧的时间，再按同样的规则算一遍”完全一致（测试里逐项对）。规则：取第一个时间不小于“下一个时间点”的帧，取了以后下一个时间点 = (floor(这一帧的时间 / 间隔) + 1) × 间隔。基准文件是 25 帧每秒，0.5 秒不是帧的时间点，所以 `-i 0.5` 取到的是 0.00、0.52、1.00、1.52……

**每一张图都经过同一张滤镜图**（L12）：`[旋转,]scale=宽:-2,setsar=1[,tile=列x行:margin=4:padding=4]`。旋转、缩放、转像素格式都在图里做，出口只接受编码器支持的像素格式，格式对不上的时候图自己插入 scale（L9）。拼图的尺寸是 列×宽 + 2×4 + (列−1)×4，行同理：4x3、每格 320x180 是 1300x556，旋转的视频 `-w 200` 的 4x3 是 820x1084。视频里只有 5 个关键帧而拼图要 6 格时，最后一格补黑，图的大小不变。

**旋转按视频里的显示矩阵转正**（`rotate90.mp4` 出来是 320x568，不是 320x180），做法和 ffmpeg 命令行的自动旋转一样：显示矩阵的旋转角取负、归到 0 到 360，90 度用 `transpose=clock`，270 度用 `transpose=cclock`，180 度用 `hflip,vflip`。加 `--no-rotate` 保持原样（320x180）。

### 做的时候踩过的三个坑

- **转正以后要把显示矩阵从帧上去掉**。滤镜只改了像素，帧上还带着“旋转 90 度”的附加数据；PNG 编码器会把它写成 EXIF 方向（`eXIf` 块），看图软件再转一次，图又歪了。测试里检查 PNG 里没有 `eXIf`。顺带一个测试方法上的坑：用 ffmpeg 读 PNG 回来比较时也要加 `-noautorotate`，否则 ffmpeg 会按 `eXIf` 再转一次，两张一样的图比出“差 128”。
- **关键帧模式必须冲刷解码器**。帧线程和 B 帧重排会让解码器里留着最后几帧，不发 `NULL` 包把它们取出来，5 个关键帧只能取到 3 个（最初的实现就是这样，测试里 `-k` 要和 ffprobe 的关键帧时间逐个对才发现的）。
- **`-q` 要设帧的 `quality`**。JPEG 的固定质量模式下，mjpeg 编码器用每一帧自己的 `quality`；不设的话帧上带的是解码器填的值，`-q 2` 和 `-q 31` 出来的文件一样大。设了以后，默认质量的第一张 JPEG 和 `ffmpeg -i … -frames:v 1 -vf scale=320:-2 -q:v 3` 的结果逐字节相同。

另外：mjpeg 编码器只接受全范围的 YUV（0 到 255），而 H.264 视频一般是标称范围（16 到 235），所以 JPEG 模式的 `scale` 要加 `out_range=full`，否则编码器打开时报 “Non full-range YUV is non-standard”。

### 测试怎么判断“对”

- 每个能打开的、有视频的文件取 3 张：退出码 0，每张图能被 ffmpeg 完整解码、宽是 320；没有视频的文件（`audio_only.m4a`、`mono_8k.wav`）退出码 1，说明“没有视频流”；打不开的文件退出码 1，不留下输出。
- 张数和时间点：基准文件每秒一张是 0 到 4 秒共 5 张；`-k` 的时间和 `ffprobe` 的关键帧时间逐个相同（基准文件 5 个，`vfr.mp4` 是 0、1.2、2.84、4.0、4.8 秒）。
- **图片的内容**：`-k --format png` 取的 5 张图，逐像素和 `ffmpeg -vf select=key,scale=320:-2,setsar=1` 出来的相同；旋转的视频和 ffmpeg 自动旋转的结果逐像素相同；JPEG 和命令行 `-q:v` 的结果逐字节相同。
- 尺寸：`-w`、`-s`、拼图的大小都是算出来的具体数字（上面列的）。
- 该拒绝的：目录里已有图片或拼图文件已存在不加 `-y`、输出是个已有的文件、输出目录建不出来、各种选项不合法（退出码 2，且不留下输出）。

## mtool audio

```bash
mtool audio [-r 采样率] [-c 声道数] [-b 码率] [--codec 编码器] [-s 起点] [-e 终点 | -d 时长] [--track 序号] [-f 格式] [-y] <输入> <输出>
```

把输入里的一路音频解码，转成指定的采样率、声道数和编码，写成音频文件。输出的编码由扩展名决定：`.wav` PCM 16 位，`.m4a` `.mp4` `.aac` AAC，`.flac` FLAC，`.mp2` MP2；`--codec` 可以换成别的音频编码器（`pcm_s24le`、`pcm_f32le`……），`-f` 指定容器时编码取容器默认的。这份 FFmpeg 没有 libmp3lame，所以没有 MP3。

```
$ mtool audio -y samples/mtool/h264_aac.mp4 out/a.wav
音频提取完成：samples/mtool/h264_aac.mp4 → out/a.wav（wav）
  输入：aac 44100 Hz stereo，第 1 路流
  输出：pcm_s16le 44100 Hz stereo
  时长 5.000 秒（220500 个采样），216 个包
$ mtool audio -y -r 16000 -c 1 samples/mtool/h264_aac.mp4 out/b.wav
  输出：pcm_s16le 16000 Hz mono
  时长 5.000 秒（80000 个采样），217 个包
$ mtool audio -y -b 96k samples/mtool/h264_aac.mp4 out/c.m4a
  输出：aac 44100 Hz stereo，96 kb/s
  时长 5.000 秒（220500 个采样），217 个包
$ mtool audio -y -r 7000 samples/mtool/h264_aac.mp4 out/x.m4a
mtool：错误：编码器 aac 不支持采样率 7000 Hz，它支持：96000、88200、64000、48000、44100、32000、24000、22050、16000、12000、11025、8000、7350
```

做法是 L17 的整条链路，每一步对应一课：

1. **先定编码器的参数，再建滤镜图**（L10、L14）：采样率、声道布局、采样格式都要是编码器支持的。用户给了的必须满足，不满足就报错并列出支持的（上面 7000 Hz 的例子）；没给的跟输入，输入的不支持（例如 MP2 没有 8000 Hz）取最接近的并警告。采样格式在编码器支持的里面优先选和输入同样位宽的，所以 24 位的输入转 FLAC 不会被截成 16 位。
2. **解码 → 滤镜图 → 编码 → 复用**：图是 `[atrim=…,]aformat=sample_fmts=…:sample_rates=…:channel_layouts=…`，重采样和混声道由图自己插入 aresample（L10）；出口按编码器的帧长切帧（`av_buffersink_set_frame_size`，AAC 是 1024，L14）；编码器要 extradata 放文件头的容器（M4A）要设 `AV_CODEC_FLAG_GLOBAL_HEADER`（L15）。
3. **帧的时间戳自己数**（L17）：编码前 pts = 已经送去编码的采样数，时间基 1/采样率；AAC 的预热帧由复用器记在编辑列表里，所以 M4A 解码回来的采样数和输入相同（测试里逐字节比）。
4. **冲刷顺序是解码器 → 滤镜图 → 编码器**（L11、L17）：先送 `NULL` 包把解码器里留着的帧取出来，再通知滤镜图输入结束，最后给编码器送 `NULL` 帧。少任何一步，最后几十毫秒的声音就丢了。

### 和 ffmpeg 命令行逐字节对

- 转成 WAV：12 个能处理的文件里有音频的 10 个，PCM 和 `ffmpeg -i … -vn -f s16le -` 解码出来的**逐字节相同**（包括 TS、FLV 里 AAC 没有预热信息、解码出来多 1708 个采样的情况：`h264_aac.flv` 是 5.039 秒、222208 个采样，`h264_aac.mkv` 是 5.000 秒、220500 个采样，和 ffmpeg 一样，不是 mtool 的毛病）。
- 改采样率和声道：`-r 16000 -c 1`、`-r 8000 -c 1`、`-r 48000 -c 2`、`-r 22050 -c 2`，以及单声道 8 kHz → 立体声 44.1 kHz，都和 `ffmpeg -ar … -ac …` 逐字节相同（同样是 aresample，默认参数一样）。
- FLAC 是无损：解码回来和输入的 PCM 逐字节相同（5 秒的 WAV 882078 字节，FLAC 71812 字节）。
- AAC 是有损，比的是码率和信噪比：`-b 64k` 实际 56557 b/s，`-b 96k` 实际 93358 b/s，`-b 128k` 实际 120968 b/s（AAC 编码器的实际码率比请求的低一点，测试里允许请求的 80% 到 105%）；解码回来和输入的信噪比 64k 是 24.4 dB，96k 是 47.9 dB，128k 是 53.8 dB（测试里要求 96k 不小于 25 dB）。基准文件的声音是合成的音调，信噪比比真实的音乐高很多，只能说明链路没有错位或丢采样，不代表音质。

### 截取

`-s`、`-e`、`-d` 是**解码以后按采样裁**的（滤镜 `atrim`），不像 `mtool cut` 只能落在关键帧上：输出的 PCM 是完整解码结果里从起点开始的一段，起点差不超过 1 个采样（测试里 7 种输入各截两段，逐字节比，并且长度要和要求的采样数一样）。三个细节：

- **帧的 pts 要先换成 1/采样率 的时间基再进滤镜图**。解码出来的 pts 以流的时间基为单位：AVI 里的 MP2 一个单位是一帧（1152 个采样），TS 是 1/90000，不换的话 `atrim` 的起点终点全错（最初的实现里 `-s 1` 对 MP2 的 AVI 什么都截不出来）。
- **进图以后自己数 pts**。FLV、MKV 的时间基是 1/1000 秒，一个 AAC 帧是 23.219 毫秒，每一帧的 pts 都舍入到毫秒，换回采样差几个到二十几个，逐帧用它裁剪起点终点就差几个采样（最初的实现 `-s 1 -d 2` 少了 3 到 4 个采样）。现在只有第一帧用容器的 pts，之后自己数；和自己数的差超过 20 毫秒才认为时间戳跳变，重新取容器的。
- **定位只在时间基够细时做**。起点之前 1 秒开始解码，让有重叠的编码（AAC 的 MDCT）在起点处解对；但定位以后第一帧的时间要取容器里的，时间基比一个采样粗（FLV、MKV）时取出来是不准的，这种流不定位，从头解码（音频解码是实时的几百倍，多花的时间可以忽略）。
- 起点不是整采样时（`-s 4.9999` 是第 220495.59 个采样），FLV 和 TS 的第一帧时间本身是毫秒舍入的（`h264_aac.flv` 的第一个音频包是 0.057 秒），所以“第 0 个采样”的位置有不到 1 个采样的不确定，这就是测试允许的 ±1。

### 出错

没有音频流、`--track` 超过音频流数、起点超过文件长度、采样率或声道数编码器不支持、扩展名认不出来、WAV 里放 AAC、输出已存在不加 `-y`、输入输出是同一个文件，都是退出码 1，stderr 说明原因，不留下输出文件（出错时 `unlink` 掉这次创建的文件，和 `cut` 一样）。损坏但能打开的文件（`truncated.ts`、`corrupt_mid.mp4`）退出码 0：解码出错的包计数，最后警告一次“解码时遇到 N 个出错的包，对应的声音可能有杂音或缺一段”，输出文件本身是完整、能解码的。

```
$ mtool audio -y samples/mtool/truncated.ts out/dm.m4a
mtool：警告：解码时遇到 1 个出错的包，对应的声音可能有杂音或缺一段
音频提取完成：samples/mtool/truncated.ts → out/dm.m4a（ipod）
  输入：aac 44100 Hz stereo，第 1 路流
  输出：aac 44100 Hz stereo，128 kb/s
  时长 1.858 秒（81920 个采样），81 个包
```

## mtool transcode

```bash
mtool transcode [选项] <输入> <输出>
```

转码：换编码、换容器、改码率、缩放、加滤镜、裁剪，带进度。只处理第一路视频和第一路音频，字幕等丢掉。默认的编码器：视频先试 H.264（libx264；有 `--hw` 的设备时是 h264_vaapi），输出格式装不下就用这种格式自己的默认视频编码器；音频先试 AAC，装不下就用格式自己的默认音频编码器，再不行用 MP2。MP4、MKV、FLV、TS、AVI 的默认都是 H.264 加 AAC。用 `--vcodec`、`--acodec` 换；选项的完整列表见 `mtool transcode --help`。

```
$ mtool transcode -y samples/mtool/h264_aac.mp4 out/t1.mp4
转码 100.0%  已处理 5.0 秒，125 帧，速度 9.64x
转码完成：samples/mtool/h264_aac.mp4 → out/t1.mp4（mp4）
  视频：h264 640x360 → libx264 640x360 yuv420p，125 帧，25.000 帧/秒，约 742 kb/s
  音频：aac 44100 Hz → aac 44100 Hz stereo，217 个包，约 122 kb/s
  时长 5.000 秒（要求的 5.000 秒，相差 0.000 秒），用时 0.53 秒（9.5 倍速）
$ mtool transcode -y -w 320 --crf 28 --preset veryfast samples/mtool/h264_aac.mp4 out/t2.mp4
  视频：h264 640x360 → libx264 320x180 yuv420p，125 帧，25.000 帧/秒，约 92 kb/s
$ mtool transcode -y -s 1.5 -d 2 samples/mtool/h264_aac.mp4 out/t3.mp4
  视频：h264 640x360 → libx264 640x360 yuv420p，49 帧，25.000 帧/秒，约 806 kb/s
  时长 2.000 秒（要求的 2.000 秒，相差 0.000 秒），用时 0.28 秒（7.1 倍速）
$ mtool transcode -y samples/mtool/rotate90.mp4 out/t4.mp4
  视频：h264 640x360 → libx264 360x640 yuv420p，125 帧，25.000 帧/秒，约 699 kb/s
$ mtool transcode -y samples/mtool/vfr.mp4 out/t5.mp4
  视频：h264 640x360 → libx264 640x360 yuv420p，77 帧，可变帧率，约 511 kb/s
  时长 5.000 秒（要求的 5.000 秒，相差 0.000 秒），用时 0.32 秒（15.8 倍速）
```

做法是 L16 的转码程序加上一个工具该有的东西，每一步对应一课：

1. **每一路流一条流水线**：解码（L5）→ 滤镜图（L12）→ 编码（L13、L14）→ 复用（L15）。滤镜图的描述是 `[trim,setpts,][转正,][--vf,][scale,][fps,][format=nv12,hwupload]`，音频是 `[atrim,asetpts,][--af,][aformat=…]`，都是按用户给的选项拼出来的；图的出口只接受编码器支持的格式，对不上时图自己插 `scale`、`aresample`。
2. **先建图，再开编码器**：编码器的宽高、像素格式、帧率、采样率、声道布局都从图的出口读（`av_buffersink_get_*`），不是照抄输入。所以 `-w 320`、`--ar 16000`、转正之后宽高互换，编码器都能拿到对的参数。
3. **时间戳**（L16、L17）：固定帧率时编码器的时间基是帧率的倒数，每帧 pts 差 1，送去编码的帧带上 `duration`（`AV_CODEC_FLAG_FRAME_DURATION`，不然 MP4 最后一帧没有时长，patch 0011）；音频帧的 pts 用 `av_rescale_delta` 接成连续的（FLV、MKV 的毫秒时间基把每一帧都舍入过）。可变帧率的输入（`r_frame_rate` 和 `avg_frame_rate` 差 2% 以上）保持可变：编码器用滤镜图出口的时间基，每一帧的时间原样保留。
4. **精确裁剪**（L16）：`-s` 之前 1 秒（有音频时）定位，从那里解码，解出来的帧交给 `trim`、`atrim` 精确裁到帧和采样；`-e`、`-d` 同样。固定帧率会把时间戳取整到帧的格子上，起点不在格子上时（`-s 1.5`，25 帧每秒），裁剪放过的最后一帧取整后可能正好落在终点上，所以**输出里时间在终点之后的视频帧不要**，输出时长不会超过要求的。
5. **转正**：和 ffmpeg 命令行自动旋转同样的做法（见 `mt_rotation_filter`），转完把帧上的 `AV_FRAME_DATA_DISPLAYMATRIX` 去掉，否则编码器或复用器把它写进输出，播放器转第二次（和 `thumb` 的 PNG 是同一个坑）。`--no-rotate` 反过来：像素不动，旋转信息复制到输出流上，`ffprobe` 看到的还是 90 度。
6. **容器的要求**：复用器要全局头（MP4、MKV、FLV……）就给编码器设 `AV_CODEC_FLAG_GLOBAL_HEADER`；`oc->max_delay` 设成 0.7 秒（ffmpeg 的 `-muxdelay` 默认值，MPEG-TS 的 PCR 时间从它来，不设的话 TS 的起始时间和命令行差 0.7 秒）；输出的扩展名是只放音频的（`.m4a`、`.wav`、`.flac`……）而输入有视频，丢掉视频并警告；`--faststart` 只对 MP4、MOV 有用，别的容器直接报错，不是悄悄忽略。
7. **自检**：转完以后把输出文件重新打开，读它的时长，和要求的比，差超过 0.1 秒就警告（验收标准）。上面输出里的“相差 0.000 秒”就是这么来的，不是程序自己算的估计值。

### 和 ffmpeg 命令行对

`--threads 1` 的 mtool 和同样设置的 `ffmpeg -threads 1 -c:v libx264 -c:a aac` 比，24 种设置里 **22 种输出文件逐字节相同**：MP4 的 16 种（默认、AVI 输入、TS 输入、旋转的输入、没有音频、很小的画面、`--crf 28 --preset veryfast -w 320`、`--vb 1500k --ab 96k`、`--gop 12`、`--ar 16000 --ac 1`、`--vf eq=brightness=0.1`、`--af volume=0.5`、`--vopt tune=film --aopt aac_coder=twoloop` 这 13 种，加 `-s 1 -d 2`、`-s 1.5 -d 2`、`-s 2.5 -e 4.25` 三种裁剪），M4A 两种（`audio_only.m4a` 和单声道 8 kHz 的 WAV 转 AAC），FLV、TS 各一种，AVI 两种（H.264 加 AAC，MPEG-4 加 MP2）。剩下 2 种的差别都有原因：

- **MKV**：每次写的 `SegmentUID`、`TrackUID` 和它们的 CRC-32 是随机的，不可能逐字节相同。测试里比的是：文件大小相同；用 L22 的 `ebml_dump.py` 列出头部元素，除了这三个随机值，名字、大小、内容逐个相同；视频包、音频包的解码时间戳、显示时间戳、时长、大小、CRC 逐包相同。
- **可变帧率的 MP4**（`vfr.mp4`）：包逐个相同，文件大 1 字节：命令行把编码器的时间基设成 1/帧率，x264 写进 SPS 的 VUI 时间信息是 1/50；mtool 用滤镜图出口的时间基（1/12800），保持每一帧原来的时间，不取整到帧格子上，SPS 里的时间信息是 1/25600。`vfr.mp4` 的每一帧恰好都在 1/25 的格子上，所以两边的时间戳一样；真正的可变帧率（屏幕录像之类，时间戳不在格子上）命令行会取整，mtool 不会。

踩过的坑（都是和命令行对出来的）：

- **MKV 少了 `DefaultDuration`、FLV 元数据少了 `framerate`**：输出流的 `avg_frame_rate` 没设（15 字节和 20 字节的差别就是这两项）。
- **MKV 少了 `Colour` 里的 `Range`、`FlagInterlaced`**：没有 VUI 的 H.264 解码器不会说颜色范围，但解出来的帧标的是电视范围；编码器的 `color_range`、`field_order` 要自己补上。
- **TS 的起始时间差 0.7 秒**：`max_delay`，见上。
- **`avformat_query_codec` 说“不知道”（负数）**：别当成“不行”。TS 容器对每一种编码都答“不知道”，当成“不行”的话默认编码器选成了 MPEG-2 加 MP2。
- **MP4 最后一帧没有时长**：送给编码器的帧不带 `duration`，编码器出来的包就没有时长，MP4 最后一帧的时长不对（patch 0011），所以设 `AV_CODEC_FLAG_FRAME_DURATION`，每一帧都给 `duration`。

### 时长误差和音视频同步

验收标准：时长误差小于 0.1 秒，没有明显的音视频不同步。每个测试文件转成 MP4，用 ffprobe 读输入和输出的时长，有音视频两路的再比两路流的开头和时长：

| 文件 | 输入时长 | 输出时长 | 误差 | 音视频开头差 | 音视频时长差 |
| --- | --- | --- | --- | --- | --- |
| `h264_aac.mp4` | 5.000 | 5.000 | 0.000 | 0.000 | 0.000 |
| `h264_aac.mkv` | 5.038 | 5.000 | 0.038 | 0.000 | 0.000 |
| `h264_aac.ts` | 5.039 | 5.040 | 0.001 | 0.040 | 0.039 |
| `h264_aac.flv` | 5.095 | 5.040 | 0.055 | 0.040 | 0.039 |
| `mpeg4_mp2.avi` | 5.016 | 5.016 | 0.000 | 0.000 | 0.016 |
| `rotate90.mp4`、`vfr.mp4`、`long_gop.mp4` | 5.000 | 5.000 | 0.000 | 0.000 | 0.000 |
| `video_only.mp4`、`audio_only.m4a` | 5.000 | 5.000 | 0.000 | — | — |
| `mono_8k.wav` | 3.000 | 3.000 | 0.000 | — | — |
| `tiny_16x14.mp4` | 2.000 | 2.000 | 0.000 | — | — |
| `truncated.ts`（损坏） | 2.063 | 2.080 | 0.017 | 0.040 | 0.182 |
| `corrupt_mid.mp4`（损坏） | 5.000 | 5.000 | 0.000 | 0.000 | 0.000 |

最大误差是 `h264_aac.flv` 的 0.055 秒：输入的“5.095 秒”是 FLV 里最后一个音频帧的时间戳加上它的时长，输出是 5.040 秒（视频 5.000 加上 AAC 预热的 0.040）。有音视频两路的 8 个好文件，开头差和时长差都不到 0.1 秒，最大是 TS、FLV 的 0.040 和 0.039（TS、FLV 里的 AAC 没有预热信息，解码出来比 MP4 的多 1708 个采样，合 38.7 毫秒，L17 里量过；这是同一个量级的数，我没有把每一项拆开验证）。`truncated.ts` 的音视频时长差 0.182 秒不是转码造成的：输入文件被截断时，音频比视频多出了 0.18 秒，输出把它们原样保留。

### 硬件加速

`--hw vaapi` 只做**编码**（和 L21 一样，解码留在 CPU 上；解码到编码之间的数据在内存里，经 `format=nv12,hwupload` 上传）：编码器换成 `h264_vaapi`，`--crf` 变成 `qp` 选项（VAAPI 编码器没有 crf），`--vcodec h264_vaapi` 必须同时给 `--hw vaapi`。`--hw auto` 有设备就用，没有设备退回软件并警告；`--hw vaapi` 没有设备就报错退出，不留下输出文件。`--hw-device /dev/dri/renderD129` 指定设备。

```
$ mtool transcode -y --hw vaapi -d 1 samples/mtool/h264_aac.mp4 out/h1.mp4
mtool：错误：打不开 VAAPI 设备（没有显卡、没有 /dev/dri，或者驱动没装）：Generic error in an external library
$ mtool transcode -y --hw auto -d 1 samples/mtool/h264_aac.mp4 out/h2.mp4
mtool：警告：没有可用的 VAAPI 设备（Generic error in an external library），改用软件编码
转码完成：samples/mtool/h264_aac.mp4 → out/h2.mp4（mp4）
  视频：h264 640x360 → libx264 640x360 yuv420p，25 帧，25.000 帧/秒，约 719 kb/s
```

**【待补 1】** 开发这个项目的机器没有显卡（`/dev/dri` 不存在），上面两条是它能验证的全部：没有设备时的报错和退回软件，已经在自动化测试里（`--hw vaapi`、`--hw auto`、`--hw-device` 指向不存在的设备、`--vcodec h264_vaapi` 没有 `--hw`）。有设备时的路径写好了，但**没有在真实的设备上跑过**。有 VAAPI 设备的人运行：

```bash
mtool/scripts/verify_hw.sh
```

它做 7 项检查（用的是 h264_vaapi、时长误差 <0.1 秒、帧数不变、能完整解码；`--crf` 越大文件越小；`-w 320` 缩放；`-s 1.5 -d 2` 裁剪；旋转的输入；`--hw auto` 用 VAAPI；帧数和 `ffmpeg -vaapi_device … -c:v h264_vaapi` 相同），打印一张结果表，把表填到这里。没有设备时它退出码 77（跳过）。

### 出错

```
$ mtool transcode -y --vcodec bogus samples/mtool/h264_aac.mp4 out/x.mp4
mtool：错误：没有叫 bogus 的视频编码器（ffmpeg -encoders 能看到这份 FFmpeg 有哪些）
$ mtool transcode -y --ar 7000 samples/mtool/h264_aac.mp4 out/x.mp4
mtool：错误：编码器 aac 不支持采样率 7000 Hz，它支持：96000、88200、64000、48000、44100、32000、24000、22050、16000、12000、11025、8000、7350
$ mtool transcode -y --vopt bogus=1 samples/mtool/h264_aac.mp4 out/x.mp4
mtool：错误：视频编码器 libx264 不认识选项 bogus（--preset、--crf、--vopt 里的名字）
$ mtool transcode -y --faststart samples/mtool/h264_aac.mp4 out/x.mkv
mtool：错误：复用器 matroska 不认识选项 movflags（--faststart 只对 MP4、MOV 有用）
$ mtool transcode -y -s 99 samples/mtool/h264_aac.mp4 out/x.mp4
mtool：错误：起点 99.000 秒超过了文件的长度 5.000 秒
$ mtool transcode -y --crf abc samples/mtool/h264_aac.mp4 out/x.mp4
mtool：用法错误：--crf abc 不是数字
```

前五个退出码 1，最后一个 2；这些失败以后都不留下输出文件（出错时 `unlink` 掉这次创建的文件）。输入输出是同一个文件、输出已存在不加 `-y`、输出目录不存在、扩展名认不出来、不认识的滤镜（`--vf bogusfilter`）也是 1。损坏但能打开的文件（`truncated.ts`、`corrupt_mid.mp4`）退出码 0：能解的帧照常编码，库会打印解码的错误（`error while decoding MB`），输出本身完整、能解码。

## mtool push

```bash
mtool push [-f 格式] [--loop N] [-d 时长] [--no-pace] [-o 键=值]... <输入> <地址>
```

把一个文件**不转码**地、按播放的速度推到流媒体地址。来自 L19（协议层）和 L15（复用）：读包，换时间戳，按 `dts` 对着墙上的时钟等，写给输出。地址的协议决定默认的封装格式：`rtmp://` 用 FLV，`udp://`、`tcp://`、`srt://` 用 MPEG-TS，`rtsp://` 用 RTSP，别的按地址的扩展名猜，`-f` 可以指定。

```
$ mtool push -d 2 samples/mtool/h264_aac.mp4 rtmp://127.0.0.1:19351/live/s3cr3tkey
推流  99.8%  已处理 2.0 秒，速度 1.00x
推流完成：samples/mtool/h264_aac.mp4 → rtmp://127.0.0.1:19351/live/***（flv）
  video：52 个包，202815 字节
  audio：88 个包，27921 字节
  共 140 个包、230736 字节，推了 1.997 秒的内容，用时 1.997 秒，循环 0 遍
$ mtool push --no-pace --loop 2 samples/mtool/h264_aac.mp4 tcp://127.0.0.1:19370
推流完成：samples/mtool/h264_aac.mp4 → tcp://127.0.0.1:19370（mpegts）
  video：375 个包，1444815 字节
  audio：651 个包，228642 字节
  共 1026 个包、1673457 字节，推了 15.070 秒的内容，用时 0.003 秒（没有按播放速度等待），循环 2 遍
```

做法和要注意的：

1. **按 `dts` 对着时钟等**：连接成功、文件头写完以后开始计时，每个包等到“它的 dts 减去文件起始时间”到了再发（`av_gettime_relative` 加 `av_usleep`，切成 50 毫秒一段，好让 Ctrl-C 马上生效）。和 `ffmpeg -re` 一样；`--no-pace` 取消等待。
2. **连接之前先检查**：地址必须是这份 FFmpeg 能**写**的网络协议（`avio_enum_protocols`）；这份没有 libsrt，`srt://` 会说“这份 FFmpeg 没有 srt 协议（编译时要加 --enable-libsrt）”，而不是一句 `Protocol not found`；普通文件路径和 `file://` 拒绝（写文件请用 `cut`、`transcode`）。输入的编码装不装得下输出格式，用 `avformat_init_output` 在连接之前查：`avformat_query_codec` 对 FLV 装 MPEG-4 视频说“可以”，只有 `avformat_init_output` 才会拒绝（FLV 的原话是 `Codec mpeg4 is not supported in the official FLV specification`）。
3. **时间戳**：整体减去文件的起始时间（TS 的时间不从 0 开始），再交给复用器。FLV 开头的元数据有时长和文件大小，往网络上推时回不去改，会打印两条警告，所以对 FLV 设 `flvflags=no_duration_filesize`，直播本来就没有时长。
4. **循环**：`--loop N` 播完以后**重新打开输入**再播，时间戳往后挪“最长的一路流的长度”（结束时间减这一路第一个包的 dts）。两个坑：直接用结束时间的话，MP4 的音频第一个包的 dts 是负的，下一遍的第一个音频包会和这一遍的最后一个重合（复用器报 `non monotonically increasing dts`）；用 `seek` 回到开头的话，TS 这类没有索引的容器不一定落在第一个包上，落在 P 帧上接收端从第二遍起会花屏（测试里 TS 的第三遍少了 2 个视频包，还有解码错误）。`-d` 把循环也算在内：`--loop -1 -d 7` 是推 7 秒。
5. **信号**：Ctrl-C（SIGINT）和 SIGTERM 只设一个标志，主循环看到以后写完文件尾再退出，**退出码 0**（直播推流本来就是靠 Ctrl-C 停的）；连接和握手这类阻塞的操作用 `AVIOInterruptCB` 打断（这一条没有单独测，测的是推流途中收到信号）。SIGPIPE 被忽略：对方断开时写操作返回错误，而不是进程被信号杀掉，退出码 1，说明原因：

   ```
   mtool：错误：往 tcp://127.0.0.1:19362 发数据失败（对方断开了？）：Broken pipe（对方关闭了连接）
   ```
6. **地址里的密钥不出现在输出里**：用户名密码、查询串（常常带 token）、RTMP 的最后一段（流名就是推流密钥）在 stdout 和错误信息里都被去掉：`rtmp://u:p4ss@主机/live/s3cr3tkey?token=t0k3n` 显示成 `rtmp://主机/live/***`。库自己的日志（`[tcp @ …] Connection to … failed`）只带主机和端口。
7. **`-o 键=值`** 把选项交给协议或者封装器（`-o rtsp_transport=tcp`、`-o pes_payload_size=2930`）：封装器认识的在 `avformat_init_output` 里取走，协议认识的在 `avio_open2` 里取走，剩下谁也不认识的报错并说出选项名。

### 测试：本机的 ffmpeg 当接收端

没有外部服务器也能测：RTMP 用 `ffmpeg -listen 1 -i rtmp://127.0.0.1:端口/live/流名 -c copy -f flv 收到的.flv`，TCP 用 `ffmpeg -i "tcp://127.0.0.1:端口?listen" -c copy -f mpegts 收到的.ts`。测试脚本先等端口进入监听状态（读 `/proc/net/tcp`，不能“连一下试试”：只接受一个连接的接收端会把试探当成推流的客户端），再推。判断标准：

- 13 个测试文件（好的和损坏的）推到 TCP（`--no-pace`）：收到的视频包、音频包的 CRC 清单，和 `ffmpeg -c copy -f mpegts` 直接转出来的逐包相同，收到的时长相差不到 0.15 秒。
- RTMP 按播放速度推 2 秒：mtool 用了 1.997 秒的内容、总共用时 2.0 到 2.4 秒；收到的包数等于发出的（视频 52、音频 88），包内容和输入开头逐包相同（FLV 里 H.264 还是 AVCC，包内容逐字节没变）；stdout、stderr 里没有流名。输入是 TS 时（AAC 是 ADTS，FLV 要 AudioSpecificConfig）封装器自动转，收到的能完整解码。
- 循环 2 遍：视频包、音频包的数量是输入的 3 倍（375、651），时长是 3 倍，收到的能完整解码；`h264_aac.mp4`、`.mkv`、`.ts` 和可变帧率的 `vfr.mp4` 都测。
- Ctrl-C 和 SIGTERM：收到信号后 0.5 秒内退出，退出码 0，收到的文件能完整解码。对方被杀掉：退出码 1，5 秒内退出。
- 失败：连不上（端口没人监听）、`srt://`、`rtmps://`、文件路径、不认识的协议、编码装不下（MP2 装不进 FLV，MPEG-4 装不进 FLV，两种都在连接之前报错）、打不开的输入、用法错误。

**接收端是 ffmpeg 时，RTMP 不要加 `--no-pace`**：发得太快、发完立刻关闭连接，接收端的 ffmpeg 回确认消息时碰到已关闭的连接（`Broken pipe`），把缓冲区里还没读的包丢掉（本机测的 3 遍循环只收到 229 个视频包，应该是 375 个）。这是接收端的行为，真实的 RTMP 服务器不会这样，也不是发送端丢了包；按播放速度推没有这个问题（上面 3 次、每次 40 个视频包 66 个音频包，收到的和发出的一样多）。SRT 在这份 FFmpeg 里没法测（没有 libsrt）。

## 验收标准对照

课程规划里对综合项目的验收标准，和这里怎么验证的：

| 验收标准 | 怎么验证 | 结果 |
| --- | --- | --- |
| 至少 10 个不同格式的测试文件，含旋转、可变帧率、无音频、损坏 | `scripts/gen_testfiles.sh` 合成 18 个 | 好的 12（MP4、MKV、TS、FLV、AVI、M4A、WAV，含 `rotate90`、`vfr`、`video_only`、`audio_only`）；打不开的 4；能打开的损坏 2 |
| 每个文件不崩溃，错误提示能看懂 | 每个子命令对每一类文件都跑，检查退出码（0、1、2）、stderr 有“mtool：错误：”、打不开的不留下输出 | 全部通过 |
| `transcode` 输出时长误差 < 0.1 秒（ffprobe） | 14 个文件转 MP4，读输入输出的 `format=duration`；程序自己转完也重新打开输出读一遍 | 最大 0.055 秒（`h264_aac.flv`），见上面的表 |
| 没有明显的音视频不同步 | 输出里音视频两路的开头差和时长差 | 8 个有音视频两路的好文件最大 0.040 秒、0.039 秒 |
| ASan 无泄漏、无越界 | `make SANITIZE=address,undefined`，每次运行都检查 sanitizer 报告，同样的全部测试再跑一遍 | 通过 |
| 有 README、自动化测试、CI 配置 | 本文件；`tests/run_tests.sh`；`.github/workflows/mtool.yml` | 都有；CI 在 GitHub Actions 上第一次运行就通过了，见“持续集成” |
| L22 的约定：退出码只有 0/1/2、错误写 stderr、版本检查、ASan 构建 | `mtool --version`（头文件和运行时库的版本检查）；测试里逐项检查 | 通过 |
| VAAPI 硬件编码 | `scripts/verify_hw.sh` | **没有验证**（没有显卡），见【待补 1】 |

## 持续集成

`.github/workflows/mtool.yml`：改了 `mtool/` 或者 `scripts/build_ffmpeg.sh` 就跑。步骤：装依赖（和 `build_ffmpeg.sh` 里的 `APT_PACKAGES` 一致）→ 取 FFmpeg 的缓存（键是 `scripts/build_ffmpeg.sh` 的内容，里面的 FFmpeg 版本号或者编译选项改了，缓存就换新的）→ 缓存没有命中时编译 FFmpeg（十几分钟）→ 运行 `mtool/scripts/check.sh`：

```bash
mtool/scripts/check.sh           # 普通构建加 ASan+UBSan 构建，都用 -Werror 编译，生成测试文件，各跑一遍全部测试
mtool/scripts/check.sh plain     # 只有普通构建
mtool/scripts/check.sh asan      # 只有 ASan 构建
```

CI 跑的就是 `check.sh`，所以本机改完代码跑它，结果和 CI 一致。

**GitHub Actions 上的第一次运行**（PR #33，`ubuntu-24.04`，没有改过工作流）通过了：装依赖约 23 秒，缓存没有命中所以编译 FFmpeg 用了约 5 分钟，`check.sh` 约 2 分 40 秒；普通构建和 ASan 加 UBSan 构建的 `-Werror` 编译都过，两份构建各 **1347 项全部通过**，最后一行是“mtool 全部检查通过”。1347 比本机的 1360 少 13 项，原因见上面“自动化测试”：那 13 项只在没有 `/dev/dri` 时才跑，少的数正好是 13，所以运行器上应该有 `/dev/dri`（当时没有直接去查，加了说明那一行以后的运行会在日志里打印出来）。工作流最后把 `ffmpeg_build` 存进了缓存；**下一次运行能不能命中缓存、`--enable-rpath` 编译的库从缓存还原后能不能用，还没有验证**（要看之后的运行）。CI 里没有跑 `verify_hw.sh`，所以 VAAPI 编码仍然没有在真实设备上验证过（见【待补 1】）。

## 练习

1. 给 `info` 加一个 `--frames` 选项，把每一帧的类型、pts、大小打印出来（参考 L08 的 `frame_info`）。想一想：JSON 输出里怎么表示？（数据量会很大，要不要一行一个？）
2. `cut` 的终点是按 dts 判断的，有 B 帧时会多出几帧。改成“按 pts 判断，多余的 B 帧丢掉”会遇到什么问题？（提示：丢掉一个 P 帧却留下依赖它的 B 帧。）
3. `info` 判断可变帧率的容差（2 个刻度、5%）是怎么来的？自己造一个 29.97 帧/秒的文件，看看在 1/1000 时间基的 MKV 里判断得对不对。
4. 把 `mtool info` 的统计改成不用全局数组、不把所有 pts 存下来（边读边算最小最大间隔），可以吗？（提示：包的 pts 不是单调的，有 B 帧时会先出后面的帧。）
5. `transcode` 对可变帧率的输入保持每一帧原来的时间，命令行会取整到 1/帧率 的格子上。造一个时间戳不在格子上的文件（例如每帧间隔 33 毫秒到 41 毫秒随机），两种做法输出的 pts 差多少？取整以后会不会有两帧落在同一个格子里？（提示：命令行的 `-vsync vfr` 会丢掉其中一帧。）
6. `push` 对方断开就退出。加一个 `--retry N`：断开以后重新连接，接着推。要想清楚三件事：时间戳要接着往后（不能从 0 重来，播放器会认为是新的流）；FLV 要重新写文件头和 H.264 的序列头；重连以后第一个视频包必须是关键帧，不然接收端花屏到下一个关键帧。
7. `--hw vaapi` 现在只加速编码。把解码也放到 VAAPI 上（L20 的 `hw_device_ctx` 和 `get_format`），解码出来的帧留在显存里，缩放换成 `scale_vaapi`，这样 `hwupload` 就不需要了。`--vf` 里用户写的软件滤镜怎么办？
