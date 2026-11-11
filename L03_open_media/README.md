# L03 打开媒体文件

从这一讲开始写代码。任何处理音视频的程序，第一件事都是把输入打开，弄清楚里面有几路流、
各是什么编码、参数是多少。本讲从官方示例里最短的 `show_metadata.c` 出发，
学会 libavformat 打开输入的固定写法，并认识后面每一讲都要用到的四个结构体。

完成标志：`media_info` 打印出的每个字段，都能在 `ffprobe -show_format -show_streams` 的输出里找到相同的值。

## 本讲要点

**容器里有什么**

- 容器文件由三部分组成：描述信息（有几路流、各自的编码参数、时长、索引）、
  按时间交错存放的各路压缩数据、元数据（标题、语言等给人看的标签）。
- 不同容器组织这三部分的方式不同：MP4 是一层套一层的 box，描述信息集中在 `moov` 里；
  FLV 是一个接一个的 tag，没有集中的索引；MPEG-TS 是固定 188 字节的小包，没有文件头，从任意位置都能开始读。
- 这些差别被 libavformat 隐藏了。不管什么容器，打开之后拿到的都是同一套结构体。

**四个结构体**

| 结构体 | 代表什么 | 本讲用到的成员 |
| --- | --- | --- |
| `AVFormatContext` | 一个打开的输入（或输出） | `iformat` `nb_streams` `streams` `duration` `bit_rate` `metadata` `programs` |
| `AVStream` | 容器里的一路流 | `index` `id` `time_base` `duration` `nb_frames` `disposition` `metadata` `codecpar` |
| `AVCodecParameters` | 这路流的编码参数 | `codec_type` `codec_id` `format` `width` `height` `sample_rate` `ch_layout` `bit_rate` |
| `AVDictionary` | 一组字符串键值对 | 既用来存元数据，也用来传选项 |

**打开输入的固定写法**

1. `avformat_open_input()`：打开文件，识别容器格式，读文件头。
2. `avformat_find_stream_info()`：再读一段数据，把文件头里没有的流参数补齐。
3. 使用 `AVFormatContext` 里的信息。
4. `avformat_close_input()`：关闭并释放。

**两条贯穿全课的约定**

- FFmpeg 的函数失败时返回负的错误码，用 `av_err2str()` 转成文字。每个返回值都要检查。
- 时间是“整数 × 时间基”。`AVFormatContext` 里的时间以微秒（`AV_TIME_BASE`）为单位，
  每路流有自己的 `time_base`。“不知道”用 `AV_NOPTS_VALUE` 表示，不能当数字用。时间基在 L04 展开讲。

## 准备

```bash
scripts/gen_samples.sh                 # 本讲新增了 TS 和 FLV 两个素材
md5sum samples/small_360p25.mp4        # a3d555ff38506373c97a0fe5d56fdc4d
make -C L03_open_media                 # 可执行文件在 L03_open_media/build/ 下
B=L03_open_media/build
```

校验和不一致的话，运行 `scripts/gen_samples.sh --force` 重新生成全部素材。
下面的命令都在仓库根目录执行。

## 第 1 步：看看容器长什么样

用 `od` 直接看文件开头的字节：

```console
$ od -A d -t x1z -N 48 samples/small_360p25.mp4
0000000 00 00 00 20 66 74 79 70 69 73 6f 6d 00 00 02 00  >... ftypisom....<
0000016 69 73 6f 6d 69 73 6f 32 61 76 63 31 6d 70 34 31  >isomiso2avc1mp41<
0000032 00 00 00 08 66 72 65 65 00 08 83 03 6d 64 61 74  >....free....mdat<
```

MP4 的每个 box 以 4 字节长度加 4 字节类型开头。这里能看到三个：`ftyp`（32 字节）、`free`（8 字节）、
`mdat`（0x00088303 = 557,827 字节）。ffprobe 在 trace 级别的日志里会列出它读到的每个 box：

```console
$ ffprobe -v trace samples/small_360p25.mp4 2>&1 | grep "parent:'root'"
... type:'ftyp' parent:'root' sz: 32 8 563196
... type:'free' parent:'root' sz: 8 40 563196
... type:'mdat' parent:'root' sz: 557827 48 563196
... type:'moov' parent:'root' sz: 5329 557875 563196
```

四个顶层 box 的大小加起来正好是文件大小 563,196。`mdat` 里是全部的压缩数据，
`moov` 只有 5,329 字节，却记录着每一路流的参数和每一个包的位置、大小、时间。它排在文件末尾。

其他三种容器的开头：

```console
$ od -A d -t x1z -N 32 samples/small_360p25.flv
0000000 46 4c 56 01 05 00 00 00 09 00 00 00 00 12 00 01  >FLV.............<
0000016 25 00 00 00 00 00 00 00 02 00 0a 6f 6e 4d 65 74  >%..........onMet<
$ od -A d -t x1z -N 32 samples/small_360p25.mkv
0000000 1a 45 df a3 a3 42 86 81 01 42 f7 81 01 42 f2 81  >.E...B...B...B..<
0000016 04 42 f3 81 08 42 82 88 6d 61 74 72 6f 73 6b 61  >.B...B..matroska<
$ for off in 0 188 376; do od -A d -t x1z -j $off -N 4 samples/small_360p25.ts | head -1; done
0000000 47 40 11 10                                      >G@..<
0000188 47 40 00 10                                      >G@..<
0000376 47 50 00 10                                      >GP..<
```

| 容器 | 怎么认出来 | 基本单位 | 描述信息在哪 |
| --- | --- | --- | --- |
| MP4 | 第 5 到 8 字节是 `ftyp` | box，可以嵌套 | 集中在 `moov` |
| MKV | 以 `1a 45 df a3` 开头，后面有 `matroska` | EBML 元素，可以嵌套 | 文件开头的 Tracks 等元素 |
| FLV | 以 `FLV` 开头 | tag，一个接一个 | 第一个 tag（`onMetaData`）里有一部分，其余要读数据才知道 |
| MPEG-TS | 每 188 字节出现一次 `0x47` | 188 字节的定长包 | 没有文件头，靠周期性重复的 PAT、PMT 表 |

TS 文件的大小 609,684 正好是 188 的 3,243 倍。它是为广播设计的，接收端随时可能从中间开始收，
所以不能依赖文件头。这个特点在第 4 步会看到后果。

## 第 2 步：读官方示例 show_metadata.c

[show_metadata.c](show_metadata.c) 是官方示例的原样拷贝，加了中文注释。去掉参数检查，它只有四个调用：

```c
AVFormatContext *fmt_ctx = NULL;
avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
avformat_find_stream_info(fmt_ctx, NULL);
while ((tag = av_dict_iterate(fmt_ctx->metadata, tag)))
    printf("%s=%s\n", tag->key, tag->value);
avformat_close_input(&fmt_ctx);
```

```console
$ $B/show_metadata samples/small_360p25.mp4
major_brand=isom
minor_version=512
compatible_brands=isomiso2avc1mp41
encoder=Lavf63.18.100
```

用 ffmpeg 给文件加上元数据再看。`-metadata` 加在容器上，`-metadata:s:a:0` 加在第一路音频流上：

```bash
mkdir -p out
for ext in mp4 mkv; do
    ffmpeg -v error -y -i samples/small_360p25.mp4 -c copy \
        -metadata title="FFmpeg 课程 L03" -metadata artist="tutorial" -metadata course=L03 \
        -metadata:s:a:0 language=chi out/tagged.$ext
done
```

```console
$ $B/show_metadata out/tagged.mp4
major_brand=isom
minor_version=512
compatible_brands=isomiso2avc1mp41
title=FFmpeg 课程 L03
artist=tutorial
encoder=Lavf63.18.100
$ $B/show_metadata out/tagged.mkv
title=FFmpeg 课程 L03
ARTIST=tutorial
COURSE=L03
ENCODER=Lavf63.18.100
```

两个现象：

- 自己起的键 `course` 在 MP4 里丢了，在 MKV 里还在。MP4 只认一批固定的键，MKV 可以存任意键。
  元数据能不能存进去由容器决定。
- 音频流上的 `language=chi` 两个文件都没有打印。示例只遍历了 `fmt_ctx->metadata`，
  流的元数据在 `fmt_ctx->streams[i]->metadata` 里（练习 1）。

官方示例在错误处理上有两处不足：打不开文件时不打印任何信息，`avformat_find_stream_info()`
失败时没有关闭输入。`media_info.c` 里是更合适的写法。这个问题已记入
[upstream_patches](../upstream_patches/README.md) 的 0009 号 patch。

## 第 3 步：media_info，打印每一路流的参数

[media_info.c](media_info.c) 把三层结构体里常用的成员都打印出来。
每一行左边的名字就是成员名：

```console
$ $B/media_info samples/small_360p25.mp4
== 容器 ==
  url                  samples/small_360p25.mp4
  iformat->name        mov,mp4,m4a,3gp,3g2,mj2
  iformat->long_name   QuickTime / MOV
  nb_streams           2
  nb_programs          0
  nb_chapters          0
  start_time           0  (0.000 秒)
  duration             5000000  (5.000 秒)
  bit_rate             901113
  avio_size(pb)        563196
  metadata
      major_brand = isom
      ...

== 流 #0：video ==
  id                   0x1
  codec_id             h264
  profile              High
  codec_tag            avc1
  bit_rate             770568
  extradata_size       47
  width                640
  height               360
  format               yuv420p
  level                30
  sample_aspect_ratio  1/1
  avg_frame_rate       25/1
  r_frame_rate         25/1
  time_base            1/12800
  start_time           0  (0.000 秒)
  duration             64000  (5.000 秒)
  nb_frames            125
  disposition          default
  ...

== 流 #1：audio ==
  id                   0x2
  codec_id             aac
  profile              LC
  codec_tag            mp4a
  bit_rate             121005
  extradata_size       5
  sample_rate          44100
  format               fltp
  ch_layout            stereo
  nb_channels          2
  frame_size           1024
  time_base            1/44100
  start_time           0  (0.000 秒)
  duration             220500  (5.000 秒)
  nb_frames            217
  disposition          default
  ...

== av_find_best_stream ==
  video                0
  audio                1

== av_dump_format ==
Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'samples/small_360p25.mp4':
  ...
```

和 ffprobe 核对。两边的名字大多相同，不同的几个对应关系如下：

```bash
ffprobe -v error -show_format -show_streams samples/small_360p25.mp4
```

| media_info | ffprobe | 说明 |
| --- | --- | --- |
| `iformat->name` | `format_name` | |
| `avio_size(pb)` | `size` | 文件大小来自 I/O 层，不在 `AVFormatContext` 里 |
| 容器的 `duration`（微秒） | `[FORMAT]` 里的 `duration`（秒） | |
| `codec_id` | `codec_name` | 代码里是枚举值，`avcodec_get_name()` 转成名字 |
| `codec_tag` | `codec_tag_string` | 容器里标识编码的四个字符 |
| `format` | `pix_fmt` 或 `sample_fmt` | 同一个成员，视频是像素格式，音频是采样格式 |
| 流的 `start_time`、`duration` | `start_pts`、`duration_ts` | 以流的 `time_base` 为单位的整数 |

代码里要注意的几处：

- **三层结构**。`fmt_ctx->streams[i]` 是 `AVStream`，与编码有关的参数再往下一层，在 `st->codecpar` 里。
  后面打开解码器时，就是把 `codecpar` 整个交给解码器。
- **两种时间单位**。64000 × 1/12800 = 5 秒，220500 × 1/44100 = 5 秒。同一个 5 秒，在两路流里是不同的整数。
- **`av_find_best_stream()`**。文件里可能有多路音频、多路视频，这个函数挑出一路。后面每一讲都用它。
- **`av_dump_format()`**。`ffmpeg -i` 开头那段 `Input #0, ...` 就是它打印的。调试时一行代码就能看到全部信息。

同样的内容换一个容器，拿到的信息并不一样：

```bash
for f in mp4 mkv ts flv; do $B/media_info samples/small_360p25.$f 2>/dev/null; done
```

| | MP4 | MKV | MPEG-TS | FLV |
| --- | --- | --- | --- | --- |
| 视频流 `time_base` | 1/12800 | 1/1000 | 1/90000 | 1/1000 |
| 视频流 `duration` | 64000（5.000 秒） | N/A | 450000（5.000 秒） | N/A |
| 视频流 `nb_frames` | 125 | 0 | 0 | 0 |
| 视频流 `bit_rate` | 770568 | 0 | 0 | 770568 |
| 容器 `start_time` | 0.000 秒 | 0.000 秒 | 1.457 秒 | 0.057 秒 |
| `nb_programs` | 0 | 0 | 1 | 0 |
| 流的 `id` | 0x1、0x2 | 0x0 | 0x100、0x101 | 0x0 |

只有 MP4 知道每路流有多少帧，因为它的索引逐个记录了每个包。MKV 和 TS 不记录每路流的码率。
TS 的时间不从 0 开始，并且多出一个节目（program）：TS 可以同时携带多个电视频道，每个频道是一个节目，
节目里再列出属于它的流。写程序时不能假设这些成员一定有值。

## 第 4 步：probe_stages，两个函数各做了什么

[probe_stages.c](probe_stages.c) 在 `avformat_open_input()` 和 `avformat_find_stream_info()`
之后各打印一次流参数，同时打印到那一刻为止读了多少字节。

```console
$ $B/probe_stages samples/small_360p25.mp4
识别出的容器格式：mov,mp4,m4a,3gp,3g2,mj2
-- avformat_open_input 之后 --
  已读取 38097 字节，nb_streams = 2，duration = N/A
  #0 video h264       640x360  format=未知  avg_frame_rate=25/1
  #1 audio aac        44100 Hz  format=未知  ch_layout=2 channels
-- avformat_find_stream_info 之后 --
  已读取 70865 字节，nb_streams = 2，duration = 5.000 秒
  #0 video h264       640x360  format=yuv420p  avg_frame_rate=25/1
  #1 audio aac        44100 Hz  format=fltp  ch_layout=stereo
```

六个素材的结果汇总：

| 素材 | open 之后 | find_stream_info 之后 | 一共读了 |
| --- | --- | --- | --- |
| small_360p25.mp4 | 2 路流，分辨率和采样率已知，像素格式和采样格式未知 | 全部已知 | 70,865 字节 |
| small_360p25.mkv | 同上，时长也已知 | 全部已知 | 32,768 字节 |
| small_360p25.flv | **0 路流** | 2 路流，全部已知 | 196,608 字节 |
| small_360p25.ts | 2 路流，只知道编码，分辨率是 0x0，采样率是 0 | 全部已知 | 381,072 字节 |
| video_360p25.m1v | 1 路流，分辨率 0x0 | 全部已知，时长 0.059 秒 | 425,984 字节 |
| audio_44k_stereo.mp2 | 1 路流，编码被认成 mp3 | 改正为 mp2，全部已知 | 65,664 字节 |

从这张表能读出三件事：

- **`open` 只读文件头**。文件头里有什么取决于容器：MP4 和 MKV 写得很全，TS 只知道有哪几路流，
  FLV 连有几路流都要读到数据才知道。
- **像素格式、采样格式这类参数不在容器里**，而在压缩数据内部。`find_stream_info` 会读一段数据，
  必要时还会试着解码几帧，把它们找出来。TS 文件为此读了 38 万字节，超过文件的一半。
  输入是直播流时，这一步是起播延迟的主要来源之一。
- **没有容器的裸流，时长只能估**。`.m1v` 实际是 5 秒，FFmpeg 根据码率估成了 0.059 秒，并且打印了
  `Estimating duration from bitrate, this may be inaccurate`。看到这条提示时，时长不可信。

习惯上两个函数总是一起调用，只有清楚输入是什么、并且在意启动速度时才省略或限制第二步。

`find_stream_info` 最多读多少，由两个选项限制：`probesize`（最多读多少字节）和
`analyzeduration`（最多分析多长时间的数据，单位是微秒）。把它们压得很小，看看会怎样
（`open_with_options` 在第 5 步介绍，这里先用一下）：

```console
$ $B/open_with_options samples/small_360p25.ts - probesize=2000 analyzeduration=0
[mpegts @ 0x...] Stream #0: not enough frames to estimate rate; consider increasing probesize
...
[mpegts @ 0x...] Could not find codec parameters for stream 1 (Audio: aac ([15][0][0][0] / 0x000F), 0 channels): unspecified sample format
Consider increasing the value for the 'analyzeduration' (0) and 'probesize' (2000) options
Input #0, mpegts, from 'samples/small_360p25.ts':
  ...
  Stream #0:0[0x100]: Video: h264 (High) ([27][0][0][0] / 0x001B), yuv420p(progressive), 640x360 [SAR 1:1 DAR 16:9], 50 tbr, 90k tbn, start 1.480000
  Stream #0:1[0x101](und): Audio: aac ([15][0][0][0] / 0x000F), 0 channels, start 1.480000
```

程序没有报错退出，但音频流的采样率、声道数、采样格式都没有读出来，视频的帧率也没有了。
`Could not find codec parameters` 是实际工作中很常见的一条日志，原因就是探测到的数据不够。

## 第 5 步：open_with_options，选项字典与没有文件头的数据

L01 导出的 `.yuv` 和 `.pcm` 没有任何文件头，FFmpeg 无法知道分辨率和采样率。
`avformat_open_input()` 的第三、第四个参数就是为这种情况准备的：第三个强制指定格式，
第四个是一个 `AVDictionary`，里面的选项交给解封装器。

先准备数据（L01 做过可以跳过）：

```bash
ffmpeg -v error -y -i samples/demo_1080p30.mp4 -t 1 -an -f rawvideo -pix_fmt yuv420p out/demo.yuv
ffmpeg -v error -y -i samples/demo_1080p30.mp4 -t 1 -vn -f s16le out/demo.pcm
```

```console
$ $B/open_with_options out/demo.yuv rawvideo
[rawvideo @ 0x...] [IMGUTILS @ 0x...] Picture size 0x0 is invalid
打不开 out/demo.yuv：Invalid argument

$ $B/open_with_options out/demo.yuv rawvideo video_size=1920x1080 pixel_format=yuv420p framerate=30
Input #0, rawvideo, from 'out/demo.yuv':
  Duration: 00:00:01.00, start: 0.000000, bitrate: 746496 kb/s
  Stream #0:0: Video: rawvideo (I420 / 0x30323449), yuv420p, 1920x1080, 746496 kb/s, 30 tbr, 30 tbn
```

746,496 kb/s 就是 L01 算过的未压缩 1080p30 的码率。

把 `pixel_format` 错写成 `pix_fmt` 再试：

```console
$ $B/open_with_options out/demo.yuv rawvideo video_size=1920x1080 pix_fmt=yuv420p framerate=30
警告：选项 pix_fmt=yuv420p 没有被识别，已被忽略
Input #0, rawvideo, from 'out/demo.yuv':
  ...
```

这条警告是 [open_with_options.c](open_with_options.c) 自己打印的，不是 FFmpeg 打印的。
`avformat_open_input()` 遇到不认识的选项不会报错，只是把它留在字典里。
调用之后检查字典里还剩什么，是工程代码里的固定动作，否则选项拼错了也不会有任何提示：

```c
ret = avformat_open_input(&fmt_ctx, argv[1], iformat, &options);
...
while ((e = av_dict_iterate(options, e)))
    fprintf(stderr, "警告：选项 %s=%s 没有被识别，已被忽略\n", e->key, e->value);
av_dict_free(&options);
```

某个格式支持哪些选项，用 `ffmpeg -hide_banner -h demuxer=rawvideo` 查看。

## 常见错误

**`AVFormatContext *fmt_ctx;` 没有初始化为 NULL**
`avformat_open_input()` 看到非 NULL 的指针会把它当成已经分配好的上下文来用。
未初始化的指针指向哪里是不确定的，结果通常是崩溃。

**把 `AV_NOPTS_VALUE` 当成时长**
`duration` 未知时的值是 `AV_NOPTS_VALUE`，一个很大的负数。直接换算会得到负九万多亿秒。
使用 `start_time`、`duration` 之前先判断。

**不看类型就用 `codecpar->format`**
它对视频是 `enum AVPixelFormat`，对音频是 `enum AVSampleFormat`，两套枚举的数值互不相干。

**成功打开之后的出错路径上忘了关闭**
`avformat_open_input()` 自己失败时会把指针置回 NULL，不需要再关。但只要它成功过，
之后任何一步失败都要经过 `avformat_close_input()`。`media_info.c` 用 `goto end` 保证这一点。

**以为 ffprobe 报的时长、帧数一定和数据相符**
这些数来自文件头或索引，不是数出来的（练习 3）。

## 练习

1. 修改 `show_metadata.c`，在容器元数据之后把每一路流的元数据也打印出来。
   用 `out/tagged.mp4` 验证能看到 `language=chi`。
2. 两条音轨的文件里，`av_find_best_stream()` 选哪一条？按下面的命令生成文件并用 `media_info` 查看。
   再用 `-disposition` 把默认轨换成另一条，结果怎么变？
   ```bash
   ffmpeg -v error -y -i samples/small_360p25.mp4 -f lavfi -i "sine=frequency=500:sample_rate=48000" \
       -t 5 -map 0:v -map 0:a -map 1:a -c:v copy -c:a:0 copy -c:a:1 aac -ac:a:1 6 \
       -metadata:s:a:0 language=chi -metadata:s:a:1 language=eng out/two_audio.mkv
   ffmpeg -v error -y -i out/two_audio.mkv -map 0 -c copy \
       -disposition:a:0 0 -disposition:a:1 default out/two_audio_b.mkv
   ```
3. 截断实验。分别取 MP4 和 MKV 的前 100,000 字节，用 `media_info` 打开，解释结果为什么不同。
   然后把 MP4 的 `moov` 挪到文件开头，再做一次。
   ```bash
   head -c 100000 samples/small_360p25.mp4 > out/truncated.mp4
   head -c 100000 samples/small_360p25.mkv > out/truncated.mkv
   ffmpeg -v error -y -i samples/small_360p25.mp4 -c copy -movflags +faststart out/faststart.mp4
   head -c 100000 out/faststart.mp4 > out/truncated_fs.mp4
   ```
4. 用 `open_with_options` 打开 `out/demo.pcm`，一次给出正确的参数
   （`s16le`，`sample_rate=48000 ch_layout=stereo`），一次什么参数都不给。两次报告的时长分别是多少？为什么？
5. 依次打开一个不存在的文件、一个文本文件、练习 3 的 `out/truncated.mp4`，
   记下 `media_info` 打印的错误文字和错误码。三种情况里哪些错误码相同？

<details>
<summary>参考答案</summary>

1. 在遍历 `fmt_ctx->metadata` 的循环后面加上：

   ```c
   for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
       tag = NULL;
       while ((tag = av_dict_iterate(fmt_ctx->streams[i]->metadata, tag)))
           printf("stream %u: %s=%s\n", i, tag->key, tag->value);
   }
   ```

   每路流开始前要把 `tag` 置回 NULL，让遍历从头开始。输出的最后几行：

   ```
   stream 0: language=und
   stream 0: handler_name=VideoHandler
   stream 0: encoder=Lavc63.28.100 libx264
   stream 1: language=chi
   stream 1: handler_name=SoundHandler
   ```

2. `two_audio.mkv` 里流 #1 是立体声、带 `default` 标志，流 #2 是 5.1 声道、不带。
   `av_find_best_stream` 返回 1。换了默认轨的 `two_audio_b.mkv` 返回 2。
   它首先看 `disposition`，默认轨优先，并不会因为声道多就选 5.1 那一条。
   需要按语言或声道数选轨时，要自己遍历 `streams` 判断。

3. `truncated.mp4` 打不开，报 `moov atom not found`：`moov` 在文件末尾，被截掉了。
   `truncated.mkv` 能打开，仍然报告时长 5.038 秒，因为 MKV 的描述信息在文件开头。
   加了 `+faststart` 的文件，顶层 box 的顺序变成 `ftyp`、`moov`、`free`、`mdat`，
   截断后能打开，并且仍然报告 5 秒、125 帧，虽然实际只剩不到五分之一的数据。
   结论有两条：网络点播的 MP4 要把 `moov` 放在前面，播放器才能边下边播；
   文件头里的时长和帧数只是记录，不代表数据真的完整。

4. 给对参数时是 1.00 秒：192,000 ÷ (48000 × 2 × 2)。不给参数时是 2.18 秒：
   解封装器按默认的 44100 Hz、单声道解释，192,000 ÷ (44100 × 1 × 2) = 2.177。
   两次都不报错。原始数据的参数给错了，FFmpeg 没有任何办法发现，只能靠耳朵听出来。

5. | 输入 | 错误文字 | 错误码 |
   | --- | --- | --- |
   | 不存在的文件 | `No such file or directory` | -2 |
   | 文本文件 | `Invalid data found when processing input` | -1094995529 |
   | `truncated.mp4` | `Invalid data found when processing input`，之前还有一行 `moov atom not found` | -1094995529 |

   -2 是系统错误码 `ENOENT` 取负，即 `AVERROR(ENOENT)`。-1094995529 是 FFmpeg 自己定义的
   `AVERROR_INVALIDDATA`。后两种情况错误码相同，只有日志能告诉你具体原因，所以排查问题时不要把日志关掉。

</details>
