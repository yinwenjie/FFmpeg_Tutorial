# L07 解封装加解码全流程

L03、L04 学了怎么从容器里读包，L05、L06 学了怎么把包解码成帧，但那两讲为了把解码单独拿出来，读的都是没有容器的裸流。
这一讲把两头接起来：打开一个 MP4，找出视频流和音频流，各开一个解码器，把读到的包分给它们，得到图像和声音。
播放器、转码工具、截图工具，所有要“看到内容”的程序，开头都是这一段代码。

接起来只多了一件新事情：**解码器的参数从哪里来**。读裸流时解码器靠自己从码流里读参数；
读容器时，参数记在容器里，要由程序交给解码器。这一步漏掉，MP4 就解不出来。

完成标志：`decode_av` 解出的 YUV 和 PCM 与 ffmpeg 命令行的结果逐字节相同；
能说清楚 `avcodec_parameters_to_context()` 拷的是什么，为什么 MP4 少了它不行而 MPEG-TS 可以。

## 本讲要点

**整个流程**

```text
                     ┌─ 视频包 ─→ 视频解码器 ─→ 图像帧（AVFrame）─→ YUV 文件
文件 ─→ 解封装器 ─→ 包 ┤
                     └─ 音频包 ─→ 音频解码器 ─→ 音频帧（AVFrame）─→ PCM 文件
     av_read_frame()   按 stream_index 分发   send_packet / receive_frame
```

| 步骤 | 函数 | 哪一讲学的 |
| --- | --- | --- |
| 打开文件，补全流参数 | `avformat_open_input()`、`avformat_find_stream_info()` | L03 |
| 选出要解码的流 | `av_find_best_stream()` | L03 |
| 找解码器，建上下文 | `avcodec_find_decoder()`、`avcodec_alloc_context3()` | L05 |
| **把容器里的参数交给解码器** | `avcodec_parameters_to_context()`，再设置 `pkt_timebase` | 本讲 |
| 打开解码器 | `avcodec_open2()` | L05 |
| 读包，分发 | `av_read_frame()`，看 `pkt->stream_index` | L03、L04 |
| 解码 | `avcodec_send_packet()`、`avcodec_receive_frame()` | L05、L06 |
| 冲刷 | 对每个解码器送一次 `NULL`，把帧取完 | L05 |

**AVCodecParameters：容器记下的编码参数**（`libavcodec/codec_par.h`）

每一路流有一个 `AVStream.codecpar`。L03 用它打印过流的信息，这一讲用它建立解码器。

| 成员 | 含义 |
| --- | --- |
| `codec_type`、`codec_id` | 视频还是音频，哪一种编码。按 `codec_id` 找解码器 |
| `width`、`height`、`format` | 视频的宽高和像素格式 |
| `sample_rate`、`ch_layout`、`format` | 音频的采样率、声道布局和采样格式 |
| `profile`、`level`、`bit_rate` | 编码档次、级别、码率 |
| `extradata`、`extradata_size` | 解码器初始化要用、但不放在每个包里的数据。内容由编码格式规定 |

`AVCodecParameters` 只是一份描述，`AVCodecContext` 才是干活的解码器。
`avcodec_parameters_to_context()` 把前者的内容拷进后者，必须在 `avcodec_open2()` 之前调用。

**extradata 和两种存放方式**

同一种编码放进不同的容器，存放方式可以不一样。最典型的是 H.264 和 AAC：

| | 参数放在哪里 | 包里是什么 | 用在哪些容器 |
| --- | --- | --- | --- |
| H.264，AVCC 方式 | SPS、PPS 只在 extradata 里 | 每个 NAL 单元前面是 4 字节的长度 | MP4、MKV、FLV |
| H.264，Annex B 方式 | SPS、PPS 在码流里，每个关键帧前重复一遍 | 每个 NAL 单元前面是起始码 `00 00 00 01` | MPEG-TS、`.h264` 裸流 |
| AAC，不带头 | 采样率、声道数在 extradata 里（AudioSpecificConfig） | 只有压缩数据 | MP4、MKV、FLV |
| AAC，ADTS | 每一帧前面有 7 字节的头，里面有采样率、声道数 | 头加压缩数据 | MPEG-TS、`.aac` 裸流 |

MP4 这一类容器的想法是：参数全文件只需要一份，放在文件头里；包的边界容器自己知道，不需要起始码。
MPEG-TS 是为广播设计的，观众随时可能从中间开始收看，所以参数要在码流里反复出现。
L05、L06 的裸流程序读不了 MP4，原因就在这里：MP4 里的包既没有起始码，也没有参数。

**帧的时间戳**

- 解封装器给每个包填了 `pts` 和 `dts`，单位是这路流的时间基 `AVStream.time_base`（L04）。
- 解码器把时间戳从包带到帧上。`frame->pts` 的单位仍然是流的时间基，换算成秒要用 `AVStream.time_base`。
- 包按 dts 的顺序送进去，帧按 pts 的顺序出来。有 B 帧时两个顺序不同，解码器负责把顺序调回来。
- `AVCodecContext.pkt_timebase` 告诉解码器包的时间戳用的是什么时间基。建议每次都设置，
  解码器要丢掉开头的采样（MP4 里的 AAC）时靠它修正时间戳。
- `frame->best_effort_timestamp`：pts 有效时就是 pts；有的容器不记录 pts，FFmpeg 会估计一个值放在这里。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L07_demux_decode               # 可执行文件在 L07_demux_decode/build/ 下
B=L07_demux_decode/build
mkdir -p out
```

下面的命令都在仓库根目录执行。用到的素材 `samples/small_360p25.mp4` 是 640x360、25 帧每秒、5 秒的 H.264 视频加 44100 Hz 双声道 AAC 音频，
`.ts`、`.mkv`、`.flv` 是同一份内容换了容器。

## 第 1 步：容器里记了哪些参数

`codec_params` 只读容器、不解码，打印每一路流的 `AVCodecParameters`，以及每一路流第一个包开头的 16 个字节。

```console
$ $B/codec_params samples/small_360p25.mp4
容器格式：mov,mp4,m4a,3gp,3g2,mj2

流 #0：video
  codec_id      h264，默认解码器 h264
  profile/level High / 30
  bit_rate      770568 b/s
  宽高          640x360
  format        yuv420p
  extradata     47 字节： 01 64 00 1e ff e1 00 1a 67 64 00 1e ac d9 40 a0 ...
流 #1：audio
  codec_id      aac，默认解码器 aac
  profile/level LC / -99
  bit_rate      121005 b/s
  采样率        44100 Hz
  声道布局      stereo
  format        fltp
  frame_size    1024
  extradata     5 字节： 12 10 56 e5 00

每一路流的第一个包：
  流 #0    8912 字节： 00 00 02 ac 06 05 ff ff a8 dc 45 e9 bd e6 d9 48 ...
  流 #1     198 字节： de 02 00 4c 61 76 63 36 33 2e 32 38 2e 31 30 30 ...
```

解码器需要的东西都在这里了。看几个字节：

- 视频的 extradata 开头 `01 64 00 1e`：版本 1，profile 是 0x64 = 100（High），level 是 0x1e = 30。
  这是 AVCC 方式的配置记录，SPS 和 PPS 跟在后面（`67` 是 SPS 的第一个字节）。
- 视频第一个包开头 `00 00 02 ac`：一个 4 字节的长度，0x2ac = 684，表示后面 684 字节是一个 NAL 单元。这里没有起始码。
- 音频的 extradata 开头 `12 10`：按位拆开是 `00010 0100 0010 000`，依次是对象类型 2（AAC LC）、
  采样率序号 4（44100 Hz）、声道配置 2（双声道）。
- 音频第一个包直接就是压缩数据，前面没有 ADTS 头。

同一份内容放在 MPEG-TS 里：

```console
$ $B/codec_params samples/small_360p25.ts
容器格式：mpegts

流 #0：video
  codec_id      h264，默认解码器 h264
  profile/level High / 30
  bit_rate      0 b/s
  宽高          640x360
  format        yuv420p
  extradata     41 字节： 00 00 00 01 67 64 00 1e ac d9 40 a0 2f f9 70 11 ...
流 #1：audio
  codec_id      aac，默认解码器 aac
  profile/level LC / -99
  bit_rate      82962 b/s
  采样率        44100 Hz
  声道布局      stereo
  format        fltp
  frame_size    1024
  extradata     0 字节

每一路流的第一个包：
  流 #0    8957 字节： 00 00 00 01 09 f0 00 00 00 01 06 05 ff ff a8 dc ...
  流 #1     205 字节： ff f1 50 80 19 bf fc de 02 00 4c 61 76 63 36 33 ...
```

- 视频包以起始码 `00 00 00 01` 开头，是 Annex B 方式。SPS、PPS 就在码流里。
  这里的 extradata 也是起始码开头的，它不是 TS 文件里记的，是 `avformat_find_stream_info()` 读了开头的一段码流之后从里面取出来的。
- 音频包以 `ff f1` 开头，这是 ADTS 头的同步字。包比 MP4 里的大 7 个字节（205 和 198），多的就是这个头；
  头后面的 `de 02 00 4c 61 76 63 ...` 和 MP4 里的包一模一样。音频没有 extradata，用不着。
- TS 不记录码率，视频的 `bit_rate` 是 0，音频的码率是从开头几帧的 ADTS 头估出来的，不准（实际是 121 kb/s）。宽高、像素格式、采样率这些，同样是 `avformat_find_stream_info()` 试着解析码流得到的。

## 第 2 步：官方示例 demux_decode.c

```console
$ $B/demux_decode samples/small_360p25.mp4 out/v_ex.yuv out/a_ex.pcm > out/demux_decode.log
Input #0, mov,mp4,m4a,3gp,3g2,mj2, from 'samples/small_360p25.mp4':
  （和 ffprobe 开头相同的一段，略）
[aac @ 0x55db76b94ec0] Could not update timestamps for discarded samples.
$ grep -c video_frame out/demux_decode.log; grep -c audio_frame out/demux_decode.log
125
216
$ tail -6 out/demux_decode.log
Demuxing succeeded.
Play the output video file with the command:
ffplay -f rawvideo -pixel_format yuv420p -video_size 640x360 out/v_ex.yuv
Warning: the sample format the decoder produced is planar (fltp). This example will output the first channel only.
Play the output audio file with the command:
ffplay -f f32le -ch_layout mono -sample_rate 44100 out/a_ex.pcm
$ ls -l out/v_ex.yuv out/a_ex.pcm
... 882000 ... out/a_ex.pcm
... 43200000 ... out/v_ex.yuv
```

视频 125 帧，每帧 345,600 字节，共 43,200,000 字节。和 ffmpeg 的结果相同：

```console
$ md5sum out/v_ex.yuv
60b1eeb6c8e8b9013fd097033352fe72  out/v_ex.yuv
$ ffmpeg -v error -i samples/small_360p25.mp4 -an -f rawvideo - | md5sum
60b1eeb6c8e8b9013fd097033352fe72  -
```

音频只有左声道。示例只写第一个平面，fltp 的第一个平面是左声道，所以 882,000 字节是 220,500 个采样 × 4 字节：

```console
$ md5sum out/a_ex.pcm
b5ad1e087e4a5f36eb0ab4a5ff78d2ac  out/a_ex.pcm
$ ffmpeg -v error -i samples/small_360p25.mp4 -vn -af "pan=mono|c0=c0" -f f32le - | md5sum
b5ad1e087e4a5f36eb0ab4a5ff78d2ac  -
```

**代码骨架**（`L07_demux_decode/demux_decode.c`，带课程注释）

```text
main()
  avformat_open_input() / avformat_find_stream_info()        L03
  open_codec_context(视频)    open_codec_context(音频)        见下
  av_image_alloc()            一块没有填充的图像缓冲区
  while (av_read_frame(fmt_ctx, pkt) >= 0)
      按 pkt->stream_index 调 decode_packet(视频或音频解码器, pkt)
      av_packet_unref(pkt)
  decode_packet(视频解码器, NULL)   decode_packet(音频解码器, NULL)   冲刷

open_codec_context()
  av_find_best_stream()               选流
  avcodec_find_decoder(codec_id)      按容器记的编码格式找解码器
  avcodec_alloc_context3()
  avcodec_parameters_to_context()     把容器里的参数交给解码器
  avcodec_open2()

decode_packet()                       和 L05、L06 相同的 send/receive
  视频帧 → output_video_frame()       av_image_copy2() 去掉填充后一次写出
  音频帧 → output_audio_frame()       写第一个平面
```

写图像的办法和 L05 不同。L05 的 `decode_yuv` 一行一行地写，每行只写有效的字节。
这里先用 `av_image_alloc(..., 1)` 申请一块对齐为 1、也就是没有填充的缓冲区，每一帧用 `av_image_copy2()` 拷过去，
拷的时候填充被丢掉，然后整块写出。两种办法结果一样，后一种多拷一次内存，代码短一些。

**这个示例的问题**

| 现象 | 复现 | patch |
| --- | --- | --- |
| 输出文件打不开，报了错，退出码是 0 | `$B/demux_decode samples/small_360p25.mp4 /nonexistent/v out/a.pcm; echo $?` | 0025 |
| 解码中途出错，仍然打印 `Demuxing succeeded.`，退出码是 0 | 见 `upstream_patches/README.md` 的 0026 | 0026 |
| 输入是 MPEG-TS 时，打印的音频时间不对（第一帧打印成 2.973 秒，实际是 1.457 秒） | `$B/demux_decode samples/small_360p25.ts out/v_ex.yuv out/a_ex.pcm 2>/dev/null \| grep -m1 audio_frame` | 0027 |
| 双声道只写出左声道；解码器输出 packed 格式时每帧只写出前一半 | 上面的运行结果 | 0028 |
| 磁盘满了也报告成功 | `$B/demux_decode samples/small_360p25.mp4 /dev/full /dev/full > /dev/null; echo $?` | 0029 |
| 解码 AAC 时有一条 `Could not update timestamps` 的警告 | 上面的运行结果 | 0008 |

## 第 3 步：decode_av

`decode_av.c` 是这一讲自己的程序，流程与官方示例相同，改掉了上面的问题：

- 视频和音频各用一个 `StreamDecoder` 结构体保存状态，不用全局变量；
- 音频写出全部声道，planar 格式交错之后再写（L06 的写法）；
- 读包、解码、写文件的每一步都检查错误，出错时退出码是 1；
- 设置了 `pkt_timebase`；
- 输入里只有视频或只有音频也能处理，输出文件名写 `-` 表示这一路只解码、不写文件。

```console
$ $B/decode_av samples/small_360p25.mp4 out/v.yuv out/a.pcm
视频：流 #0，解码器 h264，125 个包，125 帧，第一帧 0.000 秒，最后一帧 4.960 秒
  写出 43200000 字节到 out/v.yuv
  yuv420p 640x360
  ffplay -f rawvideo -pixel_format yuv420p -video_size 640x360 -framerate 25/1 out/v.yuv
音频：流 #1，解码器 aac，217 个包，216 帧，每个声道 220500 个采样，第一帧 0.000 秒，最后一帧 4.992 秒
  写出 1764000 字节到 out/a.pcm
  fltp，44100 Hz，stereo
  ffplay -f f32le -ch_layout stereo -sample_rate 44100 out/a.pcm
```

**核对**

```console
$ md5sum out/v.yuv out/a.pcm
60b1eeb6c8e8b9013fd097033352fe72  out/v.yuv
4d5669c0d8f2c2cd0a60b6b89c5d8627  out/a.pcm
$ ffmpeg -v error -i samples/small_360p25.mp4 -an -f rawvideo - | md5sum
60b1eeb6c8e8b9013fd097033352fe72  -
$ ffmpeg -v error -i samples/small_360p25.mp4 -vn -f f32le - | md5sum
4d5669c0d8f2c2cd0a60b6b89c5d8627  -
```

视频和音频都与 ffmpeg 的结果逐字节相同。其他容器和素材也核对过：

| 输入 | 视频 | 音频 | 与 ffmpeg 相同 |
| --- | --- | --- | --- |
| `samples/small_360p25.mp4` | 125 帧 | 216 帧，220,500 个采样 | 是 |
| `samples/small_360p25.mkv` | 125 帧 | 216 帧，220,500 个采样 | 是 |
| `samples/small_360p25.ts` | 125 帧 | 217 帧，222,208 个采样 | 是 |
| `samples/small_360p25.flv` | 125 帧 | 217 帧，222,208 个采样 | 音频是；视频见练习 4 |
| `samples/demo_1080p30.mp4` | 300 帧 | 469 帧，480,000 个采样 | 是 |
| `samples/audio_44k_stereo.mp2`（L06 的裸流） | 无 | 192 帧，221,184 个采样 | 是 |
| `samples/video_360p25.m1v`（L05 的裸流） | 125 帧 | 无 | 是 |

最后两行说明一件事：裸流也可以用 libavformat 打开，它内部替你用了 parser。
L05、L06 自己读文件、自己调 parser，是为了看清解码器的输入是什么；实际写程序时，不管什么文件都交给 libavformat 读就行。
`.mp2` 的 md5 是 `895666449c90ae840835d4441f040d4b`，和 L06 用官方示例 `decode_audio` 得到的一样。

**包数和帧数**

- 视频 125 个包，125 帧。一个包一帧，一帧不少（冲刷取出了最后几帧）。
- 音频 217 个包，216 帧，220,500 个采样，正好 5.000 秒。L06 把同一路 AAC 取成 ADTS 裸流来解，得到的是 217 帧、222,208 个采样，多了 1,708 个。
  区别在于容器：AAC 编码器在开头多编了一帧（1,024 个采样），结尾为了凑满一帧又补了 684 个，
  MP4 和 MKV 记下了开头和结尾各该丢掉多少，解码器照办；TS、FLV 和 ADTS 裸流不记这个信息，多出来的采样就留在了结果里。
  编码器为什么要多编这一帧，L14 讲音频编码时解释。

**读代码**（`decode_av.c`）

`open_decoder()` 是这一讲的重点，一共五步：

```c
ret = av_find_best_stream(fmt, type, -1, -1, &codec, 0);    /* 选流，同时找到解码器 */
st  = fmt->streams[ret];
sd->dec = avcodec_alloc_context3(codec);                     /* 建上下文 */
ret = avcodec_parameters_to_context(sd->dec, st->codecpar);  /* 容器里的参数交给解码器 */
sd->dec->pkt_timebase = st->time_base;                       /* 包的时间戳用什么时间基 */
ret = avcodec_open2(sd->dec, codec, NULL);                   /* 打开 */
```

`av_find_best_stream()` 的第 5 个参数传了 `&codec`，它在选流的同时把解码器也找出来，找不到解码器的流它不选。
官方示例传的是 `NULL`，选完流再调 `avcodec_find_decoder()`，效果相同，多一步。

主循环：

```c
while ((ret = av_read_frame(fmt, pkt)) >= 0) {
    StreamDecoder *sd = video.st && pkt->stream_index == video.st->index ? &video :
                        audio.st && pkt->stream_index == audio.st->index ? &audio : NULL;
    if (sd)
        ret = decode(sd, pkt, frame);
    av_packet_unref(pkt);          /* 用没用到都要解除引用 */
    if (ret < 0)
        goto end;
}
if (ret != AVERROR_EOF)            /* 读完是 AVERROR_EOF，其他负数是出错 */
    goto end;
```

`decode()` 和 L05、L06 的写法一字不差。`process_frame()` 里视频帧逐行写（L05），音频帧交错写（L06），都是学过的内容。

## 第 4 步：包和帧的先后

第四个参数写 `trace`，程序打印前 40 个事件：每读到一个包打印一行，每解出一帧打印一行（缩进的）。
输出文件名写 `-`，只看过程，不写文件。

```console
$ $B/decode_av samples/small_360p25.mp4 - - trace | head -17
读到视频包  dts  -1024  pts      0 = 0.000 秒   8912 字节  关键帧
读到视频包  dts   -512  pts   1024 = 0.080 秒   4481 字节
读到音频包  dts  -1024  pts  -1024 = -0.023 秒    198 字节  关键帧
读到视频包  dts      0  pts    512 = 0.040 秒   3101 字节
        → 视频帧   0  pts      0 = 0.000 秒  I 帧
读到音频包  dts      0  pts      0 = 0.000 秒     80 字节  关键帧
        → 音频帧   0  pts      0 = 0.000 秒  1024 个采样
读到音频包  dts   1024  pts   1024 = 0.023 秒    131 字节  关键帧
        → 音频帧   1  pts   1024 = 0.023 秒  1024 个采样
读到视频包  dts    512  pts   2048 = 0.160 秒   4284 字节
        → 视频帧   1  pts    512 = 0.040 秒  B 帧
读到音频包  dts   2048  pts   2048 = 0.046 秒     97 字节  关键帧
        → 音频帧   2  pts   2048 = 0.046 秒  1024 个采样
读到音频包  dts   3072  pts   3072 = 0.070 秒    101 字节  关键帧
        → 音频帧   3  pts   3072 = 0.070 秒  1024 个采样
读到视频包  dts   1024  pts   1536 = 0.120 秒   2964 字节
        → 视频帧   2  pts   1024 = 0.080 秒  P 帧
```

从这段输出里能看到四件事：

1. **视频包和音频包交替出现。** 复用器按时间把两路流的包穿插着写进文件（L04），读出来也是穿插的，两个解码器轮流工作。
   两路流的时间基不同（视频 1/12800，音频 1/44100），所以 pts 的数值不能直接比较，要换算成秒。
2. **视频包按 dts 递增，pts 是乱的；视频帧按 pts 递增。** 包的 pts 依次是 0、1024、512、2048、1536，
   帧的 pts 依次是 0、512、1024。解码器把顺序调了回来，程序不用自己排序。
3. **视频帧比包晚出来。** 送进去 3 个包才出第一帧，之后每送一个包出一帧。解码器要先看到后面的参考帧，才能确定前面哪一帧该先输出。
   欠着的两帧在冲刷时取出，所以最后包数和帧数相等。
4. **音频基本上是送一个出一个，除了第一个包。** 第一个音频包的 pts 是 -1024，它是编码器开头多出来的那一帧，
   容器把它标成了要丢弃（L04 里见过它带 `AV_PKT_FLAG_DISCARD`）。解码器收下了它，没有交出帧。
   这就是 217 个包只出 216 帧的原因。

## 第 5 步：不交参数会怎样

第四个参数写 `noparams`，程序跳过 `avcodec_parameters_to_context()`，其他不变。

```console
$ $B/decode_av samples/small_360p25.mp4 - - noparams
[h264 @ 0x55694c276280] No start code is found.
[h264 @ 0x55694c276280] Error splitting the input into NAL units.
视频送包出错：Invalid data found when processing input
```

解码器没有拿到 extradata，不知道这路流是 AVCC 方式，按默认的 Annex B 方式去找起始码，包里没有起始码，第一个包就失败。
这和 L05 里把 MP4 直接交给裸流程序是同一个错误。只有音频的 MP4 也一样：

```console
$ ffmpeg -v error -y -i samples/small_360p25.mp4 -vn -c copy out/audio_only.m4a
$ $B/decode_av out/audio_only.m4a - - noparams
输入里没有视频流
音频送包出错：Invalid data found when processing input
```

换成 MPEG-TS，同样跳过这一步，却能正常解完：

```console
$ $B/decode_av samples/small_360p25.ts - - noparams
视频：流 #0，解码器 h264，125 个包，125 帧，第一帧 1.480 秒，最后一帧 6.440 秒
  yuv420p 640x360
音频：流 #1，解码器 aac，217 个包，217 帧，每个声道 222208 个采样，第一帧 1.457 秒，最后一帧 6.472 秒
  fltp，44100 Hz，stereo
```

TS 里的 H.264 带着起始码和 SPS、PPS，AAC 带着 ADTS 头，解码器从码流里就能得到全部参数，和解裸流一样。

这是一个容易漏掉的错误：用 TS 文件或者裸流测试时一切正常，换成 MP4 就全部失败。所以这一步任何时候都要做，不要因为手头的文件能解就省掉。
（这个 TS 文件的时间戳从 1.4 秒左右开始，不是从 0 开始，L03、L04 都见过。）

## 常见错误

1. **漏掉 `avcodec_parameters_to_context()`。** MP4、MKV、FLV 里的 H.264 和 AAC 第一个包就解码失败，报 `No start code is found` 或 `Invalid data found`。见第 5 步。
2. **`avcodec_parameters_to_context()` 放在 `avcodec_open2()` 之后。** 解码器已经按没有参数的状态初始化完了，再拷没有用。
3. **用 `AVCodecContext.time_base` 换算帧的时间戳。** 帧的 pts 用的是流的时间基，要用 `AVStream.time_base`。官方示例就错在这里（patch 0027）。
4. **忘了 `av_packet_unref()`，或者只在用到这个包的分支里调用。** 每读一个包都要解除引用，包括不关心的流的包，否则内存一直涨。
5. **把所有的包送给同一个解码器。** 一定要看 `stream_index`。把音频包送给视频解码器，解码器会报数据无效。
6. **只冲刷一个解码器。** 每个解码器都要冲刷。漏掉视频的冲刷会少最后几帧。
7. **把 `av_read_frame()` 返回负数一律当成读完。** 读到末尾是 `AVERROR_EOF`，其他负数是出错（文件损坏、网络断开）。不区分的话，读到一半出错的文件会被当成正常处理完。
8. **以为包数等于帧数。** 多数情况下相等，但不保证：开头要丢弃的包不出帧，损坏的包不出帧，有的格式一个包里有多帧。统计帧数要数 `avcodec_receive_frame()` 成功的次数。
9. **直接比较两路流的 pts。** 时间基不同，数值没有可比性。先用各自的时间基换算成秒，或者用 `av_compare_ts()`。

## 练习

1. L05 介绍过 `thread_count`。在 `decode_av.c` 的 `avcodec_open2()` 之前加一句 `sd->dec->thread_count = 0;`（0 表示按 CPU 核数自动决定），
   用 `time $B/decode_av samples/demo_1080p30.mp4 - -` 比较修改前后的耗时。输出的 md5 变了吗？用 `trace` 看第一帧视频在第几行出来。
2. 只解关键帧：打开视频解码器之前加一句 `sd->dec->skip_frame = AVDISCARD_NONKEY;`。两个 MP4 素材各解出几帧？耗时是多少？
   这个办法适合做什么？
3. 做一个有两路音频的文件：
   ```bash
   ffmpeg -v error -y -i samples/small_360p25.mp4 -f lavfi -i "sine=frequency=880:sample_rate=48000:duration=5" \
          -map 0:v -map 0:a -map 1:a -c:v copy -c:a:0 copy -c:a:1 aac -ac:a:1 1 \
          -disposition:a:0 0 -disposition:a:1 default out/two_audio.mkv
   ```
   `decode_av` 解的是哪一路音频？为什么？怎样让它解另一路？
4. `samples/small_360p25.flv` 的视频，`decode_av` 写出 43,200,000 字节，`ffmpeg -i samples/small_360p25.flv -an -f rawvideo` 写出多少？
   多出来的是什么？给 ffmpeg 加上 `-fps_mode passthrough` 再比较。
5. 把 `decode_av.c` 里设置 `pkt_timebase` 的那一句去掉，重新解 `samples/small_360p25.mp4`，输出和 md5 有没有变化？

<details>
<summary>参考答案</summary>

**1.** 4 核的机器上，单线程约 1.5 秒，自动线程数约 0.6 秒。输出的 md5 不变（`d46bcf472549f52793b3448490ac3b06`）：
多线程只改变解码的快慢，不改变结果。`trace` 里第一帧视频从第 5 行推迟到了第 23 行：帧级多线程下，每个线程手里都压着一帧，
解码器要多收几个包才开始出帧。对离线处理这没有影响；对延迟敏感的场合（视频通话、云游戏）要注意，线程数越多，延迟越大。

**2.** `small_360p25.mp4` 解出 5 帧，`demo_1080p30.mp4` 解出 2 帧，正好是各自关键帧的个数
（`ffprobe -v error -select_streams v -show_entries packet=flags -of csv=p=0 文件 | grep -c K`）。
1080p 素材的耗时从约 1.5 秒降到约 0.05 秒。非关键帧的包解码器收下之后直接跳过，不做解码。
适合做视频缩略图、进度条预览图、快速浏览：这些场合不需要每一帧，要的是快。

**3.** 解的是流 #2（48000 Hz 单声道的那一路）：

```text
音频：流 #2，解码器 aac，236 个包，235 帧，每个声道 240000 个采样，第一帧 0.000 秒，最后一帧 4.992 秒
  fltp，48000 Hz，mono
```

`av_find_best_stream()` 优先选带默认标志（disposition 里的 default）的流，命令里把默认标志给了第二路音频。
两路都没有默认标志或者都有时，它再比较码率等。要指定某一路，把第 3 个参数（`wanted_stream_nb`）从 -1 改成流的序号，
例如音频传 1，解出来就是流 #1（44100 Hz 双声道）。播放器里的“切换音轨”做的就是这件事。

**4.** ffmpeg 写出 43,545,600 字节，多了一帧（126 帧）。FLV 素材的第一帧视频在 0.080 秒，不是 0 秒。
ffmpeg 命令行默认按固定帧率输出，发现开头空了一段，就把一帧重复一次来填补。这是 ffmpeg 命令行工具自己的同步逻辑，不是解码器做的。
加上 `-fps_mode passthrough` 让它原样输出，结果的 md5 是 `60b1eeb6c8e8b9013fd097033352fe72`，和 `decode_av` 相同。
用 ffmpeg 命令行核对解码结果时，如果帧数对不上，先想到这个选项。

**5.** 输出文件和 md5 都没有变化，只是多了一条警告：

```text
[aac @ 0x...] Could not update timestamps for discarded samples.
```

解码器照样丢掉了该丢的采样，但它不知道时间基，没法相应地修正帧的时间戳，所以提醒一句。
这个素材的结果恰好不受影响。`pkt_timebase` 在头文件里注明解码时由用户设置，所以每次都设上。官方示例没有设置（patch 0008），所以第 2 步里有这条警告。

</details>

## 下一讲

到这里，从文件到图像和声音的整条路已经走通。L08 回到视频帧本身：I、P、B 帧和 GOP 的结构，
以及解码器附在帧上的额外信息（side data），例子是导出运动矢量的官方示例 `extract_mvs.c`。
