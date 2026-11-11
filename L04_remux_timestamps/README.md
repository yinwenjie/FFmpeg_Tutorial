# L04 时间基与时间戳

上一讲打开了文件，看到了流。这一讲往下走一层，看流里的数据：一个一个的压缩数据包（`AVPacket`），
以及每个包上带的时间戳。然后跟着官方示例 `remux.c` 第一次写出文件：把包从一个容器原样搬到另一个容器，
不解码也不编码。这件事叫转封装（remux）。

时间戳是音视频开发里出问题最多的地方。画面卡顿、音画不同步、时长不对、文件写不出来，
追到最后大多是时间戳算错了。这一讲的内容后面每一讲都要用。

完成标志：`remux_cut` 截出来的片段能正常播放，并且能说清楚为什么要求从 2.5 秒开始，实际却是从 2 秒开始的。

## 本讲要点

**包（AVPacket）**

`av_read_frame()` 每次从容器里读出一个包。视频的一个包是一帧的压缩数据，音频的一个包是一个音频帧的压缩数据
（AAC 是 1024 个采样）。本讲用到的成员：

| 成员 | 含义 |
| --- | --- |
| `stream_index` | 这个包属于哪一路流 |
| `pts` | 显示时间戳（presentation timestamp）：这一帧应该在什么时刻显示 |
| `dts` | 解码时间戳（decoding timestamp）：这个包应该在什么时刻送进解码器 |
| `duration` | 这个包持续多长时间 |
| `flags` | `AV_PKT_FLAG_KEY` 表示关键帧 |
| `size`、`pos` | 数据的字节数、在文件里的位置 |

**为什么需要两个时间戳**

- 视频里的 B 帧要参考它后面的帧，所以后面的帧必须先解码。包在文件里按解码顺序存放，和显示顺序不同。
  `dts` 按存放顺序递增，`pts` 会前后跳。
- 没有 B 帧的视频、所有的音频，两个时间戳相等。
- 关键帧（I 帧）不参考任何其他帧，解码只能从关键帧开始。这决定了不重新编码的裁剪只能从关键帧起头。

**时间 = 整数 × 时间基**

- 时间戳都是整数，单位是时间基（time base），一个用 `AVRational` 表示的分数。秒数 = 时间戳 × 时间基。
- 时间基不止一个。`AVFormatContext` 里的 `duration`、`start_time` 以 1/1000000 秒（`AV_TIME_BASE_Q`）为单位；
  每路流有自己的 `AVStream.time_base`，包上的时间戳以它为单位；同一段内容换一个容器，时间基就不一样。
- 包从一路流搬到另一路流，时间戳必须换算。忘了换算是这一讲最重要的一个错误。

**换算工具**（`libavutil/rational.h`、`libavutil/mathematics.h`、`libavcodec/packet.h`）

| 函数 | 作用 |
| --- | --- |
| `av_q2d(q)` | 分数转成 double，只用于显示 |
| `av_inv_q(q)` | 取倒数，帧率的倒数就是一帧的时长 |
| `av_rescale_q(a, bq, cq)` | 把以 `bq` 为单位的 `a` 换算成以 `cq` 为单位，四舍五入，中间不会溢出 |
| `av_rescale_q_rnd()` | 同上，可以指定舍入方向 |
| `av_packet_rescale_ts(pkt, src, dst)` | 一次换算一个包的 `pts`、`dts`、`duration`，遇到 `AV_NOPTS_VALUE` 会跳过 |
| `av_compare_ts(a, tb_a, b, tb_b)` | 比较两个时间基不同的时间戳，不需要先换算 |

**写文件的固定写法**

1. `avformat_alloc_output_context2()`：创建输出上下文，容器格式由文件名的扩展名决定。
2. `avformat_new_stream()` + `avcodec_parameters_copy()`：为每一路要输出的流建一个 `AVStream`，把编码参数拷过去。
3. `avio_open()`：打开输出文件。
4. `avformat_write_header()`：写文件头。**输出流的 `time_base` 在这一步才确定。**
5. 循环：`av_read_frame()` 读包，`av_packet_rescale_ts()` 换算时间戳，`av_interleaved_write_frame()` 写包。
6. `av_write_trailer()`：写文件尾。然后关闭文件、释放上下文。

## 准备

```bash
scripts/gen_samples.sh                 # 本讲没有新增素材，已经生成过可以跳过
make -C L04_remux_timestamps           # 可执行文件在 L04_remux_timestamps/build/ 下
B=L04_remux_timestamps/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

## 第 1 步：packet_info，看包上的时间戳

[packet_info.c](packet_info.c) 用 `av_read_frame()` 把包一个一个读出来，打印上表里的成员。
每个时间戳打印两次：先是整数，再是乘上时间基之后的秒数。

```console
$ $B/packet_info samples/small_360p25.mp4 12
流 #0 video time_base = 1/12800
流 #1 audio time_base = 1/44100

   n st type       pts  pts_time      dts  dts_time duration    size      pos flags
   0  0 video        0    0.0000    -1024   -0.0800      512    8912       48 K_
   1  0 video     1024    0.0800     -512   -0.0400      512    4481     8960 __
   2  1 audio    -1024   -0.0232    -1024   -0.0232     1024     198    13441 KD
   3  0 video      512    0.0400        0    0.0000      512    3101    13639 __
   4  1 audio        0    0.0000        0    0.0000     1024      80    16740 K_
   5  1 audio     1024    0.0232     1024    0.0232     1024     131    16820 K_
   6  0 video     2048    0.1600      512    0.0400      512    4284    16951 __
   7  1 audio     2048    0.0464     2048    0.0464     1024      97    21235 K_
   8  1 audio     3072    0.0697     3072    0.0697     1024     101    21332 K_
   9  0 video     1536    0.1200     1024    0.0800      512    2964    21433 __
  10  1 audio     4096    0.0929     4096    0.0929     1024     107    24397 K_
  11  1 audio     5120    0.1161     5120    0.1161     1024     118    24504 K_
  ……（共 342 个包，只打印了前 12 个）

== 统计 ==
流 #0 video  包 125  关键帧 5  pts≠dts 125  数据 481605 字节
           pts 最小 0.0000 秒  pts 最大 4.9600 秒  首个 dts -0.0800 秒  末个 dts 4.8800 秒  dts 一直递增
流 #1 audio  包 217  关键帧 217  pts≠dts 0  数据 76214 字节
           pts 最小 -0.0232 秒  pts 最大 4.9923 秒  首个 dts -0.0232 秒  末个 dts 4.9923 秒  dts 一直递增
```

和 ffprobe 逐项核对（`packet_info` 的第二个参数给 0 表示打印全部）：

```bash
ffprobe -v error -show_entries packet=stream_index,pts,dts,duration,size,pos,flags -of csv=p=0 samples/small_360p25.mp4
```

这张表要读出五件事。

**整数和时间基**。视频流的时间基是 1/12800，一帧 1/25 秒，所以 `duration` 是 12800 ÷ 25 = 512。
音频流的时间基是 1/44100，正好是一个采样的时长，所以时间戳就是采样的个数，一个 AAC 包是 1024。
两路流的整数不能直接比较，要乘上各自的时间基之后才是同一个单位。

**pts 和 dts**。只看视频的五个包，并用 ffprobe 查出它们的帧类型：

```console
$ ffprobe -v error -select_streams v -show_entries frame=pts_time,pict_type -of csv=p=0 samples/small_360p25.mp4 | head -5
0.000000,I
0.040000,B
0.080000,P
0.120000,B
0.160000,P
```

| 包的序号 n | 0 | 1 | 3 | 6 | 9 |
| --- | --- | --- | --- | --- | --- |
| `dts_time`（存放和解码的顺序） | -0.08 | -0.04 | 0.00 | 0.04 | 0.08 |
| `pts_time`（显示的时刻） | 0.00 | 0.08 | 0.04 | 0.16 | 0.12 |
| 帧类型 | I | P | B | P | B |

显示顺序是 I B P B P，存放顺序是 I P B P B。0.04 秒的 B 帧要参考 0.08 秒的 P 帧，所以 P 帧排在它前面。
`dts` 一直递增，`pts` 前后跳。音频没有这种依赖，217 个包的 `pts` 和 `dts` 全部相等。

**负的时间戳**。一帧要先解码才能显示，所以每个包的 `dts` 都不能晚于 `pts`。
这个视频最多要提前解码两帧，编码器就把 `dts` 整体提前了两帧，第一个包的 `dts` 是 -0.08 秒。
负的时间戳是正常现象，程序不能假设时间戳从 0 开始，也不能假设它不是负数。
音频的第一个包 `pts` 是 -1024，`flags` 里的 `D` 是 `AV_PKT_FLAG_DISCARD`：
这是 AAC 编码器开头多出来的一个包，要送进解码器，但解出来的声音不播放。

**关键帧**。`flags` 里的 `K` 表示关键帧。视频 125 个包里只有 5 个，在 0、1、2、3、4 秒处（素材每 25 帧一个关键帧）。
音频的每个包都能独立解码，所以全是 `K`。

**交错**。把两路流放在一起看 `dts_time` 这一列：-0.08、-0.04、-0.0232、0、0、0.0232、0.04……是从小到大的。
`pos` 也是递增的。时间上挨着的音频和视频，在文件里也挨着，播放器顺序读下去就能同时拿到声音和画面。
这种存放方式叫交错（interleave）。

同样的内容换一个容器（`for e in mkv ts flv; do $B/packet_info samples/small_360p25.$e 5; done`）：

| | MP4 | MKV | MPEG-TS | FLV |
| --- | --- | --- | --- | --- |
| 视频流 `time_base` | 1/12800 | 1/1000 | 1/90000 | 1/1000 |
| 音频流 `time_base` | 1/44100 | 1/1000 | 1/90000 | 1/1000 |
| 一帧视频的 `duration` | 512 | 40 | 3600 | 40 |
| 一个音频包的 `duration` | 1024 | 23 | 2089 | 23 |
| 第一个视频包的 `pts` / `dts` | 0 / -1024 | 0 / N/A | 133200 / 126000 | 80 / 0 |
| 第一个视频包的 `pts_time` | 0.00 | 0.00 | 1.48 | 0.08 |

- 时间基由容器决定。MP4 每路流可以有自己的时间基，MKV 和 FLV 固定用毫秒，TS 固定用 1/90000 秒。
- 毫秒表示不了 23.22 毫秒，MKV 和 FLV 里一个音频包的时长只能记成 23。第 2 步和第 4 步会看到后果。
- MKV 每个包只存一个时间戳，开头两个视频包的 `dts` 读出来是 `N/A`，也就是 `AV_NOPTS_VALUE`。
- FLV 存不了负的时间戳，写文件时全部时间戳被整体后移了 0.08 秒。TS 的时间戳从 1.4 秒左右开始，L03 已经见过。

结论：同一个包在不同的容器里，时间戳的整数完全不同。把包从一个容器搬到另一个容器，必须换算。

## 第 2 步：timebase_math，时间基换算

[timebase_math.c](timebase_math.c) 不读文件，只演示几个换算函数。数字都取自上一步。

```console
$ $B/timebase_math
== 1. AVRational ==
  帧率 frame_rate = 25/1（25.000000）
  一帧的时长 av_inv_q(frame_rate) = 1/25（0.040000）
  MP4 视频流的时间基 = 1/12800（0.000078）
  pts 为 64000 时的秒数 = 64000 × 1/12800 = 5.000

== 2. av_rescale_q：同一段时间在不同时间基下的整数 ==
  一帧视频（1/25 秒）  1/12800 下是 512，1/1000 下是 40，1/90000 下是 3600，1/1000000 下是 40000
  一个 AAC 包（1024 个采样）  1/44100 下是 1024，1/1000 下是 23，1/90000 下是 2090

== 3. 舍入：换算到更粗的时间基会丢精度 ==
  1024 个采样的准确时长是 23.2200 毫秒
  换算到 1/1000：23，再换算回 1/44100：1014，少了 10 个采样
  向下舍入 AV_ROUND_DOWN：23，向上舍入 AV_ROUND_UP：24

== 4. 换算时间戳，不要累加换算后的时长 ==
  n 是包的序号，三列数字的单位都是毫秒
         n  rescale(n * 1024)  n * rescale(1024)     diff
         1                 23                 23        0
        10                232                230        2
       100               2322               2300       22
       217               5039               4991       48
     43066             999991             990518     9473
  217 个包是素材的长度（5 秒），43066 个包大约是 1000 秒。

== 5. 溢出：为什么不自己写乘除法 ==
  时间戳 1794441600000000（单位 1/1000000）换算到 1/90000：
  自己算 ts * 90000 / 1000000：乘法溢出，结果是错的
  av_rescale_q() 的结果：161499744000000

== 6. av_compare_ts：比较不同时间基下的两个时间戳 ==
  视频包 dts = 512（0.0400 秒），音频包 dts = 2048（0.0464 秒）
  av_compare_ts() 返回 -1：视频包更早，应该先写
```

要记住的三条：

- **用分数，不用小数**。时间基是分数，换算全程是整数运算。`av_q2d()` 得到的 double 只用来打印。
  用 double 算时间戳，迟早会遇到 0.1 + 0.2 ≠ 0.3 这类问题。
- **每个时间戳单独换算，不要累加换算后的时长**（第 4 段）。每个包只差 0.22 毫秒，
  5 秒的素材累加下来差 48 毫秒，1000 秒差 9.5 秒。直播推流跑上几个小时音画不同步，常见的原因就是这个。
- **不要自己写 `ts * num / den`**（第 5 段）。时间戳是 64 位整数，乘法会溢出，而且没有任何报错。
  `av_rescale_q()` 内部处理了溢出。

## 第 3 步：读官方示例 remux.c

[remux.c](remux.c) 是官方示例的原样拷贝，加了中文注释。去掉错误处理，骨架是这样的：

```c
avformat_open_input(&ifmt_ctx, in_filename, 0, 0);                    /* 输入，和 L03 一样 */
avformat_find_stream_info(ifmt_ctx, 0);

avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);  /* 1 创建输出上下文 */
for (i = 0; i < ifmt_ctx->nb_streams; i++) {                          /* 2 建输出流，拷编码参数 */
    out_stream = avformat_new_stream(ofmt_ctx, NULL);
    avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
    out_stream->codecpar->codec_tag = 0;
}
avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);              /* 3 打开输出文件 */
avformat_write_header(ofmt_ctx, NULL);                                /* 4 写文件头 */

while (av_read_frame(ifmt_ctx, pkt) >= 0) {                           /* 5 搬运每一个包 */
    av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
    pkt->pos = -1;
    av_interleaved_write_frame(ofmt_ctx, pkt);
}
av_write_trailer(ofmt_ctx);                                           /* 6 写文件尾 */
```

把 MP4 换成 MKV。示例会打印每个包换算前（`in`）和换算后（`out`）的时间戳，一共 684 行，先存进文件再看：

```console
$ $B/remux samples/small_360p25.mp4 out/remux.mkv > out/remux.log 2> /dev/null
$ head -6 out/remux.log
in: pts:0 pts_time:0 dts:-1024 dts_time:-0.08 duration:512 duration_time:0.04 stream_index:0
out: pts:0 pts_time:0 dts:-80 dts_time:-0.08 duration:40 duration_time:0.04 stream_index:0
in: pts:1024 pts_time:0.08 dts:-512 dts_time:-0.04 duration:512 duration_time:0.04 stream_index:0
out: pts:80 pts_time:0.08 dts:-40 dts_time:-0.04 duration:40 duration_time:0.04 stream_index:0
in: pts:-1024 pts_time:-0.02322 dts:-1024 dts_time:-0.02322 duration:1024 duration_time:0.02322 stream_index:1
out: pts:-23 pts_time:-0.023 dts:-23 dts_time:-0.023 duration:23 duration_time:0.023 stream_index:1
```

整数变了（-1024 变成 -80，512 变成 40），秒数没变（-0.08、0.04）。这就是 `av_packet_rescale_ts()` 做的事。
音频包的时长从 0.02322 秒变成了 0.023 秒，是第 2 步见过的舍入。

不要图省事写成 `$B/remux ... | head -6`。`head` 读够 6 行就退出，`remux` 随即被系统终止，
来不及把数据和文件尾写出去，实测留下的是一个 0 字节的文件。

怎么确认数据确实是原样搬过去的？`streamhash` 这个输出格式会对每一路流的全部数据算一个校验和：

```console
$ for f in samples/small_360p25.mp4 out/remux.mkv; do ffmpeg -v error -i $f -map 0 -c copy -f streamhash -hash md5 - | tail -2; done
0,      481605, 58278ed8b8a76cbc5fd228bee8fd00f9
1,       76214, 8f17f1c4034f9d1f09ccf961825dbe18
0,      481605, 58278ed8b8a76cbc5fd228bee8fd00f9
1,       76214, 8f17f1c4034f9d1f09ccf961825dbe18
```

两个文件里，视频都是 481,605 字节，音频都是 76,214 字节，校验和相同。压缩数据一个字节都没有变，
所以转封装没有任何画质损失，而且很快：7.8 MB 的 `demo_1080p30.mp4` 转成 MKV 用时不到 0.1 秒。
命令行里的 `ffmpeg -i in.mp4 -c copy out.mkv` 做的就是这件事。

代码里要注意的几处：

- **`codec_tag = 0`**。`codec_tag` 是容器里标识编码的四个字符（L03 见过 `avc1`、`mp4a`），每种容器有自己的一套。
  清零是让复用器按新容器重新选。不清零的话，MP4 转 MKV 会在写文件头时失败：
  `Tag mp4a incompatible with output codec id '86018'`。
- **`out_stream->time_base` 要在 `avformat_write_header()` 之后读**。示例在循环里每次现取，是对的。
  在写文件头之前把它存进变量，存到的是还没确定的值。
- **`av_interleaved_write_frame()` 会接管包**。调用之后 `pkt` 被清空，不需要再 `av_packet_unref()`，
  也不能再读它的成员。
- **`av_write_trailer()` 不能省**。MP4 的 `moov` 就是这一步写的（练习 3）。

这个示例作为入门很合适，拿来当工程代码的模板还有几处不足：

| 问题 | 现象 |
| --- | --- |
| 不拷贝元数据和 `disposition` | 输出文件里语言、标题、默认轨标志都丢了 |
| 不处理没有 `dts` 的包 | 输入是 MKV 时打印 `Timestamps are unset in a packet for stream 0. This is deprecated and will stop working in the future.` |
| 错误码被替换成 `AVERROR_UNKNOWN` | 创建输出失败时，最后报的是 `Unknown error occurred`，真实的错误码丢了 |
| `in:` 那行日志打印得太晚 | 输入里有被跳过的流时，`in:` 一行用错了流的序号和时间基 |

这四条已经整理成 [upstream_patches](../upstream_patches/README.md) 里的 0012 到 0015 号 patch，
复现方法见那里的说明。另外，转封装并不是任意两种容器之间都能直接搬：

```console
$ $B/remux samples/small_360p25.mp4 out/remux.avi 2>&1 | grep -v -e '^in:' -e '^out:' | tail -3
Error muxing packet
[avi @ 0x...] h264 bitstream malformed, no startcode found, use the video bitstream filter 'h264_mp4toannexb' to fix it ('-bsf:v h264_mp4toannexb' option with ffmpeg)
Error occurred: Invalid data found when processing input
```

H.264 的数据在 MP4 里和在 AVI、TS 里的打包方式不同，需要一个叫比特流滤镜（bitstream filter）的转换步骤。
TS 的复用器会自动加上这一步，AVI 的不会。比特流滤镜在后面的课里再讲，这里知道有这回事就够了。

## 第 4 步：remux_cut，工程上该怎么写

[remux_cut.c](remux_cut.c) 是本讲的实践代码。不带选项时它做的事和 `remux.c` 相同，但补上了上面那几处不足，
并且把复用器选定的时间基打印出来：

```console
$ $B/remux_cut samples/small_360p25.mp4 out/full.mkv
输出格式：matroska
  输入流 #0 -> 输出流 #0 video  time_base 建议 1/12800，实际 1/1000
  输入流 #1 -> 输出流 #1 audio  time_base 建议 1/44100，实际 1/1000
  输出流 #0 写出 125 个包
  输出流 #1 写出 217 个包
```

**建议的时间基和实际的时间基**。写文件头之前可以给 `out_stream->time_base` 赋一个值，这只是建议。
`avformat_write_header()` 会把它改成复用器实际使用的值：

| 输入 → 输出 | 视频：建议 → 实际 | 音频：建议 → 实际 |
| --- | --- | --- |
| MP4 → MKV | 1/12800 → 1/1000 | 1/44100 → 1/1000 |
| MP4 → TS | 1/12800 → 1/90000 | 1/44100 → 1/90000 |
| MP4 → FLV | 1/12800 → 1/1000 | 1/44100 → 1/1000 |
| MP4 → MP4 | 1/12800 → 1/12800 | 1/44100 → 1/44100 |
| MKV → MP4 | 1/1000 → 1/16000 | 1/1000 → 1/44100 |
| TS → MP4 | 1/90000 → 1/90000 | 1/90000 → 1/44100 |

只有 MP4 采纳了一部分建议，其余容器都用自己固定的值。所以换算时间戳时，
目标时间基一定要用写完文件头之后的 `out_stream->time_base`，不能用自己设进去的那个值。

**元数据和 disposition**。多了三行拷贝代码，输出文件里的标签就保住了。用 L03 做的 `out/tagged.mp4` 验证
（没有这个文件的话，按 L03 第 2 步的命令生成）：

```console
$ $B/remux_cut out/tagged.mp4 out/tagged_cut.mkv > /dev/null
$ ffprobe -v error -show_entries format_tags=title:stream_tags=language -of default=nw=1 out/tagged_cut.mkv
TAG:language=chi
TAG:title=FFmpeg 课程 L03
```

同一个输入交给官方的 `remux`，这条 ffprobe 命令什么都打印不出来。

**补 dts**。输入是 MKV 时开头的包没有 `dts`，`fill_missing_dts()` 按“第一个包的 `pts` 减去提前解码的帧数，
之后每个包加一个时长”的规则补上。`remux_cut samples/small_360p25.mkv out/from_mkv.mp4` 不再有那条 deprecated 警告。

**毫秒时间基的代价**。把 MP4 转成 MKV，再转回 MP4，看音频的时间戳：

```console
$ $B/remux_cut samples/small_360p25.mp4 out/rt.mkv > /dev/null
$ $B/remux_cut out/rt.mkv out/rt.mp4 > /dev/null
$ $B/packet_info out/rt.mp4 12 | grep 'audio.* K'
   2  1 audio    -1014   -0.0230    -1014   -0.0230     1014     198    13441 KD
   4  1 audio        0    0.0000        0    0.0000     1014      80    16740 K_
   5  1 audio     1014    0.0230     1014    0.0230     1015     131    16820 K_
   7  1 audio     2029    0.0460     2029    0.0460     1058      97    21235 K_
   8  1 audio     3087    0.0700     3087    0.0700     1014     101    21332 K_
  10  1 audio     4101    0.0930     4101    0.0930     1015     107    24397 K_
  11  1 audio     5116    0.1160     5116    0.1160     1014     118    24504 K_
```

原来整整齐齐的 0、1024、2048、3072 变成了 0、1014、2029、3087，时长在 1014 和 1058 之间跳。
数据没有变（`streamhash` 仍然相同），播放也正常，但精确到采样的时间信息丢了，转回来也恢复不了。
好在每个时间戳是单独换算的，误差不会累积：217 个包里偏差最大的一个是 22 个采样，也就是 0.5 毫秒。

**交错写入**。`av_interleaved_write_frame()` 不会立刻把包写进文件。它在内部把各路流的包暂存起来，
按 `dts` 排好顺序再写，保证输出文件是交错的。另一个函数 `av_write_frame()` 则是给什么写什么。
除非自己已经排好了顺序，否则都用 `av_interleaved_write_frame()`。两者的差别见练习 4。

## 第 5 步：裁剪，以及为什么起点落在关键帧上

`remux_cut` 的三个选项：`-ss` 起点、`-to` 终点（单位是秒，从文件开头算起，和 ffmpeg 的同名选项含义相同）、
`-video-only` 只保留视频。截取 2.5 秒到 4 秒：

```console
$ $B/remux_cut samples/small_360p25.mp4 out/cut.mp4 -ss 2.5 -to 4
输出格式：mp4
  输入流 #0 -> 输出流 #0 video  time_base 建议 1/12800，实际 1/12800
  输入流 #1 -> 输出流 #1 audio  time_base 建议 1/44100，实际 1/44100
请求的起点 2.500 秒，实际从 2.000 秒处的关键帧开始
  输出流 #0 写出 52 个包
  输出流 #1 写出 86 个包
$ ffprobe -v error -show_entries format=duration -of csv=p=0 out/cut.mp4
2.080000
```

要的是 1.5 秒，得到的是 2.08 秒。起点和终点都没有落在要求的位置上，原因各不相同。

**起点：只能从关键帧开始**。定位用的是这一行：

```c
av_seek_frame(ifmt_ctx, -1, start_ts, AVSEEK_FLAG_BACKWARD);
```

- 第二个参数是流的序号。给 -1 时，时间戳以微秒（`AV_TIME_BASE`）为单位，由 FFmpeg 选一路流来定位。
- `AVSEEK_FLAG_BACKWARD` 的意思是：定位到目标时刻之前最近的一个关键帧。2.5 秒之前最近的关键帧在 2 秒。
- 定位之后，`av_read_frame()` 从那个关键帧开始往下读。

为什么不能从 2.5 秒那一帧开始？那一帧不是关键帧，它要参考前面的帧，前面的帧又参考更前面的，
一直追到 2 秒处的关键帧。不重新编码，就只能把这一整串都带上。

**时间戳要平移**。从 2 秒开始读出来的包，时间戳是从 2 秒起算的。输出文件应该从 0 开始，
所以程序把第一个关键帧的 `pts` 当作零点，后面每个包的 `pts`、`dts` 都减去它，然后再换算时间基。
先平移、后换算，两步的顺序不能反，因为零点是用输入一侧的时间基表示的。

**终点：按 dts 判断**。程序在某个包的 `dts` 到达 4 秒时停止。包是按解码顺序排的，
从某个包起整段去掉，留下的包一定都能解码。代价是末尾不整齐，看原文件 4 秒附近的视频包：

| `dts_time` | 3.88 | 3.92 | 3.96 | 4.00 | 4.04 |
| --- | --- | --- | --- | --- | --- |
| `pts_time` | 3.96 | 4.00 | 4.08 | 4.04 | 4.16 |
| 结果 | 保留 | 保留 | 保留 | 丢弃 | 丢弃 |

`pts` 为 4.00 和 4.08 的两帧因为 `dts` 小于 4 秒被留下了，夹在中间的 4.04 那一帧被丢掉了。
所以片段的结尾超出了 4 秒，并且最后少一帧。按 `pts` 判断可以让结尾整齐，但可能留下一个参考帧已经被丢掉的 B 帧。

这不是这个程序特有的毛病。ffmpeg 命令行做同样的事，留下的是同样的 52 个视频包：

```console
$ ffmpeg -v error -y -ss 2.5 -to 4 -i samples/small_360p25.mp4 -c copy out/cli_cut.mp4
$ ffprobe -v error -select_streams v -count_packets -show_entries stream=nb_read_packets -of csv=p=0 out/cli_cut.mp4
52
```

想精确到帧，就必须把起点所在的那一段解码再重新编码。那是 L16 的内容。

**不同的容器，定位的结果不同**：

```console
$ for e in mkv flv ts; do $B/remux_cut samples/small_360p25.$e out/cut_from_$e.mp4 -ss 2.5 -to 4 | grep 请求; done
请求的起点 2.500 秒，实际从 2.000 秒处的关键帧开始
请求的起点 2.500 秒，实际从 2.023 秒处的关键帧开始
请求的起点 2.500 秒，实际从 3.023 秒处的关键帧开始
```

MP4 和 MKV 有索引，记录着每个关键帧的时刻和位置，定位是查表。FLV 找到的是同一个关键帧，
多出的 0.023 秒是因为这个文件的起始时间取的是音频的第一个包（0.057 秒），而视频从 0.080 秒才开始。
TS 没有索引，只能按时间戳找一个大概的位置再往后读，读到的第一个关键帧在目标之后，是 3 秒处的那个。
ffmpeg 命令行对这个 TS 文件的结果相同。
所以定位之后一定要看实际读到的第一个包在哪里，不能假设它就在要求的位置。

## 常见错误

**忘了换算时间戳，或者换算用错了时间基**
MP4 转 MP4 时两边时间基碰巧相同，看不出问题；换成 MKV 输出，5 秒的文件变成 222 秒（练习 2）。
只测过一种容器的代码不算测过。

**在 `avformat_write_header()` 之前读取输出流的 `time_base`**
那时读到的是自己设的建议值或者 0/0。必须在写完文件头之后读。

**忘了 `av_write_trailer()`，或者出错时直接退出**
MP4 文件没有 `moov`，根本打不开（练习 3）。录制程序被强行结束后文件损坏，就是这个原因。

**自己做时间戳运算**
用 double 换算、自己写乘除法、把换算后的时长逐个累加，这三种写法都会出错，第 2 步各有演示。

**把 `AV_NOPTS_VALUE` 当成数字**
它是 -9223372036854775808。拿它去加减或换算，得到的是一个看起来像时间戳的巨大数字。
`av_packet_rescale_ts()` 会跳过它；自己调用 `av_rescale_q()` 或者做加减之前，要先判断。

**`av_interleaved_write_frame()` 之后继续使用 `pkt`**
函数返回后 `pkt` 已经被清空，不管成功还是失败。要统计包的大小或时间，在调用之前取出来。

**以为不重新编码的裁剪能精确到帧**
起点由关键帧的位置决定。关键帧间隔 8 秒的文件，误差可以到 8 秒（练习 1）。

## 练习

1. 用 `packet_info` 找出 `samples/demo_1080p30.mp4` 的全部关键帧（提示：`grep 'video.* K_$'`）。
   然后执行 `$B/remux_cut samples/demo_1080p30.mp4 out/demo_cut.mp4 -ss 5 -to 6`，输出有多长？为什么？
2. 把 `remux_cut.c` 里 `av_packet_rescale_ts(...)` 那一行注释掉，重新编译，分别转成 MP4 和 MKV。
   用 `ffprobe -v error -show_entries format=duration -of csv=p=0` 看两个输出的时长，并解释。做完把代码改回来。
3. 把 `ret = av_write_trailer(ofmt_ctx);` 改成 `ret = 0;`，重新编译，分别输出 MP4、MKV、TS、FLV。
   哪些还能打开？各少了什么？做完把代码改回来。
4. 把 `av_interleaved_write_frame(ofmt_ctx, pkt)` 换成 `av_write_frame(ofmt_ctx, pkt)`，并在它后面加一句
   `av_packet_unref(pkt);`。用 `samples/small_360p25.ts` 作输入转成 MP4，再用下面的命令比较修改前后包的顺序。
   为什么要选 TS 作输入？
   ```bash
   order() { $B/packet_info "$1" 0 | awk '$1 ~ /^[0-9]+$/ { printf "%s", $3 == "video" ? "V" : "A" }' | cut -c1-60; }
   order samples/small_360p25.ts; order out/你的输出.mp4
   ```
5. 把 `av_seek_frame()` 的最后一个参数从 `AVSEEK_FLAG_BACKWARD` 改成 0，对 `samples/small_360p25.mp4`
   分别用 `-ss 2.5` 和 `-ss 4.5` 运行，结果是什么？

<details>
<summary>参考答案</summary>

1. 300 个视频包里只有两个关键帧：

   ```
      0  0 video        0    0.0000    -1024   -0.0667      512   42224       48 K_
    639  0 video   128000    8.3333   126976    8.2667      512   54412  6504422 K_
   ```

   `-ss 5 -to 6` 的输出：

   ```
   请求的起点 5.000 秒，实际从 0.000 秒处的关键帧开始
     输出流 #0 写出 182 个包
     输出流 #1 写出 282 个包
   ```

   文件长 6.07 秒。5 秒之前最近的关键帧在 0 秒，所以想要的 1 秒变成了 6 秒。
   这个素材每 250 帧（8.33 秒）才有一个关键帧，是编码器 libx264 的默认设置。
   关键帧间隔越大压缩率越高，但定位和裁剪越不精确，播放器拖动进度条之后出画面也越慢。
   需要频繁裁剪或者用于直播的视频，关键帧间隔通常设在 1 到 2 秒。

2. 转成 MP4 时长是 5.000000，一切正常；转成 MKV 时长是 222.208000，还会打印一长串
   `Starting new cluster due to backward timestamp jump` 警告。
   MP4 复用器采纳了建议的时间基，输入输出的时间基相同，不换算也碰巧是对的。
   MKV 的时间基是 1/1000：音频最后一个包的 `pts` 是 220160（单位是采样），被当成 220160 毫秒，也就是 220 秒；
   视频最后的 `pts` 是 63488（4.96 秒 × 12800），被当成 63 秒。两路流的时间完全对不上。

3. | 输出 | 文件大小（正常 → 不写文件尾） | 结果 |
   | --- | --- | --- |
   | MP4 | 563,196 → 555,638 | 打不开，`moov atom not found` |
   | MKV | 561,297 → 450,733 | 能打开，时长未知，只有 100 个视频包和 169 个音频包 |
   | TS | 640,516 → 636,568 | 能打开，视频完整，少了最后 6 个音频包 |
   | FLV | 564,497 → 562,163 | 能打开，视频完整，少了最后 5 个音频包 |

   `av_write_trailer()` 做两件事：把复用器内部还暂存着的包写出去，再写容器自己的收尾信息。
   MP4 的全部索引都在收尾时写，所以损失最大。MKV 按簇（cluster）缓存数据，最后一簇没有写出去。
   TS 和 FLV 没有集中的索引，只丢了排队等待交错的最后几个包。
   这也是直播和录制场景偏爱 TS、FLV 的原因之一：程序崩溃了，已经写下的部分还能用。

4. 三次的结果：

   ```
   VVVVVVVVVAAAAAAAAAAAAAAAAVVVVVAAAAAAAAAVVVVVVAAAAAAAAAVVVVAA    输入的 TS
   VVAVAAVAAVAAVAVAAVAAVAAVAVAAVAAVAVAAVAAVAAVAVAAVAAVAAVAVAAVA    av_interleaved_write_frame
   VVVVVVVVVAAAAAAAAAAAAAAAAVVVVVAAAAAAAAAVVVVVVAAAAAAAAAVVVVAA    av_write_frame
   ```

   TS 文件里的包是成片存放的：先是 9 个视频包（`dts` 从 1.40 到 1.72 秒），再是 16 个音频包（1.46 到 1.81 秒）。
   `av_write_frame()` 照原样写进了 MP4。`av_interleaved_write_frame()` 把它们按 `dts` 重新排好，
   得到和原始 MP4 一样的细密交错。选 TS 作输入，是因为 MP4、MKV、FLV 读出来的包本身已经排好了顺序，
   两个函数的结果没有区别。等到 L15 自己编码时，音频和视频来自两个编码器，顺序完全没有保证，那时差别更明显。

5. `-ss 2.5` 的实际起点变成 3.000 秒：标志为 0 时，定位到目标之后最近的关键帧。
   `-ss 4.5` 直接失败：

   ```
   定位到 4.500 秒失败：Operation not permitted
   ```

   4.5 秒之后已经没有关键帧了。错误文字有误导性：`av_seek_frame()` 这里返回的是 -1，
   它只表示“失败”，碰巧和系统错误码 `EPERM` 的值相同，和权限没有关系。带 `AVSEEK_FLAG_BACKWARD` 时，
   哪怕 `-ss 10` 超出了文件长度，也能定位到最后一个关键帧（4 秒处）。
   裁剪一般希望“宁可多不要少”，所以用 `AVSEEK_FLAG_BACKWARD`。

   还有一个标志 `AVSEEK_FLAG_ANY`，允许定位到非关键帧。用它从 2.5 秒附近的非关键帧开始写文件，
   实测写出的 61 个视频包里只有后 50 个能解码，前 11 个要等到 3 秒处的关键帧出现之后才有帧可以参考，全部白搬了。

</details>
