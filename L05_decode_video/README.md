# L05 视频解码

前面四讲处理的都是压缩数据：读出来，看时间戳，换一个容器写回去，从来没有打开过它。
这一讲第一次把压缩数据还原成图像。跟着官方示例 `decode_video.c` 学会 libavcodec 解码的固定写法：
`avcodec_send_packet()` 送包，`avcodec_receive_frame()` 取帧。然后看清楚解码出来的 `AVFrame` 在内存里是怎么放的，
把它正确地写进文件。

为了把解码单独拿出来讲，这一讲不用 libavformat，直接读没有容器的裸流。先读容器再解码是 L07 的内容。

完成标志：`decode_yuv` 写出的 YUV 文件与 ffmpeg 命令行解码的结果逐字节相同；
能说清楚为什么送进去一个包不一定马上出来一帧，以及 `linesize` 和宽度有什么区别。

## 本讲要点

**视频为什么能压缩**

一帧 640×360 的 yuv420p 图像是 345,600 字节，每秒 25 帧就是 8.64 MB。压缩靠三件事：

- **帧内预测、变换、量化**。一帧之内相邻的像素很像。把图像分成小块，每一块先用旁边已经编码的像素预测，
  再把预测剩下的差值做变换（DCT 一类），变成少数几个有意义的系数，然后量化：把系数除以一个步长再取整。
  量化丢掉了人眼不敏感的细节，是整个过程里唯一有损失的一步，码率高低主要由它决定。
- **帧间预测**。相邻的两帧很像。对每一块，到已经编码的帧（参考帧）里找一块最像的，
  只记下位置偏移（运动矢量）和剩下的差值。差值同样做变换和量化。
- **熵编码**。把前面得到的系数、运动矢量等符号做无损压缩，出现得多的用短码。

由此有三种帧：

| 帧类型 | 预测方式 | 特点 |
| --- | --- | --- |
| I 帧 | 只用帧内预测 | 不依赖其他帧，可以独立解码，体积最大 |
| P 帧 | 可以参考前面的帧 | 体积小，参考帧坏了它也跟着坏 |
| B 帧 | 可以同时参考前面和后面的帧 | 体积最小，要等它后面的参考帧先解码（L04 讲的 dts 和 pts 不同就是因为它） |

解码就是把上面的过程倒过来：熵解码，反量化，反变换，加上预测值。
解码 P 帧和 B 帧要用到参考帧，所以**解码器是有状态的**：它内部存着参考帧，包必须按顺序一个不漏地送进去。
这是解码接口设计成“送包”和“取帧”两个函数的原因。

**裸流和封装流**

| | 裸流（elementary stream） | 封装流（L03、L04 读的那些文件） |
| --- | --- | --- |
| 内容 | 只有一路压缩数据，一帧接一帧 | 一路或多路流，加上索引、时间戳、编码参数、元数据 |
| 帧的边界 | 没有记录，要按码流语法自己找 | 容器记下了每个包的位置和大小 |
| 时间戳 | 没有 | 每个包有 pts 和 dts |
| 时长、总帧数、定位 | 不知道，不能定位 | 有 |
| 例子 | `.m1v`、`.h264`、`.mp2` | `.mp4`、`.mkv`、`.ts` |
| 读法 | `fread()` 加 parser | `av_read_frame()` |

**parser**

`av_parser_parse2()` 把一段连续的字节流切成一帧一帧的包。你每次给它一块数据，大小随意；
它在里面找帧的边界，凑够完整的一帧就通过输出参数交出来，没凑够就先攒着。每种编码格式切帧的规则不同，
所以 parser 按编码格式创建（`av_parser_init(codec_id)`）。读容器时用不着它，容器已经把包分好了。

**send/receive：解码的固定写法**（`libavcodec/avcodec.h` 里 “send/receive encoding and decoding API overview” 一节）

| 函数 | 返回值 | 含义 | 接下来做什么 |
| --- | --- | --- | --- |
| `avcodec_send_packet()` | 0 | 解码器收下了这个包 | 去取帧 |
| | `AVERROR(EAGAIN)` | 解码器里有帧没取走，暂时不收新包 | 先取帧，再把同一个包重送一次 |
| | 其他负数 | 出错，例如数据损坏是 `AVERROR_INVALIDDATA` | 按错误处理 |
| `avcodec_receive_frame()` | 0 | 取到一帧 | 用完再取 |
| | `AVERROR(EAGAIN)` | 现在没有帧，需要更多数据 | 送下一个包 |
| | `AVERROR_EOF` | 已经冲刷完，以后不会再有帧 | 结束 |
| | 其他负数 | 出错 | 按错误处理 |

三条规则：

1. 送一个包，可能取到 0 帧、1 帧或多帧。每送一个包，都用循环把帧取到 `EAGAIN` 为止。
2. 帧按显示顺序出来，而且比送进去的包晚。晚多少取决于编码格式、有没有 B 帧、开了几个解码线程。
3. 数据送完之后要冲刷：送一个 `NULL`，再把帧取到 `AVERROR_EOF` 为止。不冲刷就会少最后几帧。

`EAGAIN` 和 `AVERROR_EOF` 是正常的状态，不是错误。

**AVFrame 里的图像**（`libavutil/frame.h`）

| 成员 | 含义 |
| --- | --- |
| `format` | 像素格式，`enum AVPixelFormat` 的值，例如 `AV_PIX_FMT_YUV420P` |
| `width`、`height` | 图像的宽和高，单位是像素 |
| `data[i]` | 第 i 个平面的首地址。yuv420p 有三个平面：`data[0]` 是 Y，`data[1]` 是 U，`data[2]` 是 V |
| `linesize[i]` | 第 i 个平面每一行在内存里占多少字节，**大于或等于**这一行的像素数 |
| `pict_type` | 帧类型（I、P、B） |
| `pts` | 显示时间戳。裸流没有时间戳，这一讲里它是 `AV_NOPTS_VALUE` |

```text
data[0] ─► ┌──────────── linesize[0] ────────────┐
           │ 第 0 行的 width 个像素     │  填充   │
           │ 第 1 行的 width 个像素     │  填充   │
           │ ……                        │         │   共 height 行
           └────────────────────────────┴─────────┘
           第 y 行的首地址是 data[0] + y * linesize[0]
```

- 每行末尾的填充是为了让每一行的首地址对齐，方便用 SIMD 指令加速。填充有多少取决于 CPU 和 FFmpeg 的版本，
  不要去猜，永远用 `linesize`。
- yuv420p 的 U、V 平面横竖都只有 Y 的一半：宽 `width / 2`，高 `height / 2`，各有各的 `linesize`。
- 三个平面是三块分开的内存，不要假设 U 平面紧跟在 Y 平面后面。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L05_decode_video               # 可执行文件在 L05_decode_video/build/ 下
B=L05_decode_video/build
mkdir -p out

# 本讲另外用到两个文件，都从课程素材生成
# 1. H.264 裸流：把 MP4 里的视频流原样取出来，不重新编码
ffmpeg -v error -y -i samples/small_360p25.mp4 -an -c:v copy out/small_360p25.h264
# 2. 一个宽度不是 16 的倍数的 MPEG-1 裸流，第 4 步用
ffmpeg -v error -y -i samples/small_360p25.mp4 -an -vf scale=510:286 \
       -c:v mpeg1video -b:v 1M -f mpeg1video out/odd_510x286.m1v
```

下面的命令都在仓库根目录执行。`samples/video_360p25.m1v` 是 L02 的脚本生成的 MPEG-1 视频裸流，640×360，25 帧每秒，共 125 帧。

## 第 1 步：先量一量压缩了多少

```console
$ ls -l samples/video_360p25.m1v out/small_360p25.h264
-rw-r--r-- 1 user user 481800 ... out/small_360p25.h264
-rw-r--r-- 1 user user 772070 ... samples/video_360p25.m1v
```

同样的 125 帧，不压缩是 345,600 × 125 = 43,200,000 字节。MPEG-1 压到了 1/56，H.264 压到了 1/90。

再看每一帧各占多少。`ffprobe -show_entries frame=...` 会把文件解码一遍，列出每一帧的信息：

```console
$ ffprobe -v error -show_entries frame=pict_type,pkt_size -of csv=p=0 samples/video_360p25.m1v | head -14 | tr '\n' ' '
18801,I 17890,P 14780,P 15028,P 14789,P 12304,P 14708,P 10509,P 8727,P 8187,P 8856,P 6301,P 20351,I 5750,P
```

每 12 帧一个 I 帧，其余是 P 帧。按帧类型统计一下：

```bash
stat_frames() {
    ffprobe -v error -show_entries frame=pict_type,pkt_size -of csv=p=0 "$1" |
        awk -F, '{ n[$2]++; s[$2] += $1 }
                 END { for (t in n) printf "%s 帧 %3d 个，平均 %5.0f 字节\n", t, n[t], s[t] / n[t] }' | sort
}
```

```console
$ stat_frames samples/video_360p25.m1v
I 帧  11 个，平均 16309 字节
P 帧 114 个，平均  5199 字节
$ stat_frames out/small_360p25.h264
B 帧  74 个，平均  3158 字节
I 帧   5 个，平均 10320 字节
P 帧  46 个，平均  4272 字节
```

和一帧原始图像的 345,600 字节比：

| | I 帧 | P 帧 | B 帧 |
| --- | --- | --- | --- |
| MPEG-1 | 1/21 | 1/66 | 没有用 |
| H.264 | 1/33 | 1/81 | 1/109 |

I 帧只靠帧内预测，压到几十分之一。P 帧和 B 帧借助别的帧，又小了好几倍。
H.264 比 MPEG-1 晚了十年，同一类帧压得更小，而且这份素材的 H.264 每 25 帧才有一个 I 帧。
代价也在这里：P 帧和 B 帧离开参考帧就解不出来。第 5 步会亲手验证这一点。

**裸流里缺什么**

ffprobe 也能打开裸流，但是它报出来的很多信息是猜的：

```console
$ ffprobe -v error -show_entries format=format_name,duration,bit_rate:stream=nb_frames -of default=nw=1 samples/video_360p25.m1v
nb_frames=N/A
format_name=mpegvideo
duration=0.058904
bit_rate=104858074
$ ffprobe -v error -select_streams v -show_entries format=format_name,duration,bit_rate:stream=nb_frames -of default=nw=1 samples/small_360p25.mp4
nb_frames=125
format_name=mov,mp4,m4a,3gp,3g2,mj2
duration=5.000000
bit_rate=901113
```

裸流的总帧数不知道，时长报成了 0.06 秒（实际 5 秒），码率报成了 105 Mb/s（实际 1.2 Mb/s）。
这些信息本来是容器负责记录的，裸流里没有。

三个文件开头的 8 个字节：

```console
$ for f in samples/video_360p25.m1v out/small_360p25.h264 samples/small_360p25.mp4; do od -A n -t x1 -N 8 $f; done
 00 00 01 b3 28 01 68 13
 00 00 00 01 06 05 ff ff
 00 00 00 20 66 74 79 70
```

- MPEG-1 裸流以 `00 00 01 b3` 开头。`00 00 01` 是起始码，`b3` 表示序列头，后面紧跟着宽和高：
  `28 0` 是十六进制的 0x280 = 640，`1 68` 是 0x168 = 360。parser 就是靠找起始码来切帧的。
- H.264 裸流以 `00 00 00 01` 开头，也是起始码。
- MP4 以一个 box 开头：长度 0x20，类型 `66 74 79 70` 是 ASCII 的 `ftyp`。里面的 H.264 数据没有起始码，
  每一帧的位置和长度记在索引里。所以**把 MP4 当成裸流直接喂给 parser 是行不通的**，第 2 步有演示。

## 第 2 步：官方示例 decode_video.c

[decode_video.c](decode_video.c) 是 FFmpeg `doc/examples/decode_video.c` 的原样拷贝，只加了中文注释。
它读一个 MPEG-1 视频裸流，把每一帧的亮度平面存成一个 PGM 灰度图。第二个参数是输出文件名的前缀。

```console
$ mkdir -p out/pgm
$ $B/decode_video samples/video_360p25.m1v out/pgm/frame > out/decode_video.log
$ head -2 out/decode_video.log; tail -1 out/decode_video.log
saving frame   1
saving frame   2
saving frame 125
$ ls out/pgm | wc -l
125
$ ls -l out/pgm/frame-1
-rw-r--r-- 1 user user 230415 ... out/pgm/frame-1
$ head -c 15 out/pgm/frame-1
P5
640 360
255
```

PGM 是最简单的图片格式：15 个字节的文本头，后面每个像素一个字节，640 × 360 + 15 = 230,415。
用 `ffplay out/pgm/frame-1` 可以直接看，没有图形界面时先转成 PNG：`ffmpeg -v error -y -i out/pgm/frame-1 out/frame-1.png`。
看到的是黑白的测试图，因为只存了亮度。

**程序的骨架**

```c
pkt    = av_packet_alloc();
codec  = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);   /* 1. 找解码器，裸流只能由调用者指定格式 */
parser = av_parser_init(codec->id);                      /* 2. 创建 parser */
c      = avcodec_alloc_context3(codec);                  /* 3. 分配解码器上下文 */
avcodec_open2(c, codec, NULL);                           /* 4. 打开解码器 */
frame  = av_frame_alloc();

do {
    data_size = fread(inbuf, 1, INBUF_SIZE, f);          /* 5. 读一块文件，4096 字节 */
    eof  = !data_size;
    data = inbuf;
    while (data_size > 0 || eof) {
        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,     /* 6. 交给 parser 切帧 */
                               data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        data      += ret;                                /*    返回值是 parser 用掉的字节数 */
        data_size -= ret;
        if (pkt->size)                                   /*    pkt->size 不为 0：切出了完整的一帧 */
            decode(c, frame, pkt, outfilename);          /* 7. 送包，取帧，存图 */
        else if (eof)
            break;
    }
} while (!eof);

decode(c, frame, NULL, outfilename);                     /* 8. 冲刷 */
```

`decode()` 里就是固定写法：

```c
ret = avcodec_send_packet(dec_ctx, pkt);
if (ret < 0)
    exit(1);
while (ret >= 0) {
    ret = avcodec_receive_frame(dec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return;                                          /* 取完了，回去送下一个包 */
    else if (ret < 0)
        exit(1);
    pgm_save(frame->data[0], frame->linesize[0], frame->width, frame->height, buf);
}
```

几处要留意的地方：

- **输入缓冲区的填充**。`inbuf` 定义成 `INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE` 个字节，多出来的部分清零。
  解码器为了速度会一次读好几个字节，数据损坏时可能读过头，这段填充保证不会越界。
  凡是自己准备缓冲区交给 parser 或解码器，都要这样做。
- **文件读完时再调一次 parser**。`eof` 为真时用 0 字节的输入再调一次 `av_parser_parse2()`，
  parser 才把攒着的最后一帧交出来。它平时要看到下一帧的起始码才知道上一帧结束了，最后一帧后面没有下一帧。
- **pgm_save() 怎么写一行**。循环里是 `fwrite(buf + i * wrap, 1, xsize, f)`：每一行从 `i * linesize` 处开始，
  只写 `width` 个字节。这是正确的写法，第 4 步演示写错的后果。
- **frame 不需要手工释放引用**。`avcodec_receive_frame()` 每次都会先释放 `frame` 里原有的内容。

**把容器文件喂给它会怎样**

```console
$ $B/decode_video samples/small_360p25.mp4 out/pgm/bad
[mpeg1video @ 0x...] Invalid frame dimensions 0x0.
Error sending a packet for decoding
```

`Invalid frame dimensions 0x0` 的意思是：解码器还没有见到序列头，不知道图像多大。以后看到这句话，先检查送进去的是不是它要的数据。

更隐蔽的是容器里装的确实是 MPEG-1 的情况。`.mpg` 是 MPEG-PS 容器：

```console
$ ffmpeg -v error -y -i samples/small_360p25.mp4 -an -c:v mpeg1video -b:v 1M out/in_container.mpg
$ $B/decode_video out/in_container.mpg out/pgm/bad > out/bad.log 2>&1; echo "退出码 $?"
退出码 0
$ grep -c "saving frame" out/bad.log; grep -c "concealing" out/bad.log
124
124
$ rm out/pgm/bad-*
```

程序正常结束，存下了 124 张图，可是每一帧都报了 `concealing ... errors`，图片几乎是一片灰。
MPEG-PS 把视频数据切成小段，每段前面加一个包头。起始码还在，parser 照样切得出帧，
可是包头混在了帧的数据中间，解码器读到一半就乱了。

**这个示例的问题**

| 问题 | patch |
| --- | --- |
| 参数不够时打印用法，退出码却是 0，脚本判断不出程序没有干活。`decode_audio`、`encode_audio`、`encode_video` 也一样 | [0016](../upstream_patches/0016-doc-examples-return-a-failure-status-after-a-usage-e.patch) |
| 输出文件建不起来时每一帧报一次 `Could not open`，照样解完全部数据，退出码是 0；读文件出错时没有任何提示，退出码也是 0 | [0017](../upstream_patches/0017-doc-examples-decode_video-do-not-ignore-I-O-errors.patch) |
| 用法里写的是 `<output file>` 和 “encoded by mpeg1video”，实际需要的是输出前缀和 MPEG-1 **裸流** | [0018](../upstream_patches/0018-doc-examples-decode_video-describe-the-input-and-the.patch) |

另外它遇到任何错误都直接 `exit(1)`，不释放资源。小工具可以这样写，库和长时间运行的程序不行。
本讲另外两个程序用的是 L03 讲过的 `goto end` 写法。

## 第 3 步：decode_trace，看 send 和 receive 的每一次返回

[decode_trace.c](decode_trace.c) 的结构和官方示例相同，区别是解码器由命令行指定，
并且把每一次 `avcodec_send_packet()` 和 `avcodec_receive_frame()` 的返回值打印出来。它不保存图像。

```console
$ $B/decode_trace mpeg1video samples/video_360p25.m1v 1 5
解码器 mpeg1video，线程数 1

包 1     18801 字节 I   send → 0   receive → EAGAIN
包 2     17890 字节 P   send → 0   receive → 帧 1(I)   receive → EAGAIN
包 3     14780 字节 P   send → 0   receive → 帧 2(P)   receive → EAGAIN
包 4     15028 字节 P   send → 0   receive → 帧 3(P)   receive → EAGAIN
包 5     14789 字节 P   send → 0   receive → 帧 4(P)   receive → EAGAIN
  ……（共 125 个包，只打印了前 5 个）
冲刷              send(NULL) → 0   receive → 帧 125(P)   receive → EOF

== 统计 ==
读文件 189 次，共 772070 字节；parser 切出 125 个包
冲刷之前取到 124 帧，冲刷时又取到 1 帧，共 125 帧
```

后两个参数是线程数和最多打印多少个包。一行一行地看：

- **parser**。文件读了 189 次，每次 4096 字节，parser 切出 125 个包。第一个包 18,801 字节，要读 5 次才凑齐。
  读入的块和切出的包在大小上没有任何关系。
- **包 1**：send 返回 0，解码器收下了。紧接着 receive 返回 `EAGAIN`：现在没有帧可取，请继续送数据。这不是错误。
- **包 2**：送进去之后取到了帧 1，再取一次又是 `EAGAIN`。从这里开始每送一个包出一帧，但出来的总是上一个包的帧。
  MPEG-1 允许有 B 帧。解码器事先不知道后面有没有 B 帧，所以每个 I 帧、P 帧都要等下一个 I 帧或 P 帧到了才输出。
- **冲刷**：125 个包都送完了，只取到 124 帧。送一个 `NULL`，最后一帧才出来，再取一次返回 `AVERROR_EOF`。

换成有 B 帧的 H.264：

```console
$ $B/decode_trace h264 out/small_360p25.h264 1 8
解码器 h264，线程数 1

包 1      8951 字节 I   send → 0   receive → EAGAIN
包 2      4481 字节 P   send → 0   receive → EAGAIN
包 3      3101 字节 B   send → 0   receive → 帧 1(I)   receive → EAGAIN
包 4      4284 字节 P   send → 0   receive → 帧 2(B)   receive → EAGAIN
包 5      2964 字节 B   send → 0   receive → 帧 3(P)   receive → EAGAIN
包 6      4212 字节 P   send → 0   receive → 帧 4(B)   receive → EAGAIN
包 7      2888 字节 B   send → 0   receive → 帧 5(P)   receive → EAGAIN
包 8      2948 字节 B   send → 0   receive → 帧 6(B)   receive → EAGAIN
  ……（共 125 个包，只打印了前 8 个）
冲刷              send(NULL) → 0   receive → 帧 124(B)   receive → 帧 125(P)   receive → EOF

== 统计 ==
读文件 118 次，共 481800 字节；parser 切出 125 个包
冲刷之前取到 123 帧，冲刷时又取到 2 帧，共 125 帧
```

- 包的顺序是 I P B P B P B B，这是解码顺序（L04 讲过）。帧出来的顺序是 I B P B P B，这是显示顺序。
  **重排由解码器完成**，取到的帧可以直接按顺序显示。
- 这一次第 3 个包送进去才出第一帧，冲刷时出来 2 帧。解码器要多留一帧来完成重排。

再打开多线程：

```console
$ $B/decode_trace h264 out/small_360p25.h264 4 8
解码器 h264，线程数 4

包 1      8951 字节 I   send → 0   receive → EAGAIN
包 2      4481 字节 P   send → 0   receive → EAGAIN
包 3      3101 字节 B   send → 0   receive → EAGAIN
包 4      4284 字节 P   send → 0   receive → EAGAIN
包 5      2964 字节 B   send → 0   receive → EAGAIN
包 6      4212 字节 P   send → 0   receive → 帧 1(I)   receive → EAGAIN
包 7      2888 字节 B   send → 0   receive → 帧 2(B)   receive → EAGAIN
包 8      2948 字节 B   send → 0   receive → 帧 3(P)   receive → EAGAIN
  ……（共 125 个包，只打印了前 8 个）
冲刷              send(NULL) → 0   receive → 帧 121(B)   receive → 帧 122(P)   receive → 帧 123(B)   receive → 帧 124(B)   receive → 帧 125(P)   receive → EOF

== 统计 ==
读文件 118 次，共 481800 字节；parser 切出 125 个包
冲刷之前取到 120 帧，冲刷时又取到 5 帧，共 125 帧
```

四个线程各解一帧，解码器要先攒够包才开工，所以延迟又多了 3 帧，冲刷时一口气出来 5 帧。

| 解码器 | 线程数 | 送了几个包之后出第一帧 | 冲刷时出来几帧 |
| --- | --- | --- | --- |
| mpeg1video | 1 | 2 | 1 |
| h264 | 1 | 3 | 2 |
| h264 | 4 | 6 | 5 |
| h264 | 0（自动，4 核机器上是 5 个线程） | 7 | 6 |

线程数用 `AVCodecContext.thread_count` 设置，必须在 `avcodec_open2()` 之前。默认是 1；
设成 0 由 FFmpeg 按 CPU 核数决定。ffmpeg 命令行工具默认是自动，所以它解码比这里的单线程快。

多线程换来的是速度。用 1080p 的素材量一下（300 帧）：

```console
$ ffmpeg -v error -y -i samples/demo_1080p30.mp4 -an -c:v copy out/demo_1080p30.h264
$ time $B/decode_trace h264 out/demo_1080p30.h264 1 1 | tail -1
冲刷之前取到 298 帧，冲刷时又取到 2 帧，共 300 帧

real	0m1.237s
$ time $B/decode_trace h264 out/demo_1080p30.h264 4 1 | tail -1
冲刷之前取到 295 帧，冲刷时又取到 5 帧，共 300 帧

real	0m0.438s
```

在 4 核的测试机上，4 个线程比 1 个线程快了将近 3 倍（`time` 输出的 user、sys 两行省略了，具体时间每台机器不同）。
自己写的程序如果不设 `thread_count`，解码就是单线程的。

这张表说明了一件事：**延迟多少帧没有固定的数，不要在程序里写死**。
只要按固定写法来（每送一个包就把帧取到 `EAGAIN`，最后冲刷到 `AVERROR_EOF`），不管延迟多少，一帧都不会丢。

## 第 4 步：decode_yuv，把一帧正确地写进文件

[decode_yuv.c](decode_yuv.c) 把每一帧的三个平面都写出来，得到一个完整的 YUV 文件，并打印第一帧的内存布局。

```console
$ $B/decode_yuv h264 out/small_360p25.h264 out/small_360p25.yuv
第 1 帧：像素格式 yuv420p，图像 640x360，编码尺寸 640x368
  平面 0（Y）  data[0] = 0x7fa695951040  linesize[0] = 640  有效宽度 640  行数 360  每行填充 0 字节
  平面 1（U）  data[1] = 0x559cda425080  linesize[1] = 320  有效宽度 320  行数 180  每行填充 0 字节
  平面 2（V）  data[2] = 0x559cda434340  linesize[2] = 320  有效宽度 320  行数 180  每行填充 0 字节
  一帧写进文件是 345600 字节

写出 125 帧，共 43200000 字节
播放命令：
ffplay -f rawvideo -pixel_format yuv420p -video_size 640x360 -framerate 25/1 out/small_360p25.yuv
```

地址每次运行都不同。从这段输出能读出三件事：

- **三个平面是三块分开的内存**。`data[0]` 和 `data[1]` 的地址相差很远，U 平面并不跟在 Y 平面后面。
- **U、V 平面是 320×180**，横竖都是 Y 的一半。一帧一共 640×360 + 2×320×180 = 345,600 字节。
- **编码尺寸是 640x368，图像是 640x360**。H.264 按 16×16 的宏块编码，360 不是 16 的倍数，
  编码器把高度补到 368，并在码流里注明显示时裁掉下面 8 行。`frame->width` 和 `frame->height` 是裁好之后的尺寸，
  写文件、显示都用它们，不要用 `AVCodecContext` 里的 `coded_width`、`coded_height`。

YUV 文件没有头部，播放器不知道里面是什么，所以播放时要把像素格式、宽高、帧率都告诉它，就是程序最后打印的那条命令。
没有图形界面时取一帧转成 PNG 来看：

```bash
ffmpeg -v error -y -f rawvideo -pixel_format yuv420p -video_size 640x360 -i out/small_360p25.yuv \
       -frames:v 1 out/small_frame1.png
```

验证结果：和 ffmpeg 命令行解码同一个视频的输出比较。

```console
$ md5sum out/small_360p25.yuv
60b1eeb6c8e8b9013fd097033352fe72  out/small_360p25.yuv
$ ffmpeg -v error -i samples/small_360p25.mp4 -f rawvideo -pix_fmt yuv420p - | md5sum
60b1eeb6c8e8b9013fd097033352fe72  -
```

逐字节相同。解码的结果是确定的：同一个码流，任何符合标准的解码器解出来的图像都一样。
这也给了我们一个可靠的测试办法，以后每一讲写出来的解码程序都可以这样核对。

**写一帧的代码**

```c
static int write_frame(const AVFrame *frame, const AVPixFmtDescriptor *desc, Output *out)
{
    for (int p = 0; p < 3; p++) {
        int w, h;

        plane_size(frame, desc, p, &w, &h);            /* 这个平面的宽和高 */
        for (int y = 0; y < h; y++) {
            /* 第 y 行从 data[p] + y * linesize[p] 开始，只写 w 个字节 */
            if (fwrite(frame->data[p] + y * frame->linesize[p], 1, w, out->file) != (size_t)w)
                return AVERROR(EIO);
        }
    }
    return 0;
}
```

色度平面缩小多少由像素格式决定，不要写死成除以 2。`av_pix_fmt_desc_get(frame->format)` 返回这个像素格式的描述，
其中 `log2_chroma_w`、`log2_chroma_h` 是色度在横向、纵向缩小的倍数（以 2 为底的对数）：
yuv420p 是 1 和 1，yuv422p 是 1 和 0，yuv444p 是 0 和 0。`plane_size()` 就是按它算的，所以这段代码对这三种格式都成立。

**linesize 不等于宽度：斜屏**

上面的输出里 `linesize[0]` 正好等于宽度 640。这是巧合：640 恰好是对齐单位的整数倍。换一个宽 510 的视频：

```console
$ $B/decode_yuv mpeg1video out/odd_510x286.m1v out/odd_ok.yuv
第 1 帧：像素格式 yuv420p，图像 510x286，编码尺寸 510x286
  平面 0（Y）  data[0] = 0x7f47411db040  linesize[0] = 512  有效宽度 510  行数 286  每行填充 2 字节
  平面 1（U）  data[1] = 0x55eb2f8f8440  linesize[1] = 256  有效宽度 255  行数 143  每行填充 1 字节
  平面 2（V）  data[2] = 0x55eb2f9015c0  linesize[2] = 256  有效宽度 255  行数 143  每行填充 1 字节
  一帧写进文件是 218790 字节

写出 125 帧，共 27348750 字节
播放命令：
ffplay -f rawvideo -pixel_format yuv420p -video_size 510x286 -framerate 25/1 out/odd_ok.yuv
```

每行 510 个像素，内存里占 512 个字节。初学者最常见的写法是把一个平面当成连续的一整块，一次写出去：

```c
fwrite(frame->data[0], 1, frame->linesize[0] * frame->height, f);   /* 错误 */
```

`decode_yuv` 的第四个参数写 `naive` 就改用这种写法：

```console
$ $B/decode_yuv mpeg1video out/odd_510x286.m1v out/odd_naive.yuv naive | tail -3
写出 125 帧，共 27456000 字节（naive 写法）
播放命令：
ffplay -f rawvideo -pixel_format yuv420p -video_size 510x286 -framerate 25/1 out/odd_naive.yuv
```

两个文件的第一帧，左边是正确的，右边是 naive 写法：

![正确的输出和把 linesize 当成宽度的输出](images/linesize_slant.png)

右边一次出了三个毛病，原因是同一个：

- **画面倾斜**。每一行多写了 2 个字节的填充，播放器按每行 510 个像素去读，
  第 2 行就错开 2 个像素，第 3 行错开 4 个，一行比一行多，竖条变成了斜条。
- **颜色不对**。Y 平面多写了 2 × 286 = 572 个字节，播放器去取 U、V 平面时位置全错了。
- **播放起来画面往上滚**。一帧多出 219,648 − 218,790 = 858 个字节，下一帧的起点就偏了 858 个字节，越往后偏得越多。

一个顺手的检查：文件大小除以一帧的大小应该是整数。27,456,000 ÷ 218,790 = 125.49，不是整数，说明每一帧的字节数就不对。
ffmpeg 读到文件末尾时也会报 `Invalid buffer size, packet size 107250 < expected frame_size 218790`。

这个错误很容易漏到线上：常见的宽度 640、1280、1920 都是 64 的倍数，`linesize` 正好等于宽度，
错误的写法在这些素材上一切正常，换一个 854×480 的视频才出问题。
在这台机器上，宽 854 的视频 `linesize[0]` 是 896，宽 1366 的是 1408。换一台 CPU 不同的机器，数值可能又不一样。

## 第 5 步：弄坏一个 I 帧，看帧间预测的代价

第 1 步说 P 帧离开参考帧就解不出来。现在把第一个 I 帧中间的 100 个字节改成 0，看看影响到哪里为止。
第一个包是 18,801 字节，偏移 9000 处在它中间。

```console
$ cp samples/video_360p25.m1v out/damaged.m1v
$ dd if=/dev/zero of=out/damaged.m1v bs=1 seek=9000 count=100 conv=notrunc status=none
$ $B/decode_yuv mpeg1video samples/video_360p25.m1v out/clean.yuv > /dev/null
$ $B/decode_yuv mpeg1video out/damaged.m1v out/damaged.yuv > /dev/null
[mpeg1video @ 0x...] ac-tex damaged at 35 9
[mpeg1video @ 0x...] Warning MVs not available
[mpeg1video @ 0x...] concealing 200 DC, 200 AC, 200 MV errors in I frame
```

解码器发现第 1 帧有一段数据坏了，用周围的内容把坏掉的 200 个宏块补上（concealing，错误隐藏），然后继续解码。
只报了这一次错，后面的包都是完好的。用 ffmpeg 的 psnr 滤镜逐帧比较损坏前后的解码结果：

```console
$ R="-f rawvideo -pixel_format yuv420p -video_size 640x360"
$ ffmpeg -v error $R -i out/damaged.yuv $R -i out/clean.yuv -lavfi psnr=stats_file=out/psnr.log -f null -
$ cut -d' ' -f1,6 out/psnr.log | sed -n '1p;2p;12p;13p;14p;125p'
n:1 psnr_avg:27.05
n:2 psnr_avg:27.20
n:12 psnr_avg:29.09
n:13 psnr_avg:inf
n:14 psnr_avg:inf
n:125 psnr_avg:inf
$ grep -c "psnr_avg:inf" out/psnr.log
113
```

PSNR 是两幅图像的相似程度，`inf` 表示完全相同。损坏的只有第 1 帧的数据，可是第 1 到第 12 帧的图像都和原来不一样；
从第 13 帧起完全恢复，一共 113 帧。下图是损坏后的第 1、12、13 帧：

![损坏后的第 1、12、13 帧](images/damaged_frames.png)

第 2 到第 12 帧是 P 帧，它们的数据没有坏，但是它们一个接一个地以前一帧为参考，把第 1 帧的损伤一路带了下去。
第 13 帧是下一个 I 帧，不参考任何帧，画面从这里恢复正常。

这就是直播里花屏的来历：丢了一点数据，画面要坏到下一个 I 帧才好。I 帧隔得越远，压缩率越高，出错之后坏得也越久。
L04 说过不重新编码的裁剪只能从关键帧开始，也是同一个原因。

## 常见错误

1. **把 `EAGAIN` 当成错误**。写成 `if (ret < 0) 报错退出`，第一个包送进去就退出了。
   `AVERROR(EAGAIN)` 和 `AVERROR_EOF` 要先单独判断，剩下的负数才是错误。
2. **忘了冲刷**。输出比输入少最后几帧，少几帧还随编码格式和线程数变化，测试时不容易发现。
   数据送完之后一定要送一个 `NULL`，再取到 `AVERROR_EOF`。
3. **送一个包只取一次帧**。大多数时候碰巧是对的，遇到一个包出多帧的情况，解码器里的帧就会越积越多，
   最后 `avcodec_send_packet()` 返回 `EAGAIN`，程序报错。取帧必须用循环。
4. **把 `linesize` 当成宽度，或者假设三个平面连在一起**。在 640、1280、1920 这些宽度上测不出来，换一个宽度就斜屏。
   永远一行一行地处理：第 y 行的首地址是 `data[p] + y * linesize[p]`，有效数据只有这个平面的宽度那么多。
5. **把容器文件当成裸流喂给 parser**。MP4 会直接报 `Invalid frame dimensions 0x0` 之类的错；
   MPEG-PS、TS 更糟，能解出满是错误的画面。读容器要用 libavformat（L07）。
6. **输入缓冲区没有留填充**。自己准备的缓冲区末尾要多留 `AV_INPUT_BUFFER_PADDING_SIZE` 个字节并清零，
   否则遇到损坏的数据可能越界读。
7. **用 `coded_width`、`coded_height` 当图像尺寸**。它们是编码尺寸，可能比图像大。图像尺寸看 `frame->width`、`frame->height`。
8. **把 `av_err2str()` 的结果作为函数返回值**。它是一个宏，字符串放在当前函数的栈上，函数返回之后就失效了。
   要把错误信息带出函数，自己准备缓冲区再调用 `av_strerror()`，`decode_trace.c` 里的 `ret_name()` 是这样写的。

## 练习

1. 把 `decode_yuv.c` 里冲刷的那几行注释掉，重新编译，分别解码 `samples/video_360p25.m1v` 和 `out/small_360p25.h264`。
   各写出多少帧？和第 3 步的表对得上吗？
2. 修改 `decode_trace.c`：送包之后不取帧，只送不取。第几个包的 `avcodec_send_packet()` 开始返回 `EAGAIN`？
   MPEG-1 和 H.264 各是第几个？
3. 把 `decode_yuv.c` 里的 `INBUF_SIZE` 改成 1，再改成 1048576，输出文件的 md5 变不变？
   对 `decode_trace.c` 做同样的修改，“读文件多少次”和“切出多少个包”各是多少？
4. 官方示例只保存了亮度。修改 `decode_video.c`，把每一帧的 U 平面和 V 平面也各存成一个 PGM 文件。
   这两个文件应该是多少字节？打开看看，U、V 平面的图像是什么样子？
5. libavutil 提供了现成的函数把一帧拷进一块连续的内存：`av_image_get_buffer_size()` 和 `av_image_copy_to_buffer()`
   （`libavutil/imgutils.h`）。用它们重写 `decode_yuv.c` 的 `write_frame()`，
   用 `out/odd_510x286.m1v` 验证输出与原来的写法逐字节相同。最后一个参数 `align` 应该传几？

<details>
<summary>参考答案</summary>

**练习 1**

MPEG-1 写出 124 帧（42,854,400 字节），H.264 写出 123 帧（42,508,800 字节），
分别少了 1 帧和 2 帧，就是第 3 步的表里“冲刷时出来几帧”那一列。
`decode_yuv` 没有设置线程数，用的是默认的 1 个线程。如果开 4 个线程，H.264 会少 5 帧。

**练习 2**

把 `receive_frames()` 那次调用去掉（换成打印一个换行）：

```console
$ ./decode_trace_sendonly mpeg1video samples/video_360p25.m1v 1 6
解码器 mpeg1video，线程数 1

包 1     18801 字节 I   send → 0
包 2     17890 字节 P   send → 0
包 3     14780 字节 P   send → 0
包 4     15028 字节 P   send → EAGAIN
送包出错：Resource temporarily unavailable
```

MPEG-1 是第 4 个包，H.264 是第 5 个包。`Resource temporarily unavailable` 就是系统对 `EAGAIN` 的文字说明。

解码器内部只给输出留了一帧的位置，另外能暂存一个还没解码的包。以 MPEG-1 为例：
包 1 解码后没有输出；包 2 解码后输出帧 1，放在那个位置上没人取；包 3 只能先存着；
包 4 没有地方放了，返回 `EAGAIN`。H.264 多一帧的重排延迟，所以晚一个包。
返回 `EAGAIN` 时这个包没有被收下，取走一帧之后要把同一个包重新送一次。

**练习 3**

md5 不变，都是 `7fd4c9f710af2404fbdcec6fa39bb2aa`（输入是 `samples/video_360p25.m1v` 时）。

| `INBUF_SIZE` | 读文件次数 | 切出的包 |
| --- | --- | --- |
| 1 | 772,070 | 125 |
| 4096 | 189 | 125 |
| 1048576 | 1 | 125 |

parser 切出的包只取决于码流本身，与每次喂给它多少字节无关。一次喂 1 个字节它也能凑出完整的帧，
一次把整个文件给它，它就在内层循环里一帧一帧地交出来。这正是内层要用 `while` 循环的原因。

**练习 4**

在 `decode()` 里 `pgm_save()` 那一句后面再加两次调用，传 `frame->data[1]`、`frame->linesize[1]` 和减半的宽高：

```c
snprintf(buf, sizeof(buf), "%s-%"PRId64"-u", filename, dec_ctx->frame_num);
pgm_save(frame->data[1], frame->linesize[1], frame->width / 2, frame->height / 2, buf);
snprintf(buf, sizeof(buf), "%s-%"PRId64"-v", filename, dec_ctx->frame_num);
pgm_save(frame->data[2], frame->linesize[2], frame->width / 2, frame->height / 2, buf);
```

每个文件 320 × 180 + 15 = 57,615 字节，一共得到 375 个文件。
U、V 平面看起来也是一幅灰度图，能认出原图的轮廓，但是明暗和亮度图完全不同：
它们记录的是颜色偏蓝、偏红的程度，没有颜色的地方是中间的灰色（128）。
宽高直接除以 2 只对 yuv420p 成立，通用的写法见 `decode_yuv.c` 的 `plane_size()`。

**练习 5**

```c
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>

static int write_frame(const AVFrame *frame, const AVPixFmtDescriptor *desc, Output *out)
{
    /* 最后一个参数是对齐：1 表示每一行紧挨着上一行，没有填充 */
    int size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
    uint8_t *buf;
    int ret;

    (void)desc;
    if (size < 0)
        return size;
    buf = av_malloc(size);
    if (!buf)
        return AVERROR(ENOMEM);

    ret = av_image_copy_to_buffer(buf, size, (const uint8_t * const *)frame->data, frame->linesize,
                                  frame->format, frame->width, frame->height, 1);
    if (ret >= 0 && fwrite(buf, 1, size, out->file) != (size_t)size)
        ret = AVERROR(EIO);
    if (ret >= 0)
        out->nb_bytes += size;
    av_free(buf);
    return ret < 0 ? ret : 0;
}
```

`align` 传 1。输出 27,348,750 字节，md5 与 `out/odd_ok.yuv` 相同。
这个函数对所有像素格式都成立，包括 `decode_yuv` 不支持的那些，实际工程里优先用它。
每一帧都分配、释放一次内存是为了让答案短一些，正式的写法是分配一次、反复使用。

</details>
