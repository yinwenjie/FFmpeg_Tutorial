# L15 编码并封装

L13、L14 编出来的都是裸码流：一个文件里只有一路流，没有时间戳，也没法准确定位。
这一讲把视频包和音频包交给**复用器**（muxer），写成一个真正的媒体文件。
这是 L03、L04 的反方向：那时是从 MP4 里读包，现在是把包写进 MP4。

完成标志：`mux_av` 写出的 MP4 与 `ffmpeg` 命令行的结果逐字节相同；
能说清楚编码器的时间基和文件里的流时间基为什么不同、什么时候要换算；
知道全局头（global header）是什么、MP4 的索引为什么要等到最后才写；
会用比特流滤镜把 MP4 里的 H.264 取出来写成裸码流。

## 本讲要点

**复用的固定套路**

| 步骤 | 函数 | 要点 |
| --- | --- | --- |
| 1 创建输出上下文 | `avformat_alloc_output_context2()` | 格式按文件扩展名猜，也可以直接指定 |
| 2 加流、打开编码器 | `avformat_new_stream()`、`avcodec_open2()`、`avcodec_parameters_from_context()` | 把编码器的参数（含 extradata）复制给流 |
| 3 打开输出文件 | `avio_open()` | 有的格式自己管文件（`AVFMT_NOFILE`） |
| 4 写文件头 | `avformat_write_header()` | 所有流的参数都设好以后才能调；返回后流的时间基才确定 |
| 5 循环写包 | `av_packet_rescale_ts()`、`av_interleaved_write_frame()` | 换算时间戳，设 `stream_index`，交给复用器 |
| 6 写文件尾 | `av_write_trailer()` | MP4 的索引在这里写，不能省，结果要检查 |
| 7 关闭、释放 | `avio_closep()`、`avformat_free_context()` | 关闭文件时才会把缓冲里的数据写出去，结果也要检查 |

**两个时间基**

编码器有自己的时间基（视频 `1/25`，音频 `1/44100`），文件里的每一路流也有自己的时间基。两者通常不同，而且**文件里的时间基要到
`avformat_write_header()` 返回以后才确定**，复用器可以不接受我们建议的值。所以每个包写进去之前要用
`av_packet_rescale_ts(pkt, 编码器时间基, 流时间基)` 换算一次，pts、dts、duration 一起换。

**全局头**

H.264 解码要先知道 SPS、PPS，AAC 要知道采样率和声道配置。这些参数集放在哪里，各种格式的做法不同：

- MP4、MKV：放在文件头里（编码器的 `extradata`），包里不再重复；
- MPEG-TS、裸 H.264：放在码流里，每个关键帧前面带一份。

复用器用 `AVFMT_GLOBALHEADER` 标志说明自己属于哪一类。属于前一类时，要在 `avcodec_open2()` **之前**给编码器设
`AV_CODEC_FLAG_GLOBAL_HEADER`，编码器才会把参数集放进 `extradata`，而不是塞进码流。

**交织**

文件里的包要按时间顺序排列，同一时刻的画面和声音挨在一起，播放器才能一边读一边播。
`av_interleaved_write_frame()` 会在内部缓存，按 dts 排好顺序再写；`av_write_frame()` 收到就写，顺序要调用者保证。

**文件头和文件尾**

MP4 由一个个 box 组成：`ftyp`（文件类型）、`mdat`（所有包的数据）、`moov`（索引：每一路流的参数，每个包的位置、大小、时间）。
`moov` 要等所有包都写完才知道内容，所以默认写在文件末尾。`mdat` 的长度在开头，也是写完才知道，
复用器先写一个占位值，最后回到文件开头填上。这和 L06 写 WAV 头时先占位、写完数据再 `seek` 回去填长度是同一个思路。
所以 MP4 不能直接写到管道这样不能 `seek` 的输出里。

**比特流滤镜**

H.264 的码流有两种封装方式：Annex B（NAL 单元前面是起始码 `00 00 00 01`，参数集在码流里）和 AVCC（NAL 单元前面是它的长度，
参数集在 extradata 里）。MP4、MKV 用 AVCC，裸码流、TS 用 Annex B。从 MP4 里读出来的包是 AVCC 的，要写成 `.h264`
必须先过一个只改写字节、不解码的“比特流滤镜”：`h264_mp4toannexb`。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L15_mux                        # 可执行文件在 L15_mux/build/ 下
B=L15_mux/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `mux` | 官方示例（加了课程注释）：把程序算出的画面和声音编码，写进一个文件 |
| `mux_av` | 把 Y4M 和 WAV 编码，写进一个文件；格式由扩展名决定 |
| `extract_h264` | 用比特流滤镜把 MP4 里的 H.264 取出来，写成 `.h264` |

需要 L13 的 Y4M 和 L14 的 WAV。已经做过的可以跳过：

```bash
ffmpeg -v error -y -i samples/small_360p25.mp4 -pix_fmt yuv420p out/src.y4m
ffmpeg -v error -y -i samples/small_360p25.mp4 -vn -c:a pcm_s16le out/src.wav
```

## 第 1 步：官方示例 mux.c

```bash
$B/mux out/official.mp4 2>/dev/null > out/official.log
sed -n 1,3p out/official.log | cut -c1-110
wc -l < out/official.log
ffprobe -v error -count_frames -show_entries stream=codec_name,nb_frames,nb_read_frames,duration -of csv=p=0 out/official.mp4
```

```text
pts:-1024 pts_time:-0.02322 dts:-1024 dts_time:-0.02322 duration:1024 duration_time:0.02322 stream_index:1
pts:0 pts_time:0 dts:0 dts_time:0 duration:1024 duration_time:0.02322 stream_index:1
pts:1024 pts_time:0.02322 dts:1024 dts_time:0.02322 duration:1024 duration_time:0.02322 stream_index:1
683
h264,10.000000,251,250
aac,10.007800,432,431
```

示例自己算出 10 秒的画面（渐变色块）和声音（频率不断升高的正弦波），分别编码，每写一个包打印一行：时间戳和这个包属于哪一路流
（0 是视频，1 是音频）。输出格式由扩展名决定，两路流都用这种格式的默认编码器。换成 `.mkv`、`.ts`、`.avi` 都可以。

读 `L15_mux/mux.c` 时注意：

- `main()` 的流程就是上面“复用的固定套路”那张表，注释里也列出来了。
- `write_frame()` 里有两行是这一讲的主角：`av_packet_rescale_ts()` 和 `av_interleaved_write_frame()`。
- `add_stream()` 末尾设置全局头标志。

这个示例有 9 个问题已经做成 patch（`upstream_patches/` 的 0062 到 0070），代码里都有标注。上面的输出里就能看到两个：

- 视频写了 251 个包，不是 250 个（0064：判断结束时多生成了一帧）；文件里记着 251 帧，能读出来的只有 250 帧（0065：帧没有时长，
  MP4 里最后一个包的时长是 0，落在编辑列表之外，播放时被丢掉）。两个错误互相抵消，表面上看帧数是对的。
- 写文件尾失败时不报错。下面把文件大小限制在 398 KB（`ulimit -f`），数据写得进去，索引写不进去：

```bash
( trap '' XFSZ; ulimit -f 398; $B/mux out/lim.mp4 >/dev/null 2>&1; echo "退出码 $?" )
ffprobe -v error out/lim.mp4 2>&1 | head -2
```

```text
退出码 0
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x562d2f309a40] stream 1, contradictionary STSC and STCO
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x562d2f309a40] error reading header
```

程序报告成功，留下的却是一个打不开的文件（0062）。（方括号里的地址每次运行都不同。）

另外两个现象：`./mux out/official.gif` 报 `Specified pixel format yuv420p is not supported by the gif encoder`（0066），
`./mux 'out/img/f%03d.jpg'` 报 `Non full-range YUV is non-standard`（0067），都是退出码 1。用法说明里写着可以输出图片，实际打不开编码器。

## 第 2 步：mux_av，编码并封装

```bash
$B/mux_av out/src.y4m out/src.wav out/av.mp4 2>/dev/null
```

```text
输出格式：mp4（MP4 (MPEG-4 Part 14)）
视频：编码器 libx264，编码器时间基 1/25，文件里的流时间基 1/12800，extradata 40 字节
音频：编码器 aac，编码器时间基 1/44100，文件里的流时间基 1/44100，extradata 5 字节
视频：送进 125 帧，写出 125 个包，共 445659 字节
音频：读入 220500 个采样，送进编码器 220500 个采样（216 帧），写出 217 个包，共 77510 字节
写出 out/av.mp4
```

（`2>/dev/null` 丢掉的是 libx264 和 AAC 编码器打印的统计信息，不是错误。）

视频是 L13 的 Y4M，音频是 L14 的 WAV，编码和切帧的部分都是前两讲的代码：视频按 Y4M 的帧率设时间基，
音频用缓冲队列切成 1024 个采样一帧。新的部分是怎么把两路流写进同一个文件。和命令行比较：

```bash
ffmpeg -v error -y -i out/src.y4m -i out/src.wav -c:v libx264 -threads 1 -c:a aac out/cli.mp4
cmp out/av.mp4 out/cli.mp4 && echo 相同
ls -l out/av.mp4 | awk '{print $5}'
ffprobe -v error -show_entries stream=codec_name,time_base,nb_frames,duration -of csv=p=0 out/av.mp4
```

```text
相同
528739
h264,1/12800,5.000000,125
aac,1/44100,5.000000,217
```

逐字节相同。125 帧的 H.264 视频加 217 个包的 AAC 音频，都是 5 秒，一共 528,739 字节。

两处值得说明：

- 命令行默认会给每一路流写一条元数据 `encoder=Lavc… libx264`，MP4 复用器把它写进视频的描述里。
  `mux_av.c` 里的 `add_stream()` 末尾补上了同样的一条，所以结果相同。
- 音频的 217 个包比 L14 里的 216 帧多一个，是 AAC 开头多编的那一帧，pts 是 -1024（L14 讲过）。
  MP4 里记着“开头丢 1024 个采样”，所以 `duration` 仍是 5 秒。

`mux_av.c` 的主循环：

```text
每次循环：视频和音频，谁的下一帧时间更靠前就先处理谁（av_compare_ts）
    视频：读一帧 → avcodec_send_frame → write_packets()
    音频：队列不够一帧就读 WAV → 取一帧 → avcodec_send_frame → write_packets()
冲刷两个编码器，av_write_trailer()
```

`write_packets()` 是所有包写进文件的出口：取出编码器的包，换算时间戳，设流编号，`av_interleaved_write_frame()`。

## 第 3 步：时间基，编码器的和文件里的

上面第 2 步的输出里有“编码器时间基 1/25，文件里的流时间基 1/12800”。同一个程序换几种格式，看文件里的时间基：

```bash
for f in mp4 mkv ts avi flv; do
  echo "-- $f"; $B/mux_av out/src.y4m out/src.wav out/m.$f 2>/dev/null | grep '时间基' | sed 's/，extradata.*//'
done
```

```text
-- mp4
视频：编码器 libx264，编码器时间基 1/25，文件里的流时间基 1/12800
音频：编码器 aac，编码器时间基 1/44100，文件里的流时间基 1/44100
-- mkv
视频：编码器 libx264，编码器时间基 1/25，文件里的流时间基 1/1000
音频：编码器 ac3，编码器时间基 1/44100，文件里的流时间基 1/1000
-- ts
视频：编码器 mpeg2video，编码器时间基 1/25，文件里的流时间基 1/90000
音频：编码器 mp2，编码器时间基 1/44100，文件里的流时间基 1/90000
-- avi
视频：编码器 mpeg4，编码器时间基 1/25，文件里的流时间基 1/25
音频：编码器 ac3，编码器时间基 1/44100，文件里的流时间基 128/3675
-- flv
视频：编码器 flv，编码器时间基 1/25，文件里的流时间基 1/1000
音频：编码器 adpcm_swf，编码器时间基 1/44100，文件里的流时间基 1/1000
```

每种格式有自己习惯的时间基：MKV 和 FLV 用毫秒，TS 用 90 kHz，MP4 的视频用 12800（帧率的整数倍），音频用采样率，
AVI 的音频是 `128/3675`，等于 1536 ÷ 44100，也就是一个 AC-3 包（1536 个采样）的时长。而且这些值是复用器在 `avformat_write_header()` 里定下的，
我们在 `add_stream()` 里给流设的 `time_base` 只是建议。

所以每个包写进去前必须换算。`mux_av.c` 里是一行：

```c
av_packet_rescale_ts(os->pkt, os->enc->time_base, os->st->time_base);
```

加 `log=1` 可以看到每个包换算之后的时间戳（单位是 MP4 视频流的 1/12800）：

```bash
$B/mux_av out/src.y4m out/src.wav out/av.mp4 log=1 2>/dev/null | grep '^视频 包' | sed -n 1,4p
```

```text
视频 包     0  pts        0  dts    -1024  时长   512    8909 字节  关键帧
视频 包     1  pts     2048  dts     -512  时长   512    5027 字节
视频 包     2  pts     1024  dts        0  时长   512    3379 字节
视频 包     3  pts      512  dts      512  时长   512    2988 字节
```

- 一帧的时长是 512：12800 ÷ 25 = 512。
- 第 0 个包的 dts 是 -1024，比 pts 小 1024，也就是 2 帧。这就是 L08 讲的 B 帧重排：解码顺序和显示顺序不同，
  第 2 帧（pts 1024）要比第 1 帧（pts 512）先解码，解码器要先收到 2 帧才能开始输出，所以 dts 要整体往前挪 2 帧，
  保证 dts 不大于 pts。L13 里 `encode_yuv` 打印的“重排延迟 2”就是这个数。

## 第 4 步：全局头和 extradata

第 2 步的输出里还有 `extradata 40 字节`（视频）和 `5 字节`（音频）。这就是编码器放在文件头里的参数集：
视频的 40 字节是 H.264 的 SPS 和 PPS，音频的 5 字节是 AAC 的配置（采样率、声道布局）。换成 MPEG-TS：

```bash
$B/mux_av out/src.y4m out/src.wav out/m.ts 2>/dev/null | grep extradata | sed 's/，编码器时间基.*文件里的/ …/'
```

```text
视频：编码器 mpeg2video …流时间基 1/90000，extradata 0 字节
音频：编码器 mp2 …流时间基 1/90000，extradata 0 字节
```

TS 的参数在码流里，没有 extradata。程序里对应的是 `add_stream()` 里的这两行：

```c
if (c->oc->oformat->flags & AVFMT_GLOBALHEADER)
    enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
```

复用器说它要全局头，就给编码器设标志。这一行必须在 `avcodec_open2()` 之前，编码器是在打开时决定参数集放在哪里的。
去掉这两行会怎样，是练习 1。

另一个次序要求：`avcodec_parameters_from_context()` 必须在 `avcodec_open2()` **之后**，因为 `extradata` 是打开时才生成的。
复用器写文件头时读的是流的 `codecpar`，不读编码器上下文。

## 第 5 步：交织

`log=1` 打印的是包从编码器出来的顺序，不是它们在文件里的顺序。比较一下：

```bash
$B/mux_av out/src.y4m out/src.wav out/av.mp4 log=1 2>/dev/null | grep -c '^音频 包'
$B/mux_av out/src.y4m out/src.wav out/av.mp4 log=1 2>/dev/null | grep -n '^视频 包' | sed -n 1p
ffprobe -v error -show_entries packet=stream_index,dts,pos -of csv=p=0 out/av.mp4 | sort -t, -k3 -n | head -10 | cut -d, -f1,2
```

```text
217
74:视频 包     0  pts        0  dts    -1024  时长   512    8909 字节  关键帧
0,-1024
0,-512
1,-1024
0,0
1,0
1,1024
0,512
1,2048
1,3072
0,1024
```

前两条命令说：第 1 个视频包出现在日志的第 74 行（前 3 行是文件信息），在它前面有 70 个音频包，音频一共 217 个包。libx264 要先看后面的 40 来帧才开始输出，
视频比音频晚出包。如果程序按出包顺序直接写文件，就会先写好几秒的音频，再写视频。

文件里的顺序（第三个命令，按位置排序，每行是流编号和 dts）是按时间排好的：视频（0）和音频（1）交替出现。
这是 `av_interleaved_write_frame()` 的功劳：它把先到的包缓存起来，等另一路的包到了，再按时间顺序写出去。
可以数一下文件里“换一路流”的次数：

```bash
ffprobe -v error -show_entries packet=stream_index,pos -of csv=p=0 out/av.mp4 | sort -t, -k2 -n |
  awk -F, '{ if ($1!=last) {runs++; last=$1} } END{print "切换次数", runs}'
```

```text
切换次数 247
```

247 个交替的小块。如果不交织，文件里会是整段的视频接整段的音频，播放器边下边播时，要等整个视频段下完才能拿到第一个音频包。
练习 3 会改代码验证这一点。

## 第 6 步：文件尾、faststart 和不能 seek 的输出

MP4 的结构（第 2 步生成的文件）：

```bash
ffprobe -v trace out/av.mp4 2>&1 | grep -o "type:'[a-z0-9]*' parent:'root'" | cut -d"'" -f2 | tr '\n' ' '; echo
```

```text
ftyp free mdat moov
```

`mdat` 是全部包的数据，`moov` 是索引，在最后。播放器要先读到 `moov` 才知道怎么播，所以从网络上边下边播时，
要先把整个文件下完。截断文件看看（L03 做过同样的实验）：

```bash
head -c 100000 out/av.mp4 > out/trunc_av.mp4
ffprobe -v error out/trunc_av.mp4 2>&1 | head -1
```

```text
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x562f186d2cc0] moov atom not found
```

复用器有个选项 `movflags=+faststart` 可以把 `moov` 挪到前面。`mux_av` 把不认识的选项都交给 `avformat_write_header()`，所以直接传：

```bash
$B/mux_av out/src.y4m out/src.wav out/fs.mp4 movflags=+faststart 2>/dev/null | tail -1
ffprobe -v trace out/fs.mp4 2>&1 | grep -o "type:'[a-z0-9]*' parent:'root'" | cut -d"'" -f2 | tr '\n' ' '; echo
ls -l out/av.mp4 out/fs.mp4 | awk '{print $5}'
head -c 100000 out/fs.mp4 > out/trunc_fs.mp4
ffprobe -v error -show_entries stream=codec_name -of csv=p=0 out/trunc_fs.mp4 | tr '\n' ' '; echo
```

```text
写出 out/fs.mp4
ftyp moov free mdat
528739
528739
h264 aac
```

文件大小不变，`moov` 到了前面，截掉一半的文件也能读出流的参数。复用器是怎么做到的？它先按平常的次序写完，
最后在 `av_write_trailer()` 里把 `moov` 挪到前面，把整个文件重写了一遍。这要求输出文件能回头 `seek`。

不能 `seek` 的输出（管道、网络）就没法这样做。`pipe:3` 是第 3 号文件描述符，`3>out/pipe.mp4` 把它接到一个文件上，
但复用器只知道它是管道：

```bash
$B/mux_av out/src.y4m out/src.wav pipe:3 format=mp4 3>out/pipe.mp4 2>&1 >/dev/null | grep -v '^\[libx\|Qavg\|using'
```

```text
[mp4 @ 0x5574e617e380] muxer does not support non seekable output
写文件头失败：Invalid argument
```

MP4 要回头填 `mdat` 的长度，不能写到管道里。办法是“分片 MP4”：每个关键帧开一个片段（`moof`），不再有一个总的索引：

```bash
$B/mux_av out/src.y4m out/src.wav pipe:3 format=mp4 movflags=frag_keyframe+empty_moov 3>out/pipe2.mp4 >/dev/null 2>&1; echo "退出码 $?"
ffprobe -v error -count_frames -show_entries stream=codec_name,nb_read_frames -of csv=p=0 out/pipe2.mp4
```

```text
退出码 0
h264,125
aac,217
```

DASH、HLS 的 fMP4 和浏览器里的流式播放用的就是这种形式（输出名没有扩展名时必须给 `format=`）。

## 第 7 步：只有一路流，规范的多声道 WAV

`mux_av` 的视频或音频参数写 `none` 就是不要这一路。只要音频：

```bash
$B/mux_av none out/src.wav out/a.m4a 2>/dev/null | tail -2
ffprobe -v error -show_entries stream=codec_name,duration -of csv=p=0 out/a.m4a
```

```text
音频：读入 220500 个采样，送进编码器 220500 个采样（216 帧），写出 217 个包，共 77510 字节
写出 out/a.m4a
aac,5.000000
```

输出扩展名是 `.wav` 时，这种格式的默认音频编码器是 `pcm_s16le`，等于把 PCM 包进 WAV 容器。L10 留下过一个问题：
`resample_file` 写 5.1 声道的 WAV 时只写了简化的文件头，声道的含义靠约定。规范的做法是带声道掩码的扩展头，交给复用器来写：

```bash
ffmpeg -v error -y -f lavfi -i "sine=440:d=1" -ac 6 -c:a pcm_s16le out/six.wav
$B/mux_av none out/six.wav out/six_mux.wav 2>/dev/null | tail -2
cmp out/six.wav out/six_mux.wav && echo 相同
ffprobe -v error -show_entries stream=codec_name,channels,channel_layout -of csv=p=0 out/six_mux.wav
echo "fmt 块长度 $(od -An -tu4 -j16 -N4 out/six_mux.wav | tr -d ' ')，格式代码 0x$(od -An -tx2 -j20 -N2 out/six_mux.wav | tr -d ' ')，声道掩码 0x$(od -An -tx4 -j40 -N4 out/six_mux.wav | tr -d ' ')"
```

```text
音频：读入 44100 个采样，送进编码器 44100 个采样（45 帧），写出 45 个包，共 529200 字节
写出 out/six_mux.wav
相同
pcm_s16le,6,5.1
fmt 块长度 40，格式代码 0xfffe，声道掩码 0x3f
```

和 ffmpeg 写的文件逐字节相同。`fmt ` 块有 40 字节，格式代码是 `0xFFFE`（扩展格式），后面的声道掩码 `0x3f`
的 6 个二进制位分别是前左、前右、前中、低频、后左、后右。播放器因此知道每个声道是什么，`ffprobe` 读出来是 `5.1`。
`mux_av.c` 的 WAV 读取函数也认这种扩展头：读到掩码就按掩码建声道布局，不再按声道数去猜。

## 第 8 步：比特流滤镜，从 MP4 取出 H.264

```bash
$B/extract_h264 out/av.mp4 out/x.h264
ffmpeg -v error -y -i out/av.mp4 -c:v copy -bsf:v h264_mp4toannexb -an -f h264 out/cli_x.h264
cmp out/x.h264 out/cli_x.h264 && echo 相同
```

```text
读入 125 个包，写出 125 个包到 out/x.h264
相同
```

和命令行的结果逐字节相同。看一下滤镜做了什么：MP4 里第 0 个视频包的开头，和滤镜输出的开头：

```bash
ffprobe -v error -select_streams v:0 -show_packets -show_data -read_intervals '%+#1' out/av.mp4 | grep -A1 '^data' | tail -1
head -c 16 out/x.h264 | od -An -tx1
```

```text
00000000: 0000 02ae 0605 ffff aadc 45e9 bde6 d948  ..........E....H
 00 00 00 01 06 05 ff ff aa dc 45 e9 bd e6 d9 48
```

（第一行是 ffprobe 的十六进制显示，两字节一组，右边是对应的字符；只看开头 16 个字节。）

- MP4 里的包：`00 00 02 ae` 是后面这个 NAL 单元的长度（0x2ae = 686 字节），然后是 `06`（NAL 类型 6，SEI）。
- 滤镜输出：长度换成了起始码 `00 00 00 01`，其余原样。滤镜还会在每个关键帧前面补上 extradata 里的 SPS 和 PPS。

再确认画面没有变。把这个 `.h264`、L13 直接编出来的裸码流、MP4 三者解码的结果比较：

```bash
make -C L13_encode_video >/dev/null     # 需要 L13 的 encode_yuv
L13_encode_video/build/encode_yuv out/src.y4m out/l13.264 libx264 aspect=1 >/dev/null 2>&1
for f in out/x.h264 out/l13.264 out/av.mp4; do ffmpeg -v error -i $f -map 0:v -f md5 -; done
```

```text
MD5=98c53157351eb3afc06b5082d85b0985
MD5=98c53157351eb3afc06b5082d85b0985
MD5=98c53157351eb3afc06b5082d85b0985
```

三个文件的解码结果一样。（`.h264` 文件和 L13 的文件大小只差 1 字节，开头 SEI 和参数集的排列有些不同，不影响解码。）

`extract_h264.c` 里的用法和 L11 的滤镜图很像：

```text
av_bsf_get_by_name("h264_mp4toannexb")    找到滤镜
av_bsf_alloc()、avcodec_parameters_copy(bsf->par_in, …)、av_bsf_init()    创建、告诉它输入流的参数、初始化
循环：av_read_frame() → av_bsf_send_packet() → 循环 av_bsf_receive_packet() → 写文件
结束：av_bsf_send_packet(bsf, NULL) 冲刷，再取一遍
```

和编码器一样，取到 `AVERROR(EAGAIN)` 是“还要更多输入”，`AVERROR_EOF` 是“已经结束”，都不是错误。

## 第 9 步：出错会怎样

```bash
$B/mux_av out/src.y4m out/src.wav out/x.xyz 2>&1 >/dev/null | tail -1
$B/mux_av out/src.y4m out/src.wav out/x.mkv movflags=+faststart 2>&1 >/dev/null | tail -1
$B/mux_av out/src.y4m out/src.wav out/x.mp4 vcodec=aac 2>&1 >/dev/null | tail -1
$B/mux_av out/src.y4m out/src.wav out/x.gif 2>&1 >/dev/null | tail -1
$B/mux_av out/src.y4m out/src.wav /nonexistent/x.mp4 2>&1 >/dev/null | grep -v '^\['
ln -sf /dev/full out/full.mkv; $B/mux_av out/src.y4m out/src.wav out/full.mkv 2>&1 >/dev/null | grep -v '^\['
```

```text
不知道输出文件 out/x.xyz 是什么格式，请用 format= 指定
复用器 matroska 不认识选项 movflags（值 +faststart）
找不到视频编码器 aac（这种格式没有默认的视频编码器时要用 vcodec= 指定）
视频编码器 gif 不接受 yuv420p
打不开输出文件 /nonexistent/x.mp4：No such file or directory
写文件尾失败：No space left on device
```

- 扩展名猜不出格式时，`avformat_alloc_output_context2()` 返回 NULL。官方示例会改用 MPEG，我们的程序报错，让用户明确指定。
- 选项传给了不认识它的复用器：和 L13、L14 的编码器选项一样，`avformat_write_header()` 不会失败，要自己检查字典里有没有剩下的项。
- `/dev/full` 是永远写不进去的设备。这里错误出现在写文件尾（MKV 的索引在这里写），程序报错并返回 1；
  用不同的格式，错误会出现在写文件头、写包等不同的位置（`avformat_write_header()`、`av_interleaved_write_frame()`），都要检查。

## 常见错误

1. **忘了换算时间戳，或者换算用错了时间基。** 编码器的时间戳不能直接写进文件，要用 `av_packet_rescale_ts()`，
   而且目标时间基是 `avformat_write_header()` 之后流里的值（练习 2）。
2. **在 `avformat_write_header()` 之前就把流的时间基当成最终值。** 复用器可能改掉它（第 3 步，AVI 的音频是 `128/3675`）。
3. **没有设全局头标志，或者在 `avcodec_open2()` 之后才设。** 编码器已经按另一种方式输出了，MKV 写不出文件头，MP4 的内容也不一样（练习 1）。
4. **`avcodec_parameters_from_context()` 在 `avcodec_open2()` 之前调用。** `extradata` 还没有，文件头里没有参数集，播放器解不了码。
5. **不写文件尾，或者不检查它的结果。** MP4 的索引在这里写，没有它文件打不开；磁盘满了这一步会失败，示例却照样返回 0（官方示例的 patch 0062）。
6. **不检查 `avio_closep()` 的结果。** 缓冲区里剩下的数据在关闭时才写出去，这里也可能失败。
7. **忘了设包的 `stream_index`。** 包都写进第 0 路流。
8. **视频帧没有时长。** MP4 里最后一个包的时长是 0，最后一帧播放时被丢掉（官方示例的 patch 0065，L16 的 `transcode.c` 也有同样的问题）。
9. **两路流各写各的，不交织。** 文件里先是整段视频再是整段音频，边下边播要等很久，编辑软件定位也慢（练习 3）。
10. **从 MP4、MKV 里取出 H.264 包直接写成裸码流。** 它们是 AVCC 格式，没有起始码，播放器不认；要过 `h264_mp4toannexb`（练习 4）。

## 练习

1. 把 `mux_av.c` 的 `add_stream()` 里这一句注释掉：`enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;`，重新编译，
   分别输出 `.mkv` 和 `.mp4`。各发生什么？MP4 的文件和原来比有什么不同？做完把代码改回去。
2. 把 `write_packets()` 里的 `av_packet_rescale_ts(...)` 这一行注释掉，重新编译，输出 `.mp4`，
   用 `ffprobe -v error -show_entries stream=codec_name,duration -of csv=p=0` 看两路流的时长。为什么视频出问题、音频没有问题？做完把代码改回去。
3. 把主循环里 `if (video_active && (!audio_active || av_compare_ts(...) <= 0))` 改成 `if (video_active)`（先把视频全部写完，再写音频），
   重新编译，输出 `.mp4`，和原来的文件比较。再把 `av_interleaved_write_frame()` 换成 `av_write_frame()`，文件有什么变化？
   用第 5 步最后的命令数“切换次数”。做完把代码改回去。
4. 把 `extract_h264.c` 主循环里的“送进滤镜、取出来”两步换成直接 `fwrite(pkt->data, 1, pkt->size, out)`（别忘了 `av_packet_unref()`），
   重新编译，对 `out/av.mp4` 运行，再用 `ffprobe` 看输出文件。看到了什么？文件开头的 4 个字节是什么？做完把代码改回去。
5. 第 3 步的日志里，第 0 个视频包的 pts 是 0、dts 是 -1024，第 1 个包的 pts 是 2048、dts 是 -512。
   如果不用 B 帧（`mux_av` 不能给编码器传选项，要在 `venc` 上加一行 `venc->max_b_frames = 0;`），这几个包的 pts 和 dts 会是什么？先推算，再改代码验证。

<details>
<summary>参考答案</summary>

1. 输出 `.mkv` 失败：`写文件头失败：Invalid data found when processing input`，退出码 1，文件只有 293 字节。
   MKV 需要从编码器的 extradata 里取参数集，没有全局头时 `extradata` 是空的。输出 `.mp4` 却成功了：MP4 复用器遇到 Annex B 格式的包会自己
   转换成 AVCC，并从第一个包里取出 SPS 和 PPS。文件 528,779 字节，比原来的 528,739 多 40 字节，和命令行的结果不再相同，
   但 `ffprobe` 仍然读出 125 帧视频、216 帧音频，解码不报错。能写成不等于写对，应当按复用器要求设标志。
2. 视频流 0.009766 秒（= 125 ÷ 12800），音频 5.000000 秒。视频的时间戳是以 1/25 为单位的帧序号（0 到 124），
   被当成 1/12800 单位的数字，整个视频不到 0.01 秒。音频没出问题是因为 MP4 里音频流的时间基也是 1/44100，和编码器相同，
   换算是恒等的。换成 `.ts`，音频也会出错：那里的音频时间基是 1/90000，音频流时长变成 2.470922 秒。
3. 只改选择条件：文件和原来逐字节相同（`cmp` 无输出）。`av_interleaved_write_frame()` 把先到的视频包缓存起来，
   等音频包到了再按 dts 排好写出，所以先后顺序不影响结果，代价是缓存了整个视频。
   再换成 `av_write_frame()`：文件 526,927 字节，切换次数是 1，前面全是视频，后面全是音频，两路各只有一个大块。
   文件仍然能读，125 帧视频、216 帧音频，但不交织。
4. 输出文件 445,660 字节，比滤镜的输出（445,699）少 39 字节（滤镜在关键帧前面补的参数集等）。
   `ffprobe` 报 `Invalid data found when processing input`，`ffmpeg` 解码时报 `No start code is found`、`missing picture in access unit`。
   开头 4 个字节是 `00 00 02 ae`，是第一个 NAL 单元的长度（686），不是起始码。
5. 不用 B 帧时，帧按显示顺序编码，没有重排延迟，dts 等于 pts：第 0 个包 pts 0、dts 0；第 1 个包 pts 512、dts 512；
   第 2 个包 pts 1024、dts 1024，第 3 个包 pts 1536、dts 1536，每个包时长都是 512（实测正是这样）。dts 比 pts 小 1024（2 帧）是因为 B 帧要等后面的参考帧先解码，
   解码器要先收到 2 帧才能开始输出，没有 B 帧就没有这个延迟。

</details>

## 下一讲

L16 做完整的转码：读一个文件，解码，处理，编码，再写成另一个文件，例子是官方示例 `transcode.c`。
这一讲写文件的部分会原样用到，新的东西是读、解码、滤镜和写之间的时间戳怎么一路换算，以及“只换容器、不重新编码”的流拷贝和重新编码怎么取舍。
