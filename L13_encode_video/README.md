# L13 视频编码

前 12 讲走的是“读进来”的方向：解复用、解码、处理。从这一讲开始走另一半：把原始画面**编码**成压缩码流。
编码的调用方式和解码是对称的，新东西在参数上：解码时一切参数都写在码流里，编码时每一个参数都要由我们决定。

完成标志：`encode_yuv` 用 libx264 编出的码流与 `ffmpeg` 命令行的结果逐字节相同；
能说出码率、CRF、关键帧间隔、预设各自在换什么；会用选项字典给编码器传参数，并能发现写错的选项。

## 本讲要点

**编码和解码是对称的**

| | 解码（L05） | 编码 |
| --- | --- | --- |
| 找 | `avcodec_find_decoder()` | `avcodec_find_encoder_by_name()` |
| 送 | `avcodec_send_packet()` | `avcodec_send_frame()` |
| 取 | `avcodec_receive_frame()` | `avcodec_receive_packet()` |
| 结束 | 送空包，取到 `AVERROR_EOF` | 送 `NULL` 帧，取到 `AVERROR_EOF` |
| 参数从哪来 | 码流、容器 | **程序给** |

送一帧不一定马上得到一个包。编码器要先看到后面的帧，才能决定前面的帧怎么编，所以开头连续几次取不到包（`AVERROR(EAGAIN)`），
结束时冲刷才能把留在里面的帧全部拿出来。

**必须给的参数**

```c
enc->width     = 640;
enc->height    = 360;
enc->pix_fmt   = AV_PIX_FMT_YUV420P;     /* 必须是这个编码器支持的格式 */
enc->time_base = (AVRational){ 1, 25 };  /* 帧的 pts 的单位 */
```

每一帧的 `pts` 以 `time_base` 为单位，必须递增。时间基取帧率的倒数时，第 n 帧的 pts 就是 n。

**决定大小和质量的参数**

| 选项 | 字段 | 意思 | 换的是什么 |
| --- | --- | --- | --- |
| `b` | `bit_rate` | 目标码率（比特每秒）。文件大小 ≈ 码率 × 时长 ÷ 8 | 大小固定，质量随画面内容浮动 |
| `crf` | libx264 私有 | 恒定质量因子，0 到 51，越小质量越高，默认 23 | 质量固定，大小随画面内容浮动 |
| `g` | `gop_size` | 关键帧间隔：最多隔多少帧出一个 I 帧 | 间隔短：能更快地从错误中恢复、定位更细，文件更大 |
| `bf` | `max_b_frames` | 两个非 B 帧之间最多放几个 B 帧 | B 帧省码率，但带来重排延迟（dts 和 pts 不同） |
| `preset` | libx264 私有 | 编码速度档位，`ultrafast` 到 `veryslow` | 用编码时间换压缩率 |

码率和 CRF 是两种码率控制方式，二选一：要控制文件大小或带宽（直播、限定大小的上传）用码率；要控制质量（存档、点播的源文件）用 CRF。

**通用选项和私有选项，选项字典**

- 通用选项是 `AVCodecContext` 的字段，所有编码器共用：`b`、`g`、`bf`、`colorspace`……
- 私有选项属于某一个编码器：libx264 的 `preset`、`crf`、`tune`……mpeg2video 没有它们。

两种选项都可以放进一个字典，在打开编码器时一起传：

```c
AVDictionary *opts = NULL;
av_dict_set(&opts, "b", "500k", 0);
av_dict_set(&opts, "preset", "fast", 0);
ret = avcodec_open2(enc, codec, &opts);
/* 返回后，opts 里剩下的就是编码器不认识的选项 */
```

名字和取值的写法与 ffmpeg 命令行相同（`-b:v 500k` 对应 `b=500k`）。
`avcodec_open2()` 会把认出来的项从字典里删掉，所以**打开之后检查字典是否为空**，就能发现写错的名字。
L08 里给解码器传 `flags2=+export_mvs` 用的是同一个机制。

**裸码流**

这一讲把包的数据直接写进文件，没有容器。H.264（Annex B）、MPEG-1/2 的裸码流播放器能直接认；
但裸码流里没有时间戳，也不是所有格式都能这样存。把包装进 MP4 是 L15 的内容。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L13_encode_video               # 可执行文件在 L13_encode_video/build/ 下
B=L13_encode_video/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `encode_video` | 官方示例（加了课程注释）：把程序生成的 25 帧画面编码成裸码流 |
| `encode_yuv` | 把 Y4M 文件编码成裸码流，编码参数用“选项=值”从命令行给 |
| `encoder_info` | 查看一个视频编码器接受的像素格式和它的私有选项 |

准备一份未压缩的输入。Y4M 是 L12 用过的格式，文件头里带着宽、高和帧率：

```bash
ffmpeg -v error -y -i samples/small_360p25.mp4 -pix_fmt yuv420p out/src.y4m
head -1 out/src.y4m
ls -l out/src.y4m | awk '{print $5}'
```

```text
YUV4MPEG2 W640 H360 F25:1 Ip A1:1 C420mpeg2 XYSCSS=420MPEG2
43200810
```

5 秒、640x360、每秒 25 帧的未压缩视频是 43 MB。（L12 的 `filter_video` 写出的 Y4M 也可以直接当输入。）

## 第 1 步：官方示例 encode_video.c

```bash
$B/encode_video out/ev.m1v mpeg1video 2>/dev/null | sed -n 1,9p
```

```text
Send frame   0
Send frame   1
Write packet   0 (size= 6731)
Send frame   2
Write packet   2 (size= 3727)
Send frame   3
Write packet   1 (size= 1682)
Send frame   4
Write packet   4 (size= 2744)
```

示例自己算出 25 帧 352x288 的渐变画面，交给第二个参数指定的编码器。两件事值得看：

- 送了第 0 帧，没有包出来；送了第 1 帧，才拿到第 0 帧的包。编码器留了一帧。
- 包的顺序是 0、2、1、4、3。示例设了 `max_b_frames = 1`，第 1 帧是 B 帧，它要参考第 2 帧，所以第 2 帧先编、先输出。
  这就是 L08 讲的解码顺序和显示顺序，现在是从编码器这一头看到的。

最后三行：

```bash
$B/encode_video out/ev.m1v mpeg1video 2>/dev/null | tail -3
```

```text
Send frame  24
Write packet  24 (size= 3191)
Write packet  23 (size= 1955)
```

第 23 帧的包是冲刷时才出来的。用 ffprobe 看结果：

```bash
ffprobe -v error -count_frames -show_entries stream=codec_name,width,height,nb_read_frames -of csv=p=0 out/ev.m1v
ffprobe -v error -show_entries frame=pict_type -of csv=p=0 out/ev.m1v | tr -d '\n'; echo
```

```text
mpeg1video,352,288,25
IBPBPBPBPBIBPBPBPBPBIBPBP
```

25 帧，每 10 帧一个 I 帧（`gop_size = 10`），I、P 之间夹一个 B 帧。有图形界面时可以 `ffplay out/ev.m1v` 看一下。

换成 libx264：

```bash
$B/encode_video out/ev.264 libx264 2>/dev/null | grep -n Write | head -1
```

在作者的机器上输出是 `20:Write packet   0 (size= 2069)`：送了 19 帧之后才拿到第一个包。
libx264 为了做码率控制要预读很多帧，留的帧比 mpeg1video 多得多（具体数字和线程数有关，你的机器上可能不同）。
不冲刷的话，这 25 帧里的大部分都拿不到。

读代码时找 `课程注释`。这个示例有 4 处问题，已做成 patch（`upstream_patches/` 的 0016、0053 到 0055）：
参数不够时退出码是 0；不检查写文件的结果；设置 `preset` 失败时没有提示；一处注释过时。

## 第 2 步：encode_yuv，编码真实的画面

```bash
$B/encode_yuv out/src.y4m out/a.264 libx264 2>/dev/null | sed -n 1,12p
```

```text
编码器 libx264：640x360，yuv420p，帧率 25/1，时间基 1/25
目标码率未指定，关键帧间隔未指定，重排延迟 2 帧
色彩矩阵 unknown，色彩范围 unknown
包   0  pts    0  dts   -2    8947 字节  I  关键帧
包   1  pts    4  dts   -1    5027 字节  P
包   2  pts    2  dts    0    3379 字节  B
包   3  pts    1  dts    1    2988 字节  B
包   4  pts    3  dts    2    2860 字节  B
包   5  pts    7  dts    3    4270 字节  P
包   6  pts    5  dts    4    2820 字节  B
包   7  pts    6  dts    5    2936 字节  B
包   8  pts   11  dts    6    4485 字节  P
```

（`2>/dev/null` 丢掉了 libx264 自己打印的统计信息，想看可以去掉。）

每个包一行：

- **pts 和 dts。** 包是按 dts 的顺序出来的（-2、-1、0、1……），pts 是 0、4、2、1、3……和 L04 里用 ffprobe 看到的 MP4 里的包一样。
  dts 从 -2 开始：有 B 帧时解码要比显示提前，“重排延迟 2 帧”，所以编码器把 dts 整体提前了 2。
- **大小。** I 帧 8947 字节，P 帧四五千，B 帧三千左右。未压缩的一帧是 640 × 360 × 1.5 = 345,600 字节。
- **帧类型。** 编码器在包上附带了一份附加数据（side data），里面有帧类型，程序把它打印了出来。

结尾：

```bash
$B/encode_yuv out/src.y4m out/a.264 libx264 2>/dev/null | tail -3
```

```text
送进 125 帧，写出 125 个包，其中关键帧 1 个，冲刷时写出 41 个
共 445697 字节，时长 5.00 秒，平均码率 713 kb/s
写出 out/a.264
```

43 MB 压成了 445,697 字节，约 97 分之一。125 个包里有 41 个是冲刷时才出来的。

验证：用 ffprobe 检查，再和 ffmpeg 命令行（同样用单线程）编出的结果比较。

```bash
ffprobe -v error -count_frames -show_entries stream=codec_name,profile,width,height,nb_read_frames -of csv=p=0 out/a.264
ffmpeg -v error -y -threads 1 -i out/src.y4m -c:v libx264 -threads 1 -f h264 out/cli.264
$B/encode_yuv out/src.y4m out/a_sar.264 libx264 aspect=1 > /dev/null 2>&1
cmp out/a_sar.264 out/cli.264 && echo 两个文件相同
```

```text
h264,High,640,360,125
两个文件相同
```

比较时多加了一个选项 `aspect=1`。ffmpeg 命令行读到了 Y4M 文件头里的像素宽高比（`A1:1`），把它交给了编码器；
`encode_yuv` 没有读这一项，所以用通用选项 `aspect` 补上。补上之后，两个程序编出的码流逐字节相同。
不加这个选项时两个文件差 1 个字节（码流开头的参数集里少了宽高比这一项），解码出来的画面完全相同。

`encode_yuv.c` 默认用单线程编码（`enc->thread_count = 1`）。多线程时结果的大小和留帧的数量会随线程数略有变化，
用单线程，你的机器上的数字才会和这里的一样。加 `threads=0` 可以让编码器用全部的核。

## 第 3 步：码率和 CRF

不给任何选项时 libx264 用 CRF 23。换几个值，同时用 ffmpeg 的 psnr 滤镜量一下和原始画面的差距
（PSNR 的单位是 dB，越大越接近原画，一般 40 以上很难看出差别，30 以下明显变差）：

```bash
for q in 18 23 28 35; do
    $B/encode_yuv out/src.y4m out/q$q.264 libx264 crf=$q 2>/dev/null | grep '^共'
    ffmpeg -i out/q$q.264 -i out/src.y4m -lavfi psnr -f null - 2>&1 | grep -o 'average:[0-9.]*'
done
```

```text
共 625828 字节，时长 5.00 秒，平均码率 1001 kb/s
average:49.259377
共 445697 字节，时长 5.00 秒，平均码率 713 kb/s
average:45.439790
共 249723 字节，时长 5.00 秒，平均码率 400 kb/s
average:39.892732
共 131105 字节，时长 5.00 秒，平均码率 210 kb/s
average:35.624015
```

| CRF | 大小（字节） | 平均码率 | PSNR |
| --- | --- | --- | --- |
| 18 | 625,828 | 1001 kb/s | 49.3 dB |
| 23（默认） | 445,697 | 713 kb/s | 45.4 dB |
| 28 | 249,723 | 400 kb/s | 39.9 dB |
| 35 | 131,105 | 210 kb/s | 35.6 dB |

CRF 每加 5 到 6，大小大约减半。下图左边是 CRF 23，右边是 CRF 45（69,521 字节）的同一帧的左上角，放大了两倍：

![CRF 23 和 CRF 45 的同一帧](images/crf.png)

用 CRF 时不知道文件会有多大。要控制大小，改用码率：

```bash
$B/encode_yuv out/src.y4m out/b500.264 libx264 b=500k 2>/dev/null | sed -n '2p;/^共/p'
```

```text
目标码率 500000 b/s，关键帧间隔未指定，重排延迟 2 帧
共 307124 字节，时长 5.00 秒，平均码率 491 kb/s
```

目标 500 kb/s，实际 491 kb/s。500,000 × 5 ÷ 8 = 312,500 字节，实际 307,124。
编码器是边编边调整的，片段越短偏差越大。

## 第 4 步：关键帧间隔和 B 帧

libx264 默认的关键帧间隔是 250 帧，5 秒的素材里只有开头 1 个关键帧。改小：

```bash
for g in 1 12 25 250; do
    $B/encode_yuv out/src.y4m out/g$g.264 libx264 g=$g 2>/dev/null | grep '^送进\|^共'
done
```

| `g` | 关键帧个数 | 大小（字节） |
| --- | --- | --- |
| 1 | 125 | 1,107,620 |
| 12 | 11 | 510,405 |
| 25 | 5 | 470,471 |
| 250（默认） | 1 | 445,697 |

（表里是上面命令输出的整理。）质量因子相同，关键帧越多文件越大：`g=1` 每一帧都是 I 帧，是默认的 2.5 倍。

那为什么不把间隔设得很大？看一个实验。编两个文件，间隔分别是 25 和 250，在同一个位置各弄坏 300 个字节，然后数有多少帧受到影响
（为了让每一帧都被后面的帧参考，这里用 `bf=0` 关掉了 B 帧）：

```bash
for g in 25 250; do
    $B/encode_yuv out/src.y4m out/k$g.264 libx264 g=$g bf=0 > /dev/null 2>&1
    cp out/k$g.264 out/dmg$g.264
    head -c 300 /dev/zero | dd of=out/dmg$g.264 bs=1 seek=140000 conv=notrunc status=none
    ffmpeg -v error -i out/dmg$g.264 -i out/k$g.264 -lavfi psnr=stats_file=out/psnr$g.txt -f null - 2>/dev/null
    echo "g=$g：$(stat -c %s out/k$g.264) 字节，和没损坏时不一样的帧有 $(grep -vc psnr_avg:inf out/psnr$g.txt) 个"
done
```

```text
g=25：471565 字节，和没损坏时不一样的帧有 13 个
g=250：445447 字节，和没损坏时不一样的帧有 86 个
```

```bash
grep -v psnr_avg:inf out/psnr25.txt  | sed -n '1p;$p' | cut -d' ' -f1
grep -v psnr_avg:inf out/psnr250.txt | sed -n '1p;$p' | cut -d' ' -f1
```

```text
n:38
n:50
n:40
n:125
```

间隔 25 时，错误从第 38 帧持续到第 50 帧，第 51 帧是下一个关键帧，画面恢复正常。
间隔 250 时，后面再没有关键帧，错误一直延续到最后一帧。代价是文件大了 6%（471,565 对 445,447）。

关键帧间隔换的是三样东西：

- **出错后的恢复时间。** 丢了数据，最坏要等一个间隔才恢复。
- **定位的粒度。** L04 讲过，定位只能落在关键帧上；不重新编码的剪切也只能在关键帧处下刀。
- **加入的等待时间。** 直播的观众中途进来，要等到下一个关键帧才有画面。

所以存档的文件间隔可以长（几秒到十秒），直播常用 1 到 2 秒。

再看 B 帧。`bf=0` 关掉之后：

```bash
$B/encode_yuv out/src.y4m out/bf0.264 libx264 bf=0 2>/dev/null | sed -n '2p;4,7p'
```

```text
目标码率未指定，关键帧间隔未指定，重排延迟 0 帧
包   0  pts    0  dts    0    8715 字节  I  关键帧
包   1  pts    1  dts    1    3932 字节  P
包   2  pts    2  dts    2    3765 字节  P
包   3  pts    3  dts    3    3619 字节  P
```

没有 B 帧，dts 和 pts 相同，重排延迟为 0。对延迟敏感的场合（视频通话）通常不用 B 帧。

别的编码器用法一样，选项也是这几个：

```bash
$B/encode_yuv out/src.y4m out/d.m2v mpeg2video b=2M g=12 bf=2 2>/dev/null | sed -n '2p;4,7p;/^送进/,/^共/p'
```

```text
目标码率 2000000 b/s，关键帧间隔 12，重排延迟 1 帧
包   0  pts    0  dts   -1   19187 字节  I  关键帧
包   1  pts    3  dts    0   20609 字节  P
包   2  pts    1  dts    1   10196 字节  B
包   3  pts    2  dts    2    9956 字节  B
送进 125 帧，写出 125 个包，其中关键帧 11 个，冲刷时写出 2 个
共 1337502 字节，时长 5.00 秒，平均码率 2140 kb/s
```

MPEG-2 只在冲刷时留了 2 个包，libx264 留了 41 个。留多少帧是编码器自己的事，程序只要总是冲刷就不会错。

## 第 5 步：预设

`preset` 是 libx264 的速度档位，CRF 不变，换三个档位并计时：

```bash
for p in ultrafast medium veryslow; do
    echo "== $p"
    time $B/encode_yuv out/src.y4m out/p_$p.264 libx264 preset=$p 2>/dev/null | grep '^共'
done
```

作者机器上的结果（时间取 `real` 一行，每台机器不同；大小是确定的）：

| preset | 大小（字节） | 编码时间 |
| --- | --- | --- |
| ultrafast | 1,080,028 | 0.12 秒 |
| medium（默认） | 445,697 | 0.87 秒 |
| veryslow | 380,963 | 4.1 秒 |

从 ultrafast 到 medium，时间是 7 倍，文件小了一多半。从 medium 到 veryslow，时间又是近 5 倍，只小了 15%。
实时编码（直播、录屏）选快的档位，离线转码选慢的档位，默认的 medium 是一个合理的起点。

多线程能把时间拿回来一部分：

```bash
time $B/encode_yuv out/src.y4m out/pt.264 libx264 preset=veryslow threads=0 2>/dev/null | grep '^共'
```

作者的机器（4 核）上是 1.5 秒，大小 392,713 字节，比单线程的大 3%。这就是程序默认用单线程来保证数字可重复的原因。

## 第 6 步：选项写错了会怎样

把 `preset` 拼错：

```bash
$B/encode_yuv out/src.y4m out/x.264 libx264 perset=fast 2>&1 | grep -v '^\[libx264'; echo "退出码 ${PIPESTATUS[0]}"
```

```text
编码器 libx264 不认识选项 perset（值 fast）
退出码 1
```

`avcodec_open2()` 本身是成功的：它不认识 `perset`，就把这一项留在字典里不管。是 `encode_yuv` 在打开之后检查了字典：

```c
ret = avcodec_open2(enc, codec, &opts);
...
if (av_dict_count(opts)) {
    while ((entry = av_dict_iterate(opts, entry)))
        fprintf(stderr, "编码器 %s 不认识选项 %s（值 %s）\n", codec_name, entry->key, entry->value);
    ret = AVERROR_OPTION_NOT_FOUND;
    goto end;
}
```

不做这个检查，写错的选项会被悄悄忽略，程序用默认值编完，报告成功。官方示例直接往 `priv_data` 上设 `preset` 又不看返回值，是同一类问题（patch 0054）。

名字对、值不对时，`avcodec_open2()` 会失败：

```bash
$B/encode_yuv out/src.y4m out/x.264 libx264 preset=quick 2>&1 | grep -v 'using cpu'
```

```text
x264 [error]: invalid preset 'quick'
[libx264 @ 0x564c19b8b600] Error setting preset/tune quick/(null).
[libx264 @ 0x564c19b8b600] Possible presets: ultrafast superfast veryfast faster fast medium slow slower veryslow placebo
[libx264 @ 0x564c19b8b600] Possible tunes: film animation grain stillimage psnr ssim fastdecode zerolatency
打不开编码器 libx264：Invalid argument
```

（方括号里的地址每次运行不同。）

私有选项换一个编码器就不认识了：

```bash
$B/encode_yuv out/src.y4m out/x.m2v mpeg2video crf=23
```

```text
编码器 mpeg2video 不认识选项 crf（值 23）
```

一个编码器有哪些私有选项，用 `encoder_info` 查（或者 `ffmpeg -h encoder=libx264`）：

```bash
$B/encoder_info libx264 | sed -n '1,9p;$p'
```

```text
编码器 libx264：libx264 H.264 / AVC / MPEG-4 AVC / MPEG-4 part 10
编码格式：h264
内部会留帧（结束时必须冲刷）：是
硬件编码器：否
接受的像素格式：yuv420p yuvj420p yuv422p yuvj422p yuv444p yuvj444p nv12 nv16 nv21 yuv420p10le yuv422p10le yuv444p10le nv20le gray gray10le
私有选项：
  preset             Set the encoding preset (cf. x264 --fullhelp)
  tune               Tune the encoding params (cf. x264 --fullhelp)
  profile            Set profile restrictions (cf. x264 --fullhelp)
共 48 个
```

## 第 7 步：像素格式和色彩属性

编码器只接受它声明的像素格式。给 png 编码器送 yuv420p：

```bash
$B/encode_yuv out/src.y4m out/x.png png 2>&1 | sed -n '1p;$p'
$B/encoder_info png | sed -n 5p
```

```text
[png @ 0x5634ecda5540] Specified pixel format yuv420p is not supported by the png encoder.
打不开编码器 png：Invalid argument
接受的像素格式：rgb24 rgba rgb48be rgba64be pal8 gray ya8 gray16be ya16be monob
```

png 只接受 RGB 和灰度。要用它，得先用 L09 的 libswscale（或者 L12 的滤镜图）把画面转成 rgb24。
实际程序里的做法是：先查编码器接受哪些格式（`avcodec_get_supported_config()`，`encoder_info.c` 里有用法），输入不在其中就先转换。

再看色彩属性。L09 讲过，YUV 数据要配上色彩矩阵和范围才能正确地还原成 RGB。上面编出的文件里没有这些信息：

```bash
ffprobe -v error -show_entries stream=color_range,color_space,color_primaries,color_transfer -of csv=p=0 out/a.264
```

```text
unknown,unknown,unknown,unknown
```

播放器只能猜。知道画面是什么标准时，应该在编码时写进去。它们是通用选项：

```bash
$B/encode_yuv out/src.y4m out/e.264 libx264 colorspace=bt709 color_range=tv color_primaries=bt709 color_trc=bt709 2>/dev/null | sed -n 3p
ffprobe -v error -show_entries stream=color_range,color_space,color_primaries,color_transfer -of csv=p=0 out/e.264
```

```text
色彩矩阵 bt709，色彩范围 tv
tv,bt709,bt709,bt709
```

这些选项不改变画面数据，只是在码流里做了标记（文件大了 4 个字节）。标记要和数据的实际情况一致，标错了比不标更糟。
在转码程序里，正确的做法是把解码出来的帧上的色彩属性原样交给编码器，L16 会这样做。

色彩范围还会影响编码器是否接受一种格式。mjpeg 默认要求全范围的 YUV：

```bash
$B/encode_yuv out/src.y4m out/x.mjpeg mjpeg 2>&1 | head -1
$B/encode_yuv out/src.y4m out/x.mjpeg mjpeg color_range=pc | tail -3
```

```text
[mjpeg @ 0x5613fd277540] Non full-range YUV is non-standard, set strict_std_compliance to at most unofficial to use it.
送进 125 帧，写出 125 个包，其中关键帧 125 个，冲刷时写出 0 个
共 879547 字节，时长 5.00 秒，平均码率 1407 kb/s
写出 out/x.mjpeg
```

（这里为了演示直接标成了 `pc`。我们的素材实际是有限范围的，这样标出来的文件播放时对比度会不对，正确的做法是先把数据转换成全范围。）
mjpeg 每一帧都是独立的 JPEG 图片，所以 125 个包全是关键帧，也不留帧。

## 常见错误

1. **不冲刷编码器。** 少最后几帧到几十帧。libx264 默认设置下 125 帧里有 41 帧是冲刷时才出来的。
2. **没有设 `time_base`，或者帧的 `pts` 不递增。** 编码器打不开，或者不停地警告 `non-strictly-monotonic PTS`，码率控制也会失常。
3. **送进去的帧和编码器的参数不一致。** 尺寸、像素格式必须和 `avcodec_open2()` 时给的相同。
4. **像素格式不被编码器支持。** `avcodec_open2()` 失败并列出支持的格式。先查询，必要时先转换。
5. **反复使用同一个 `AVFrame` 时没有调 `av_frame_make_writable()`。** 编码器可能还引用着上一帧的数据，直接改写会破坏参考帧。
6. **选项名写错而不自知。** `avcodec_open2()` 不会因此失败，要自己检查字典里有没有剩下的项。
7. **把私有选项用在别的编码器上。** `crf`、`preset` 是 libx264（和少数几个编码器）的，换成硬件编码器就没有了，名字相同含义也可能不同。
8. **同时指定码率和 CRF。** 两种码率控制方式二选一，都给时以哪个为准由编码器决定。
9. **不检查写文件的结果。** 磁盘满了照常报告成功（官方示例的 patch 0053）。
10. **以为裸码流就是可以交付的文件。** 裸码流没有时间戳、不能准确定位，也不是所有格式都能这样存。交付的文件要用容器（L15）。

## 练习

1. 用 `b=1M` 编码 `out/src.y4m`。先算一下文件大约多大，再运行核对。
2. 给 libx264 加 `tune=zerolatency`。冲刷时写出几个包？重排延迟是多少？文件大小和默认设置比怎样？这个选项适合什么场合？
3. 先用 L12 的 `filter_video` 把素材缩小到 320 宽（`scale=320:-2`），再用 `encode_yuv` 以默认设置编码。码率大约是 640 宽时的多少？
4. 把 `encode_yuv.c` 里冲刷编码器的那一句 `ret = encode(enc, NULL, pkt, out, &st);` 改成 `ret = 0;`，重新编译，
   分别用 libx264 和 `mpeg2video bf=2` 编码。各写出多少个包？做完把代码改回去。
5. 把 `encode_yuv.c` 里的 `frame->pts = st.nb_frames;` 改成 `frame->pts = 0;`，重新编译，用 libx264 编码（不要加 `2>/dev/null`）。
   看到了什么警告？包的 pts 是多少？做完把代码改回去。

<details>
<summary>参考答案</summary>

1. 1,000,000 × 5 ÷ 8 = 625,000 字节。实际 595,364 字节，平均码率 953 kb/s，比目标低 5%。
2. 冲刷时写出 0 个，重排延迟 0 帧：每送一帧立刻得到这一帧的包。文件 601,511 字节，比默认的 445,697 大 35%。
   zerolatency 关掉了 B 帧和预读，压缩率下降，换来的是编码器不留帧。适合视频通话、云游戏这类要求实时的场合。
3. 输出是 320x180，98,531 字节，158 kb/s，约为 640 宽时（713 kb/s）的 22%。像素数是四分之一，码率也接近四分之一。
4. libx264 写出 84 个包（少 41 个），ffprobe 数出来也是 84 帧，最后 1.6 秒没有了。`mpeg2video bf=2` 写出 123 个包（少 2 个）。
   不冲刷丢多少取决于编码器和参数，和 L11、L12 里滤镜图的结论一样：必须总是冲刷。
5. libx264 对第 2 帧起的每一帧警告 `non-strictly-monotonic PTS`，一共 124 次；写出的每个包 pts 和 dts 都是 0。
   程序仍然“成功”结束，裸码流也能播放（裸码流里本来就不存时间戳）。但这些包一旦要装进容器，时间戳全是 0 的文件是没法用的。

</details>

## 下一讲

L14 学音频编码，例子是官方示例 `encode_audio.c`。音频编码多了一个约束：编码器要求每一帧的采样数是固定的。
还会回答前面几讲留下的问题：AAC 的第一个包为什么 pts 是 -1024，编码器为什么要在开头多编一帧。
