# L14 音频编码

L13 把画面编码成了视频码流，这一讲把声音编码成音频码流。调用方式和视频编码完全一样：送帧、取包、冲刷。
音频编码多出来的是一条约束：**编码器规定了每一帧必须有多少个采样**，送进去的帧不能多也不能少。
真实的声音不会正好按这个数量到来，所以要在前面加一个缓冲队列。这一讲的主要工作就是这个队列和跟着它走的时间戳。

完成标志：`encode_wav` 用 AAC 编码器编出的文件与 `ffmpeg` 命令行的结果逐字节相同；
能解释 AAC 的第一个包为什么 pts 是 -1024，解码 ADTS 文件为什么多出 1708 个采样；
能说出为什么要用缓冲队列，改了采样率之后时间戳怎么算。

## 本讲要点

**音频编码器要的参数**

```c
enc->sample_rate = 44100;                  /* 必须是编码器支持的采样率 */
enc->sample_fmt  = AV_SAMPLE_FMT_FLTP;     /* 必须是编码器支持的采样格式 */
av_channel_layout_copy(&enc->ch_layout, &layout);
enc->bit_rate    = 128000;                 /* 不设就用编码器的默认值 */
```

`time_base` 不设时默认是 1/采样率，帧的 `pts` 以采样为单位。

三样东西编码器都可能有限制，用 L13 见过的 `avcodec_get_supported_config()` 查：
`AV_CODEC_CONFIG_SAMPLE_FORMAT`、`AV_CODEC_CONFIG_SAMPLE_RATE`、`AV_CODEC_CONFIG_CHANNEL_LAYOUT`。
输入不符合要求时，先用 L10 的 libswresample 转换。

**固定帧长**

打开编码器之后，`enc->frame_size` 就是它要求的每帧采样数。除了最后一帧，每一帧都必须正好这么多。

| 编码器 | 每帧采样数 | 开头垫的采样数（`initial_padding`） | 接受的采样格式 |
| --- | --- | --- | --- |
| `aac` | 1024 | 1024 | fltp |
| `mp2` | 1152 | 481 | s16 |
| `ac3` | 1536 | 256 | fltp |
| `flac` | 4608（可以改） | 0 | s16、s32 |
| `pcm_s16le` | 0（不限） | 0 | s16 |

解码器、重采样器每次给出的采样数和这些数字都对不上，中间要加一个采样的缓冲队列 `AVAudioFifo`：

```text
来源（每次数量不定） → swr_convert() → av_audio_fifo_write()
                                              │
                    队列里够 frame_size 个 →  av_audio_fifo_read() 取出正好一帧 → avcodec_send_frame()
```

结束时的顺序：冲刷重采样器，把队列里剩下的不足一帧的采样作为最后一帧送进去，最后冲刷编码器。

**时间戳按送进编码器的采样数算**

```c
frame->pts = 已经送进编码器的采样数;     /* 时间基 1/输出采样率 */
```

经过重采样和队列之后，帧的边界和输入已经没有对应关系，采样率也可能变了。时间戳只能在队列的出口重新数。

**开头的垫头和末尾的填充**

AAC、MP3 这类编码器，相邻两帧的数据是重叠一半的，解码器拿到相邻两个包才能还原出中间那 1024 个采样。
为了让第一段声音也能还原，编码器在声音开始之前垫了一帧静音，多编出一个包，它的 pts 是 -1024。
最后一帧不满 1024 个采样，编码器用静音补齐，在包的 side data（`AV_PKT_DATA_SKIP_SAMPLES`）里记下补了多少个。

MP4 这样的容器会记录“开头丢多少、末尾丢多少”，解码后得到的采样数和原来一个不差。
ADTS 裸流没有地方记，解码出来就多了开头的 1024 个和末尾补的那些。

**AAC 裸流要加 ADTS 头**

MP2、AC-3 的包自带帧头，直接连起来就是能播放的文件。AAC 编码器输出的包没有帧头，
采样率、声道数放在 `extradata` 里。写成裸流时要在每个包前面加 7 字节的 ADTS 头，或者把包交给容器（L15）。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L14_encode_audio               # 可执行文件在 L14_encode_audio/build/ 下
B=L14_encode_audio/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `encode_audio` | 官方示例（加了课程注释）：把程序算出的 440 Hz 正弦波编码成 MP2 |
| `encode_wav` | 把 WAV 文件编码成裸码流，编码器和参数从命令行给 |

准备一份未压缩的输入，把素材的音轨解码成 16 位 PCM 的 WAV 文件：

```bash
ffmpeg -v error -y -i samples/small_360p25.mp4 -vn -c:a pcm_s16le out/src.wav
ls -l out/src.wav | awk '{print $5}'
```

```text
882078
```

5 秒、44100 Hz、双声道、16 位：44100 × 5 × 2 × 2 = 882,000 字节，加上 78 字节的文件头。一共 220,500 个采样（每个声道）。

## 第 1 步：官方示例 encode_audio.c

```bash
$B/encode_audio out/tone.mp2
ls -l out/tone.mp2 | awk '{print $5}'
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels,bit_rate:format=duration -of default=nw=1 out/tone.mp2
ffprobe -v error -count_packets -show_entries stream=nb_read_packets -of csv=p=0 out/tone.mp2
```

```text
41795
codec_name=mp2
sample_rate=44100
channels=2
bit_rate=64000
duration=5.224375
200
```

示例什么也不打印。它算出 200 帧、每帧 1152 个采样的 440 Hz 正弦波（200 × 1152 ÷ 44100 = 5.22 秒），
用 MP2 编码器以 64 kb/s 编码，把包直接写进文件。可以用 `ffplay out/tone.mp2` 听一下。

读 `L14_encode_audio/encode_audio.c` 时注意这几处：

- `check_sample_fmt()`、`select_sample_rate()`、`select_channel_layout()` 三个函数分别查编码器支持的采样格式、采样率、声道布局。
- `frame->nb_samples = c->frame_size;`：帧的大小取自编码器。示例的声音是自己算的，要多少算多少，所以不需要缓冲队列。
- `encode()` 和 L13 的结构一模一样。

这个示例有 8 处问题已经做成 patch（`upstream_patches/` 的 0010、0016、0056 到 0061），代码里都有标注。两个能直接看到：

```bash
$B/encode_audio /dev/full; echo "退出码 $?"
$B/encode_audio; echo "退出码 $?"
```

```text
退出码 0
Usage: L14_encode_audio/build/encode_audio <output file>
退出码 0
```

写不进去（`/dev/full` 是一个永远“磁盘已满”的设备）和用法不对，退出码都是 0。

## 第 2 步：encode_wav，编码真实的声音

```bash
$B/encode_wav out/src.wav out/a.aac aac 2>/dev/null | sed -n '1,8p;$p'
```

```text
输入：44100 Hz，2 声道，s16
编码器 aac：44100 Hz，stereo，fltp，目标码率 128000 b/s
每帧 1024 个采样，开头垫 1024 个采样，时间基 1/44100
包   0  pts   -1024  时长 1024   205 字节
包   1  pts       0  时长 1024    82 字节
包   2  pts    1024  时长 1024   128 字节
包   3  pts    2048  时长 1024    99 字节
包   4  pts    3072  时长 1024   102 字节
写出 out/a.aac
```

（`2>/dev/null` 丢掉的是 AAC 编码器在关闭时打印的一行统计信息，以 `Qavg` 开头，不是错误。）

前三行是程序打开编码器之后读到的信息：

- 输入是 s16，AAC 编码器只接受 fltp，程序用 libswresample 做了转换。
- `frame_size` 是 1024，`initial_padding` 是 1024，时间基是 1/44100，所以 pts 和时长都以采样为单位。

和命令行的结果比较：

```bash
ffmpeg -v error -y -i out/src.wav -c:a aac -f adts out/cli.aac
cmp out/a.aac out/cli.aac && echo 相同
ls -l out/a.aac | awk '{print $5}'
```

```text
相同
79029
```

逐字节相同。882,078 字节的 WAV 变成了 79,029 字节，约十一分之一。

`encode_wav.c` 的主流程：

```text
读 WAV 文件头                         read_wav_header()
找编码器，设置采样率、声道布局、采样格式    select_sample_format()
把“选项=值”放进字典，打开编码器           avcodec_open2()
创建重采样器和缓冲队列                    swr_alloc_set_opts2()、av_audio_fifo_alloc()
循环：读 1000 个采样
      转换后写进队列                    convert_to_fifo()
      队列里够一帧就取出来编码            encode_from_fifo(…, 0)
冲刷重采样器
把队列里剩下的采样作为最后一帧编码         encode_from_fifo(…, 1)
冲刷编码器                             encode(enc, NULL, …)
```

## 第 3 步：217 个包和多出来的 1708 个采样

看最后几行：

```bash
$B/encode_wav out/src.wav out/a.aac aac 2>/dev/null | tail -5
```

```text
包 215  pts  219136  时长 1024   614 字节
包 216  pts  220160  时长  340   437 字节  末尾有 684 个采样是填充
读入 220500 个采样，送进编码器 220500 个采样（216 帧），写出 217 个包
共 77510 字节，时长 5.00 秒，平均码率 124 kb/s
写出 out/a.aac
```

**送进去 216 帧，出来 217 个包。** 220,500 ÷ 1024 = 215 余 340，所以是 215 个整帧加一个 340 采样的尾帧，共 216 帧。
多出来的那个包是第 0 个，pts 是 -1024：它编的是声音开始之前的一帧静音。

为什么要多编这一帧？AAC 把声音切成一段一段做变换，相邻两段重叠一半：每个包管着 2048 个采样，每次只往前走 1024 个。
解码器要把相邻两个包的结果叠加起来，才能还原出它们重叠的那 1024 个采样。
如果第一个包从采样 0 开始，采样 0 到 1023 只被一个包覆盖，还原不出来。
所以编码器在前面垫 1024 个静音，让第一个包覆盖 -1024 到 1023，第二个包覆盖 0 到 2047，真正的声音就都被覆盖了两次。
`initial_padding` 说的就是垫了多少，MP2 是 481，AC-3 是 256，原因类似，数值由各自的算法决定。

**最后一个包时长 340，末尾有 684 个采样是填充。** 尾帧只有 340 个采样，编码器补了 684 个静音凑成 1024，并在包的 side data 里记了下来。

现在把这个文件解码回去数一数：

```bash
ffmpeg -v error -y -i out/a.aac -f s16le out/a.pcm
ls -l out/a.pcm | awk '{print $5, $5/4}'
```

```text
888832 222208
```

222,208 个采样，比原来的 220,500 多 1708 个。1708 = 1024 + 684：开头垫的一帧，加上末尾补的填充。
217 个包每个解出 1024 个采样，217 × 1024 = 222,208，ADTS 裸流里没有地方记录哪些该丢，解码器就全部输出了。
这就是 L06 留下的那个问题（当时差了 6832 字节，6832 ÷ 4 = 1708）。

同样的内容放进 MP4 再解码：

```bash
ffmpeg -v error -y -i out/src.wav -c:a aac out/a.m4a
ffmpeg -v error -y -i out/a.m4a -f s16le out/m.pcm
ls -l out/m.pcm | awk '{print $5, $5/4}'
ffprobe -v error -show_entries packet=pts,duration:packet_side_data=skip_samples,discard_padding -of compact out/a.m4a | sed -n '1p;$p'
```

```text
882000 220500
packet|pts=-1024|duration=1024|side_datum/skip_samples:skip_samples=1024|side_datum/skip_samples:discard_padding=0
packet|pts=220160|duration=1024|side_datum/skip_samples:skip_samples=0|side_datum/skip_samples:discard_padding=684
```

一个不多，一个不少。MP4 记下了“第一个包开头丢 1024 个采样，最后一个包末尾丢 684 个”，
解复用器把它们放在包的 side data 里交给解码器，解码器照着丢。L07 看到的 pts 为 -1024 的第一个音频包就是这么来的。

## 第 4 步：固定帧长和缓冲队列

程序每次从 WAV 里读 1000 个采样（`READ_SAMPLES`），编码器每帧要 1024 个。对不上是故意的：
实际工作中，解码器给的每帧采样数由输入格式决定，重采样之后更是每次都不一样（L10）。

`encode_from_fifo()` 的核心：

```c
while (av_audio_fifo_size(fifo) >= frame_size || (flush && av_audio_fifo_size(fifo) > 0)) {
    const int nb = FFMIN(av_audio_fifo_size(fifo), frame_size);
    ...
    av_audio_fifo_read(fifo, (void **)frame->extended_data, nb);
    frame->pts = st->enc_samples;      /* 这一帧之前已经送进去的采样数 */
    st->enc_samples += nb;
    ret = encode(enc, frame, pkt, out, st);
}
```

- 平时只有队列里够一整帧才取。
- `flush` 为 1 时（输入已经结束），剩下的不足一帧的采样也取出来，作为最后一帧。只有最后一帧可以比 `frame_size` 小。

换几个编码器，看帧长怎么变：

```bash
for e in mp2 ac3 flac pcm_s16le; do $B/encode_wav out/src.wav out/t.$e $e 2>/dev/null | grep '每帧\|读入\|共'; done
```

```text
每帧 1152 个采样，开头垫 481 个采样，时间基 1/44100
读入 220500 个采样，送进编码器 220500 个采样（192 帧），写出 192 个包
共 240744 字节，时长 5.00 秒，平均码率 385 kb/s
每帧 1536 个采样，开头垫 256 个采样，时间基 1/44100
读入 220500 个采样，送进编码器 220500 个采样（144 帧），写出 144 个包
共 120372 字节，时长 5.00 秒，平均码率 193 kb/s
每帧 4608 个采样，开头垫 0 个采样，时间基 1/44100
读入 220500 个采样，送进编码器 220500 个采样（48 帧），写出 49 个包
共 63445 字节，时长 5.00 秒，平均码率 102 kb/s
每帧 0 个采样，开头垫 0 个采样，时间基 1/44100
读入 220500 个采样，送进编码器 220500 个采样（221 帧），写出 221 个包
共 882000 字节，时长 5.00 秒，平均码率 1411 kb/s
```

- **mp2**：每帧 1152。192 帧 192 个包，没有多出来的包。它开头垫的 481 个采样不够一帧，只体现在第一个包的 pts 是 -481 上。
  默认码率 384 kb/s。
- **ac3**：每帧 1536，默认码率 192 kb/s。
- **flac**：无损压缩，码率不由我们定，结果是 102 kb/s，约为 PCM 的 7%。多出来的第 49 个包大小是 0，
  它只带一份更新过的 `extradata`（编码结束后才知道的总采样数等），给能回头改文件头的容器用。
- **pcm_s16le**：`frame_size` 是 0，表示不限制帧长，程序读多少送多少。PCM “编码”只是按字节序摆放采样，输出和 WAV 的数据部分一样：

```bash
tail -c +79 out/src.wav | cmp - out/t.pcm_s16le && echo 相同
```

```text
相同
```

## 第 5 步：码率

音频编码通常直接指定码率，选项名和视频一样是 `b`：

```bash
for b in 64k 128k 192k; do $B/encode_wav out/src.wav out/b.aac aac b=$b 2>/dev/null | grep '共'; done
$B/encode_wav out/src.wav out/b.mp2 mp2 b=192k | grep '共'
```

```text
共 35772 字节，时长 5.00 秒，平均码率 57 kb/s
共 77510 字节，时长 5.00 秒，平均码率 124 kb/s
共 114852 字节，时长 5.00 秒，平均码率 184 kb/s
共 120372 字节，时长 5.00 秒，平均码率 193 kb/s
```

AAC 的实际码率比目标低一些（这段素材不难编），MP2 则是严格的恒定码率，每个包大小几乎相同（626 或 627 字节）。
同样 192 kb/s，MP2 和 AC-3 的文件大小完全一样（120,372 字节）：恒定码率下，大小只由码率和时长决定。

码率给得太低时编码器会提醒（去掉 `2>/dev/null` 能看到）：

```bash
$B/encode_wav out/src.wav out/b.aac aac b=64k 2>&1 >/dev/null | head -1
```

```text
[aac @ 0x55faed882600] 32 kb/s per channel at 44100 Hz: consider resampling the input to 32000 Hz or lower for better quality.
```

（方括号里的地址每次运行都不同。）它建议降低采样率，这就引出下一步。

## 第 6 步：改采样率，时间戳要重新数

`ar` 选项让编码器用另一个采样率，程序里的重采样器负责把输入转过去：

```bash
$B/encode_wav out/src.wav out/r.aac aac ar=22050 2>/dev/null | sed -n '2,6p;$p' | grep -v '^写出'
$B/encode_wav out/src.wav out/r.aac aac ar=22050 2>/dev/null | grep '^包\|读入' | tail -2
```

```text
编码器 aac：22050 Hz，stereo，fltp，目标码率 128000 b/s
每帧 1024 个采样，开头垫 1024 个采样，时间基 1/22050
包   0  pts   -1024  时长 1024   379 字节
包   1  pts       0  时长 1024   181 字节
包   2  pts    1024  时长 1024   258 字节
包 108  pts  109568  时长  682   999 字节  末尾有 342 个采样是填充
读入 220500 个采样，送进编码器 110250 个采样（108 帧），写出 109 个包
```

- 读入 220,500 个采样，送进编码器的是 110,250 个，正好一半。时间基也变成了 1/22050。
- 帧长仍然是 1024 个采样，但一帧的时间从 23 毫秒变成了 46 毫秒，所以只有 108 帧。
- 最后一个包的 pts 是 109,568，109,568 ÷ 22050 = 4.97 秒。如果还按输入文件里的采样位置（二十多万）当时间戳，时间就错了一倍。

升采样也一样：

```bash
$B/encode_wav out/src.wav out/r48.aac aac ar=48000 2>/dev/null | grep '读入'
```

```text
读入 220500 个采样，送进编码器 240000 个采样（235 帧），写出 236 个包
```

规则只有一条：**时间戳在缓冲队列的出口数，以送进编码器的采样数为准。**

## 第 7 步：参数不对会怎样

采样率不被编码器支持：

```bash
$B/encode_wav out/src.wav out/x.aac aac ar=12345 2>&1 | sed -n '1,3p;$p'
```

```text
[aac @ 0x55ede4181600] Specified sample rate 12345 is not supported by the aac encoder
[aac @ 0x55ede4181600] Supported sample rates:
[aac @ 0x55ede4181600]   96000
打不开编码器 aac：Invalid argument
```

选项名写错（和 L13 一样，靠检查字典里剩下的项发现）：

```bash
$B/encode_wav out/src.wav out/x.aac aac bb=1 2>&1 | head -1
```

```text
编码器 aac 不认识选项 bb（值 1）
```

其他几种：

```bash
$B/encode_wav out/src.wav out/x.h264 libx264
$B/encode_wav out/src.wav out/x.opus opus 2>&1 | tail -1
$B/encode_wav samples/small_360p25.mp4 out/x.aac aac
$B/encode_wav out/src.wav /dev/full aac 2>&1 | grep 失败; echo "退出码 ${PIPESTATUS[0]}"
```

```text
libx264 不是音频编码器
打不开编码器 opus：Experimental feature
输入不是 WAV 文件
写输出文件失败：No space left on device
退出码 1
```

FFmpeg 自带的 `opus` 编码器被标记为实验性的，默认不让用。实际项目里编码 Opus 用的是 `libopus`（需要在编译 FFmpeg 时启用）。

## 常见错误

1. **送进去的帧不是 `frame_size` 个采样。** 编码器报 `frame_size (1024) was not respected for a non-last frame` 或
   `nb_samples (2048) > frame_size (1024)`，`avcodec_send_frame()` 返回 `AVERROR(EINVAL)`。用缓冲队列切帧。
2. **在打开编码器之前读 `frame_size`。** 这时它是 0。`frame_size`、`initial_padding` 都是 `avcodec_open2()` 之后才有值。
3. **采样格式不对。** AAC、AC-3 要 fltp，MP2 要 s16。先查询编码器支持什么，必要时用 libswresample 转换。
4. **忘了队列里剩下的采样。** 输入结束时队列里通常还有不足一帧的采样，不取出来就丢了声音的最后一小段（练习 3）。
5. **忘了冲刷重采样器或编码器。** 结束时三步都要做：冲刷重采样器、清空队列、冲刷编码器。
6. **改了采样率，时间戳还按输入的采样位置算。** 声音的时间会按采样率的比例出错。
7. **把 AAC 的包直接连起来写进文件。** 没有 ADTS 头，播放器认不出来。写裸流要加头，或者交给容器。
8. **不设帧的 `pts`。** 写裸流时看不出问题，包要交给复用器时就没有时间戳可用（官方示例的 patch 0061）。
9. **以为解码出来的采样数等于编码前的采样数。** ADTS、MP2 这些裸流解码后会多出开头的垫头和末尾的填充。
   要精确对齐（例如和视频对口型、拼接多段声音），用能记录这些信息的容器。
10. **相信裸流的时长。** `ffprobe out/a.aac` 报的时长是 6.07 秒，实际是 5 秒：裸流里没有时间戳，时长是用文件大小和码率估出来的。

## 练习

1. 用 `mp2 b=96k` 编码 `out/src.wav`。先算一下一共多少帧、文件大约多大，再运行核对。
2. 用 `aac ar=8000` 编码。先算：送进编码器多少个采样？多少帧？多少个包？最后一个包里有多少个采样是填充？
   再把输出解码成 PCM，数一数有多少个采样。
3. 把 `encode_wav.c` 里的 `#define READ_SAMPLES 1000` 改成 4096，重新编译，用 aac 编码。输出和原来的 `out/a.aac` 一样吗？为什么？
   做完把代码改回去。
4. 把 `encode_wav.c` 里输入结束之后的那一句 `ret = encode_from_fifo(enc, fifo, frame, pkt, out, &st, 1);` 的最后一个参数改成 0，
   重新编译，用 aac 编码。送进编码器多少个采样、写出多少个包？少了多长的声音？做完把代码改回去。
5. 把 `encode_from_fifo()` 里的 `const int frame_size = enc->frame_size > 0 ? enc->frame_size : READ_SAMPLES;`
   改成 `const int frame_size = 1000;`，重新编译，用 aac 编码。看到了什么？做完把代码改回去。

<details>
<summary>参考答案</summary>

1. 220,500 ÷ 1152 = 191.4，所以是 192 帧（最后一帧不满）。192 帧的时长是 192 × 1152 ÷ 44100 = 5.0155 秒，
   96,000 × 5.0155 ÷ 8 = 60,186 字节。实际正好 60,186 字节，192 个包。MP2 是恒定码率，可以算得一字节不差。
2. 220,500 × 8000 ÷ 44100 = 40,000 个采样。40,000 ÷ 1024 = 39 余 64，所以 40 帧，41 个包（多一个垫头的包）。
   最后一帧只有 64 个采样，填充 1024 − 64 = 960 个。解码得到 41 × 1024 = 41,984 个采样，比 40,000 多 1984 = 1024 + 960。
3. 逐字节相同。队列把“每次读多少”和“每帧多少”隔开了，编码器看到的始终是同样的一串 1024 采样的帧。
4. 送进 220,160 个采样（215 帧），写出 216 个包，76,952 字节。队列里剩下的 340 个采样没有送进去，
   少了 340 ÷ 44100 = 7.7 毫秒的声音。7.7 毫秒听不出来，但是每一段都少一点，拼接或者和视频对齐时误差会累积。
5. 第一帧就失败：编码器报 `frame_size (1024) was not respected for a non-last frame`，
   程序打印“向编码器送帧失败：Invalid argument”后退出。帧长是编码器规定的，不是我们选的。

</details>

## 下一讲

L15 学封装：把 L13、L14 编出来的视频包和音频包交给复用器，写成一个 MP4 文件，例子是官方示例 `mux.c`。
到那时包的时间戳才真正派上用场，也会看到编码器的 `extradata` 是怎么进到文件头里的。
