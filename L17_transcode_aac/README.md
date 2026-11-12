# L17 音频转码与帧长适配

L16 把转码的整条流水线接起来了，音频那一路是靠滤镜图出口的 `av_buffersink_set_frame_size()` 把帧切成编码器要的长度。
这一讲看不用滤镜图的做法：官方示例 `transcode_aac.c` 把音频转成 AAC，格式转换交给 `libswresample`，
帧长对不上的问题用一个队列 `AVAudioFifo` 自己解决。另外我们自己写一个 `transcode_aac_av`，
功能更全：采样率、声道数、编码器、容器都可以换，输入有视频和封面图也不碍事。

完成标志：`transcode_aac_av` 的输出和 `ffmpeg` 命令行逐字节相同（37 个组合里有 32 个相同，另外 5 个的原因都查明了，见第 9 步）；
说得清楚解码器和编码器每帧的采样数为什么不一样、`AVAudioFifo` 每一轮攒了多少又切走了多少；
知道采样率不同时转换器会缓存采样，输入结束要冲刷，不冲刷会丢多少；
知道音频的时间戳可以不看输入，自己数出来，这样做的利和弊。

## 本讲要点

**音频转码要对齐五样东西**

解码器给什么，和编码器要什么，一般不一样。AAC 编码器为例：

| 要对齐的 | 解码器给的（例） | 编码器要的（AAC） | 谁来做 |
| --- | --- | --- | --- |
| 采样格式 | `s16p`（MP2）、`s16`（WAV）、`fltp`（Vorbis） | 只有 `fltp` | libswresample |
| 声道布局 | 单声道、立体声、5.1 | 编码器支持的任何一种；这里固定输出立体声 | libswresample（混音） |
| 采样率 | 192000、4000、44100 | 只有 13 个固定值：96000、88200、64000、48000、44100、32000、24000、22050、16000、12000、11025、8000、7350 | libswresample |
| 每帧的采样数 | 1152（MP2）、4096（WAV）、960（Opus） | 固定 1024，只有最后一帧可以短 | `AVAudioFifo` |
| 时间戳 | 输入流的时间基 | 编码器时间基 1/采样率 → 输出流的时间基 | 自己数 + `av_packet_rescale_ts()` |

前三行 L10 讲过，时间戳 L14、L15 讲过。这一讲新的是第四行：**每帧的采样数**。

**解码器每帧给多少，编码器每帧要多少**

用同一个 5 秒的素材（`samples/audio_44k_stereo.mp2`）转成各种编码，再用 `ffprobe -show_frames` 看每一帧的 `nb_samples`（命令见第 4 步）：

| 编码 | 解码器每帧给的采样数 | 备注 |
| --- | --- | --- |
| MP2 | 1152 | |
| WAV（PCM） | 4096 | PCM 没有“帧”，是解复用器按每秒约 10 个包来切的（取 2 的幂），采样率不同帧长也不同 |
| FLAC | 4096 | 等于编码时用的块大小，文件不同可能不同 |
| Vorbis | 1024 | |
| Opus | 960 | 第一帧 840，最后一帧 865：开头的预热部分被去掉了 |
| AC-3 | 1536 | |
| WMA | 2048 | |
| AAC | 1024 | |

| 编码器 | 每帧要的采样数 |
| --- | --- |
| AAC | 1024 |
| AC-3 | 1536 |
| MP2 | 1152 |
| FLAC | 默认的块大小，44.1 kHz 是 4608 |
| PCM | 不限制（`frame_size` 是 0） |

数字不一样，所以要有个地方把采样攒起来，够了再切。这个地方就是 **FIFO**（先进先出的队列）。

**FIFO 怎么工作**

输入是 MP2，解码器每次给 1152 个采样，AAC 编码器每次要 1024 个。每一轮都是“先攒、再切”：
解码、转换格式、把采样放进 FIFO；只要 FIFO 里够 1024 个，就取 1024 个去编码，剩下的留着。

| 轮次 | 放进去 | FIFO 里共有 | 取走编码 | 剩下 |
| --- | --- | --- | --- | --- |
| 第 1 轮 | 1152 | 1152 | 1 帧（1024） | 128 |
| 第 2 轮 | 1152 | 1280 | 1 帧 | 256 |
| 第 3 轮 | 1152 | 1408 | 1 帧 | 384 |
| …… | | | | 每轮多剩 128 |
| 第 8 轮 | 1152 | 2048 | **2 帧** | 0 |
| 第 9 轮 | 1152 | 1152 | 1 帧 | 128，从头再来 |

每轮多剩 128 个，攒够 8 轮就多出一整帧：8 个解码帧（9216 个采样）刚好编出 9 个 AAC 帧。这个 5 秒的素材：解码 192 帧，编码 216 帧，采样总数都是 221184。
输入读完以后 FIFO 里剩下的零头不够一帧，也要送给编码器，这就是最后一帧会比较短的原因（AAC 编码器允许最后一帧短）。

**两种办法**

L16 的 `transcode_av` 靠滤镜图出口切帧，这一讲的程序自己用 FIFO 切。两种办法做的是同一件事：

| | 滤镜图出口 `av_buffersink_set_frame_size()` | `AVAudioFifo` |
| --- | --- | --- |
| 代码量 | 一行 | 自己写攒和切的循环 |
| 格式转换 | 滤镜图自动插入 `aresample` | 自己建 `SwrContext` |
| 什么时候用 | 已经在用滤镜图（要改音量、混音、变速……） | 不想引入滤镜图，或者要更多控制 |
| 冲刷 | 对滤镜图送 EOF | 自己依次冲刷解码器、转换器、FIFO |

只转码、不做别的处理，两种办法的输出完全一样。滤镜图本身就是用同样的原理实现的。

**输入结束时的冲刷顺序**

上游的缓存要先吐出来，下游才能接得到。一路上有四个地方可能缓存着东西：

```text
解码器 ──► 重采样器 ──► FIFO ──► 编码器 ──► 复用器
(帧的延迟)  (滤波器的延迟)  (不足一帧的零头)  (编码器的延迟)
```

从左到右依次冲刷。漏掉哪一个，那里缓存的采样就丢了，程序不报错，输出只是悄悄少一截（练习 2、3）。

**音频的时间戳：自己数**

视频的时间戳要靠输入，因为帧的间隔可以不均匀（可变帧率）。音频不一样：每一帧有多少个采样是已知的，下一帧的 pts 就是这一帧的 pts 加采样数。
所以示例里只用一个计数器：每送一帧给编码器，`pts += nb_samples`，时间基是 1/采样率（L14）。好处是输出的时间戳绝对均匀，和输入容器的时间基粗不粗没有关系。
代价是输入时间戳里的偏移和抖动被忽略了：输出总是从 0 开始、连续。`ffmpeg` 命令行则跟着输入的时间戳走，第 8 步会看到两种做法的差别。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L17_transcode_aac              # 可执行文件在 L17_transcode_aac/build/ 下
B=L17_transcode_aac/build
mkdir -p out out/l17
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `transcode_aac` | 官方示例（加了课程注释，打了 4 个 patch）：把音频转成 AAC，96 kbit/s，立体声，采样率照输入 |
| `transcode_aac_av` | 自己写的：采样率、声道数、编码器、容器都可以换；输出与 `ffmpeg` 命令行相同 |
| `compare_cli.sh` | 一次性把 `transcode_aac_av` 和 `ffmpeg` 命令行在 37 个组合上逐字节比较（第 9 步） |

本讲用到的输入，都是 `samples/audio_44k_stereo.mp2` 用 `ffmpeg` 转出来的：

```bash
MP2=samples/audio_44k_stereo.mp2
D=out/l17
ffmpeg -v error -y -i $MP2 $D/in.wav
ffmpeg -v error -y -i $D/in.wav $D/in.flac
ffmpeg -v error -y -i $MP2 -c:a vorbis -strict -2 $D/in.ogg
ffmpeg -v error -y -i $MP2 -c:a opus -strict -2 $D/in.opus
ffmpeg -v error -y -i $MP2 -c:a ac3 $D/in.ac3
ffmpeg -v error -y -i $MP2 -c:a wmav2 $D/in.wma
```

## 第 1 步：官方示例 transcode_aac.c

```bash
$B/transcode_aac $MP2 out/official.m4a 2>/dev/null; echo "退出码 $?"
ffprobe -v error -count_packets -show_entries stream=codec_name,sample_rate,channels,duration,nb_read_packets -of csv=p=0 out/official.m4a
ffmpeg -v error -y -i $MP2 -c:a aac -b:a 96k -ac 2 out/ff.m4a
cmp out/official.m4a out/ff.m4a && echo "与 ffmpeg 命令行的输出逐字节相同"
```

```text
退出码 0
aac,44100,2,5.015510,217
与 ffmpeg 命令行的输出逐字节相同
```

输入 192 个 MP2 帧，输出 217 个 AAC 包，和命令行逐字节相同。217 比 216 多一个：AAC 编码器有 1024 个采样的延迟，所以文件开头有一个“预热包”
（第 8 步）。

> 这里用的是打过补丁的版本。提交 `a337e8d` 的原版有四个问题，第 2 步逐个看。读 `transcode_aac.c` 的注释，凡是改过的地方都标了 patch 编号。

读 `transcode_aac.c`，按调用顺序看这几处：

- `open_input_file()`：用 `av_find_best_stream()` 选一路音频，开解码器（patch 0080）。
- `open_output_file()`：编码器是 `avcodec_find_encoder(AV_CODEC_ID_AAC)`，参数写死或者抄输入；`avcodec_open2()` 之后 `frame_size` 才有值。
- `init_resampler()`：`swr_alloc_set_opts2()` 建转换器，这一讲的程序里采样率总是不变。
- `init_fifo()`：建 FIFO。
- `main()` 里的两层循环：里层第一个 `while` 是“攒”，第二个 `while` 是“切”；输入读完以后冲刷编码器。
- `encode_audio_frame()`：数 pts，送编码器，包的时间戳换算以后写文件（patch 0078）。

## 第 2 步：原版的四个问题

用 L02 编译出来的原版（`~/ffmpeg_src/doc/examples/transcode_aac`，没有打补丁）来看。命令里 `E` 指向它：

```bash
E=~/ffmpeg_src/doc/examples
```

**问题一（patch 0078）：换了容器，时间戳全错。**

```bash
for e in m4a mkv flv ts avi; do
    $E/transcode_aac $MP2 out/orig.$e >/dev/null 2>&1
    printf "%-4s %8d 字节  时长 %s 秒\n" $e $(stat -c %s out/orig.$e) $(ffprobe -v error -show_entries format=duration -of csv=p=0 out/orig.$e)
done
```

```text
m4a     62656 字节  时长 5.015510 秒
mkv     66444 字节  时长 222.208000 秒
flv     64921 字节  时长 222.208000 秒
ts      92308 字节  时长 2.480811 秒
avi   5399748 字节  时长 5159.659683 秒
```

输入是 5 秒的音频：MKV 和 FLV 变成 222 秒，TS 变成 2.5 秒，AVI 变成 5159 秒（文件有 5.4 MB，里面的音频只有 62 KB）。
只有 MP4 是对的。原因：示例让编码器用 1/采样率做时间基，也建议复用器用它，但是复用器可以不听：

| 容器 | 复用器实际选的流时间基 |
| --- | --- |
| MP4 | 1/44100，和建议的相同，所以输出碰巧对 |
| MKV、FLV | 1/1000 |
| TS | 1/90000 |
| AVI | 256/11025 |

原版写包之前没有换算时间戳，把 1/44100 单位的数字当成了别的单位。要用 `av_packet_rescale_ts()` 换算，L15 说过。
打了补丁的版本这样试：

```bash
for e in m4a mkv flv ts avi; do
    $B/transcode_aac $MP2 out/fixed.$e >/dev/null 2>&1
    printf "%-4s %8d 字节  时长 %s 秒\n" $e $(stat -c %s out/fixed.$e) $(ffprobe -v error -show_entries format=duration -of csv=p=0 out/fixed.$e)
done
```

```text
m4a     62656 字节  时长 5.015510 秒
mkv     63081 字节  时长 5.038000 秒
flv     64921 字节  时长 5.038000 秒
ts     100392 字节  时长 5.038722 秒
avi     71964 字节  时长 5.038730 秒
```

（MKV、FLV 的时长比 5.0155 多 23 毫秒：这两种容器没有 MP4 的编辑列表，那个 1024 个采样的预热包（第 8 步）也算进了时长；TS 文件为什么这么大，见第 7 步。）

**问题二（patch 0079）：有的解码器，文件尾的帧丢了。**

```bash
$E/transcode_aac $D/in.wma out/o3.m4a >/dev/null 2>&1
$B/transcode_aac $D/in.wma out/f3.m4a >/dev/null 2>&1
echo "命令行解码 $(ffmpeg -v error -i $D/in.wma -f s16le -ac 2 - | wc -c) 字节"
echo "原版转码后 $(ffmpeg -v error -i out/o3.m4a -f s16le -ac 2 - | wc -c) 字节"
echo "修好以后   $(ffmpeg -v error -i out/f3.m4a -f s16le -ac 2 - | wc -c) 字节"
```

```text
命令行解码 876544 字节
原版转码后 868352 字节
修好以后   876544 字节
```

少了 8192 个字节：2048 个采样 × 2 个声道 × 2 字节，正好是一整帧 WMA。原因：原版的 `decode_audio_frame()` 一读到文件尾就宣布“解码结束”，
可是有的解码器（带 `AV_CODEC_CAP_DELAY` 标志的，WMA、APE、Opus 都是）收到“没有更多输入了”的信号以后，才会把缓存的最后几帧吐出来。
正确的做法是：读到文件尾以后把空包送给解码器（冲刷），一直取到解码器自己报告 `AVERROR_EOF` 为止。这一点 L06 讲过，原版漏了。

**问题三（patch 0080）：输入不止一路流就拒绝。**

```bash
ffmpeg -v error -y -f lavfi -i color=c=red:s=64x64 -frames:v 1 $D/cover.png
ffmpeg -v error -y -i $D/in.flac -i $D/cover.png -map 0:a -map 1 -c copy -disposition:v attached_pic \
    -metadata title="My Song" -metadata artist="Me" $D/tagged.flac
$E/transcode_aac samples/small_360p25.mp4 out/o.m4a 2>&1 | grep -v Qavg
$E/transcode_aac $D/tagged.flac out/o.m4a 2>&1 | grep -v Qavg
```

```text
Expected one audio input stream, but found 2
Expected one audio input stream, but found 2
```

两种常见的输入都被拒绝了：视频文件（要取它的音轨），和带封面图的音乐文件（封面图是第二路流，一路“视频”）。
补丁改用 `av_find_best_stream()` 选出最好的一路音频，别的流读到了就丢掉。

**问题四（patch 0081）：不拷贝元数据。**

```bash
ffmpeg -v error -y -i $D/in.flac -metadata title="My Song" -metadata artist="Me" -c copy $D/tag2.flac
$E/transcode_aac $D/tag2.flac out/o2.m4a >/dev/null 2>&1
echo "原版：     $(ffprobe -v error -show_entries format_tags=title,artist -of compact out/o2.m4a)"
$B/transcode_aac $D/tag2.flac out/f2.m4a >/dev/null 2>&1
echo "修好以后： $(ffprobe -v error -show_entries format_tags=title,artist -of compact out/f2.m4a)"
```

```text
原版：     format
修好以后： format|tag:title=My Song|tag:artist=Me
```

原版转出来的文件没有标题、艺术家，在音乐库里就是一首无名的歌。补丁和 L04 的 `remux.c`、L16 的 `transcode.c` 一样：`av_dict_copy()` 拷贝容器和这一路流的元数据，再复制流的 `disposition`。

## 第 3 步：transcode_aac_av，做同样的事

```bash
$B/transcode_aac_av $MP2 out/t.m4a 2>/dev/null
cmp out/t.m4a out/ff.m4a && echo "与 ffmpeg 命令行的输出逐字节相同"
```

```text
输入：mp2，44100 Hz，stereo，s16p
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
解码：192 帧，221184 个采样，每帧 1152 到 1152 个
编码：216 帧，221184 个采样，最后一帧 1024 个
写出：217 个包，60989 字节
与 ffmpeg 命令行的输出逐字节相同
```

（编码器在关闭时会往标准错误打印一行 `Qavg: ...` 的统计，用 `2>/dev/null` 去掉了。）

`transcode_aac_av` 和官方示例的主要区别：

| | 官方示例 | `transcode_aac_av` |
| --- | --- | --- |
| 编码器、码率、声道数 | 写死：AAC、96 kbit/s、立体声 | 命令行参数 `acodec=`、`br=`、`ch=` |
| 采样率 | 必须等于输入，编码器不支持（比如 192 kHz）就失败 | 可以不同；编码器不支持时取它支持的最接近的一个 |
| 解码器的帧 | 每次调用取一帧 | 送一个包，用循环把所有的帧取完 |
| 转换结果 | 自己分配裸的指针数组 | 用 `AVFrame` 装，`swr_convert_frame()` 自己分配 |
| FIFO | 一律用 | 编码器要求固定帧长才用（FLAC、PCM 不用） |
| 创建输出文件 | 最先做，之后失败会留下 0 字节的文件 | 编码器和输出流都配好以后才做 |
| 出错 | 统一返回 `AVERROR_EXIT`（退出码 187），不管什么原因 | 返回 1，释放资源，不用 `exit()` |

读 `transcode_aac_av.c`。它的主要函数：

| 函数 | 做什么 |
| --- | --- |
| `open_input()` | 选一路音频，开解码器；声道布局未指定时先定下来（第 6 步） |
| `pick_sample_rate()` | 编码器支持哪几个采样率，取最接近的（第 5 步） |
| `open_output()` | 编码器、输出流、元数据；这时还没有创建文件 |
| `init_resampler()` | 建转换器 |
| `decode_packet()` | 送包给解码器，取出所有的帧 |
| `convert_store()` | 帧 → 转换器 → FIFO（或者直接编码），凑够一帧就送 |
| `fifo_encode()` | 从 FIFO 里一帧一帧取，送编码器 |
| `encode_write()` | 数 pts，编码，包换算时间基，写文件 |
| `transcode()` | 读包的循环，结束时从上游到下游依次冲刷 |

## 第 4 步：看 FIFO 怎么攒、怎么切

`trace=N` 把前 N 个解码帧经过转换、FIFO、编码器的情况打印出来：

```bash
$B/transcode_aac_av $MP2 out/t.m4a trace=10 2>/dev/null | head -12
```

```text
输入：mp2，44100 Hz，stereo，s16p
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
解码 #0   帧长  1152  转换后  1152  写进 FIFO 后共  1152  送编码器 1 帧（每帧 1024）  FIFO 剩   128
解码 #1   帧长  1152  转换后  1152  写进 FIFO 后共  1280  送编码器 1 帧（每帧 1024）  FIFO 剩   256
解码 #2   帧长  1152  转换后  1152  写进 FIFO 后共  1408  送编码器 1 帧（每帧 1024）  FIFO 剩   384
解码 #3   帧长  1152  转换后  1152  写进 FIFO 后共  1536  送编码器 1 帧（每帧 1024）  FIFO 剩   512
解码 #4   帧长  1152  转换后  1152  写进 FIFO 后共  1664  送编码器 1 帧（每帧 1024）  FIFO 剩   640
解码 #5   帧长  1152  转换后  1152  写进 FIFO 后共  1792  送编码器 1 帧（每帧 1024）  FIFO 剩   768
解码 #6   帧长  1152  转换后  1152  写进 FIFO 后共  1920  送编码器 1 帧（每帧 1024）  FIFO 剩   896
解码 #7   帧长  1152  转换后  1152  写进 FIFO 后共  2048  送编码器 2 帧（每帧 1024）  FIFO 剩     0
解码 #8   帧长  1152  转换后  1152  写进 FIFO 后共  1152  送编码器 1 帧（每帧 1024）  FIFO 剩   128
解码 #9   帧长  1152  转换后  1152  写进 FIFO 后共  1280  送编码器 1 帧（每帧 1024）  FIFO 剩   256
```

和“本讲要点”里的表一一对应：每轮 FIFO 里多剩 128 个，第 8 个解码帧（`#7`）以后 FIFO 里有 2048 个，一次送 2 帧，回到 0。

换一个解码帧长大的输入，每轮送得更多：

```bash
$B/transcode_aac_av $D/in.wav out/t2.m4a trace=2 2>/dev/null
```

```text
输入：pcm_s16le，44100 Hz，stereo，s16
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
解码 #0   帧长  4096  转换后  4096  写进 FIFO 后共  4096  送编码器 4 帧（每帧 1024）  FIFO 剩     0
解码 #1   帧长  4096  转换后  4096  写进 FIFO 后共  4096  送编码器 4 帧（每帧 1024）  FIFO 剩     0
解码：54 帧，221184 个采样，每帧 4096 到 4096 个
编码：216 帧，221184 个采样，最后一帧 1024 个
写出：217 个包，60989 字节
```

4096 刚好是 1024 的 4 倍，每轮 FIFO 都被取空。输入帧长不规则的，比如 Opus，开头的第一帧是 840，结尾的一帧是 865：

```bash
$B/transcode_aac_av $D/in.opus out/t3.m4a trace=300 2>/dev/null | (head -4; tail -6)
```

```text
输入：opus，48000 Hz，stereo，fltp
输出：aac，48000 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
解码 #0   帧长   840  转换后   840  写进 FIFO 后共   840  送编码器 0 帧（每帧 1024）  FIFO 剩   840
解码 #1   帧长   960  转换后   960  写进 FIFO 后共  1800  送编码器 1 帧（每帧 1024）  FIFO 剩   776
解码 #249 帧长   960  转换后   960  写进 FIFO 后共  1288  送编码器 1 帧（每帧 1024）  FIFO 剩   264
解码 #250 帧长   865  转换后   865  写进 FIFO 后共  1129  送编码器 1 帧（每帧 1024）  FIFO 剩   105
输入读完：FIFO 里剩 105 个采样，不够一帧（每帧 1024），作为最后一帧送编码器
解码：251 帧，240745 个采样，每帧 840 到 960 个
编码：236 帧，240745 个采样，最后一帧 105 个
写出：237 个包，60994 字节
```

- 第一轮只有 840 个采样，不够一帧，送了 0 帧：**一个解码帧不一定对应一次编码**。
- 输入读完时 FIFO 里剩 105 个采样，不够一帧，作为最后一帧送给编码器：编码器输出的总采样数（240745）等于解码器给的总数，一个也没丢。
- 官方示例一次只取一个解码帧、靠外层循环重复调用；`transcode_aac_av` 送一个包以后把解码器里所有的帧取完。大多数音频解码器一个包出一个帧，两种写法没有差别。

不用 FIFO 会怎么样？`fifo=0` 让转换出来的帧直接送编码器：

```bash
$B/transcode_aac_av $MP2 out/e.m4a fifo=0
echo "退出码 $?"
```

```text
[mp3 @ 0x559195af2640] Estimating duration from bitrate, this may be inaccurate
输入：mp2，44100 Hz，stereo，s16p
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，没有用 FIFO（fifo=0）
[aac @ 0x559195af5fc0] nb_samples (1152) > frame_size (1024)
送帧给编码器失败：Invalid argument
[aac @ 0x559195af5fc0] Qavg: nan  Tr: 0.0%  TNS(L): 0.0%  TNS(S): 0.0%  M/S: 0.0%  I/S: 0.0%  PNS: 0.0%
出错：Invalid argument
退出码 1
```

（地址每次运行不同。）编码器直接拒绝：1152 比它要的 1024 多。这是帧长不对时最常见的错误信息。
另一种是帧**比**要的短，而且不是最后一帧：`frame_size (1024) was not respected for a non-last frame`（练习 1）。
规则是：除了最后一帧，每一帧都必须正好是 `frame_size` 个采样。

## 第 5 步：采样率不同时，转换器会缓存采样

AAC 只支持 13 个采样率。输入是 192 kHz 的话，官方示例（采样率照输入）直接失败：

```bash
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:sample_rate=192000:duration=2" -c:a pcm_s16le $D/in_192k.wav
$B/transcode_aac $D/in_192k.wav out/o192.m4a 2>&1 | grep -v '^\[aac @ [0-9a-fx]*\]   [0-9]*$'
$B/transcode_aac $D/in_192k.wav out/o192.m4a >/dev/null 2>&1; echo "退出码 $?  输出文件 $(stat -c %s out/o192.m4a) 字节"
```

```text
[aac @ 0x55a1f572cd80] Specified sample rate 192000 is not supported by the aac encoder
[aac @ 0x55a1f572cd80] Supported sample rates:
Could not open output codec (error 'Invalid argument')
退出码 187  输出文件 0 字节
```

退出码 187 是 `AVERROR_EXIT`（一个负数）的低 8 位；输出文件已经被创建、清空了，留下 0 字节的垃圾（第 2 步说过，官方示例先创建文件、后配编码器）。
`transcode_aac_av` 先问编码器支持哪些采样率，取最接近的一个，再用转换器转过去：

```bash
$B/transcode_aac_av $D/in_192k.wav out/t.m4a trace=3 2>&1 | grep -v Qavg
ffmpeg -v error -y -i $D/in_192k.wav -c:a aac -b:a 96k -ac 2 out/ff.m4a
cmp out/t.m4a out/ff.m4a && echo "与 ffmpeg 命令行逐字节相同"
```

```text
编码器 aac 不支持 192000 Hz，改用最接近的 96000 Hz
输入：pcm_s16le，192000 Hz，mono，s16
输出：aac，96000 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
解码 #0   帧长 16384  转换后  8176  写进 FIFO 后共  8176  送编码器 7 帧（每帧 1024）  FIFO 剩  1008
解码 #1   帧长 16384  转换后  8192  写进 FIFO 后共  9200  送编码器 8 帧（每帧 1024）  FIFO 剩  1008
解码 #2   帧长 16384  转换后  8192  写进 FIFO 后共  9200  送编码器 8 帧（每帧 1024）  FIFO 剩  1008
输入读完：FIFO 里剩 512 个采样，不够一帧（每帧 1024），作为最后一帧送编码器
解码：24 帧，384000 个采样，每帧 7168 到 16384 个
转换：192000 个采样（采样率变了，采样数跟着变）
编码：188 帧，192000 个采样，最后一帧 512 个
写出：189 个包，23917 字节
与 ffmpeg 命令行逐字节相同
```

192 kHz 变成 96 kHz，采样数减半：16384 个采样转换后应该是 8192 个，但**第一帧只出来 8176 个，少了 16 个**。
转换器做插值需要看前后的采样，所以它总是留着 16 个在里面，等下一帧进来再一起吐出来。到输入结束时，最后的 16 个还在转换器里，
这时要用空输入调用 `swr_convert_frame(swr, out, NULL)`，把它们取出来：这就是“冲刷转换器”。程序最后统计的“转换：192000 个采样”正好是输入时长（2 秒）乘以 96000，一个也没少。
不冲刷会少 16 个采样，练习 2 可以验证。

“最接近”是怎么挑的？`pick_sample_rate()` 用 `avcodec_get_supported_config()` 取出编码器支持的采样率列表，选和想要的差最小的一个；
差一样大的时候选列表里靠前的（即较大的）：

```bash
$B/transcode_aac_av $MP2 out/t.m4a ar=50000 2>&1 | grep 不支持
$B/transcode_aac_av $MP2 out/t.m4a ar=56000 2>&1 | grep 不支持
$B/transcode_aac_av $MP2 out/t.ac3 acodec=ac3 ar=22050 2>&1 | grep 不支持
```

```text
编码器 aac 不支持 50000 Hz，改用最接近的 48000 Hz
编码器 aac 不支持 56000 Hz，改用最接近的 64000 Hz
编码器 ac3 不支持 22050 Hz，改用最接近的 32000 Hz
```

56000 到 48000 和 64000 一样远，选了 64000。`ffmpeg` 命令行在“输入的采样率编码器不支持”时也是取最接近的（上面的 192 kHz 例子两者逐字节相同），
但用户**明确**用 `-ar 56000` 要求一个不支持的值，命令行是拒绝的：

```bash
ffmpeg -v error -y -i $MP2 -c:a aac -ar 56000 out/ff56.m4a 2>&1 | head -2
```

```text
[aac @ 0x55ff4fd21300] Specified sample rate 56000 is not supported by the aac encoder
[aac @ 0x55ff4fd21300] Supported sample rates:
```

`transcode_aac_av` 对 `ar=` 的处理更宽松，这是故意的，方便做实验；自己写程序的时候要想好哪种更适合：悄悄改成别的值，用户可能不知道。

44.1 kHz 与 48 kHz 之间也一样：

```bash
$B/transcode_aac_av $D/in.opus out/t.m4a ar=44100 trace=3 2>&1 | grep -v Qavg
ffmpeg -v error -y -i $D/in.opus -c:a aac -b:a 96k -ac 2 -ar 44100 out/ff.m4a
cmp out/t.m4a out/ff.m4a && echo "与 ffmpeg 命令行逐字节相同"
```

```text
输入：opus，48000 Hz，stereo，fltp
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
解码 #0   帧长   840  转换后   756  写进 FIFO 后共   756  送编码器 0 帧（每帧 1024）  FIFO 剩   756
解码 #1   帧长   960  转换后   882  写进 FIFO 后共  1638  送编码器 1 帧（每帧 1024）  FIFO 剩   614
解码 #2   帧长   960  转换后   882  写进 FIFO 后共  1496  送编码器 1 帧（每帧 1024）  FIFO 剩   472
输入读完：FIFO 里剩 1 个采样，不够一帧（每帧 1024），作为最后一帧送编码器
解码：251 帧，240745 个采样，每帧 840 到 960 个
转换：221185 个采样（采样率变了，采样数跟着变）
编码：217 帧，221185 个采样，最后一帧 1 个
写出：218 个包，61034 字节
与 ffmpeg 命令行逐字节相同
```

840 × 44100 ÷ 48000 = 771.75，实际出来 756，又是少了 16 个。最后一帧只有 **1 个采样**，编码器照样编出一个包。

## 第 6 步：声道数，以及“声道布局未指定”的陷阱

```bash
ffmpeg -v error -y -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=2" -c:a pcm_s16le $D/in_mono.wav
ffmpeg -v error -y -f lavfi -i "anoisesrc=d=2:c=pink:r=44100:a=0.1,aformat=channel_layouts=5.1" -c:a pcm_s16le $D/in_51.wav
for f in in_mono in_51; do
    $B/transcode_aac_av $D/$f.wav out/t.m4a 2>&1 | grep -E "^输[入出]"
    ffmpeg -v error -y -i $D/$f.wav -c:a aac -b:a 96k -ac 2 out/ff.m4a
    cmp -s out/t.m4a out/ff.m4a && echo "  与命令行逐字节相同"
done
$B/transcode_aac_av $MP2 out/t.m4a ch=1 br=64000 2>&1 | grep -E "^输[入出]"
```

```text
输入：pcm_s16le，44100 Hz，mono，s16
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
  与命令行逐字节相同
输入：pcm_s16le，44100 Hz，5.1，s16
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
  与命令行逐字节相同
输入：mp2，44100 Hz，stereo，s16p
输出：aac，44100 Hz，mono，fltp，64000 bit/s，每帧必须 1024 个采样，用 FIFO 切
```

单声道变立体声（左右一样）、5.1 混成立体声、立体声混成单声道，都是转换器做的，输出和命令行相同。

再看一个容易踩的坑。`out/l17/in.wav` 是 `ffmpeg` 把 MP2 转成 WAV 得到的，看看它的声道布局：

```bash
ffprobe -v error -show_entries stream=channels,channel_layout -of compact $D/in.wav
```

```text
stream|channels=2|channel_layout=unknown
```

WAV 文件头里只写了“2 个声道”，没有写是左右（立体声）还是别的，所以布局是“未指定”。解码器照实报告，解码出来的每一帧上写的也是“未指定”。
可是 `swr_alloc_set_opts2()` + `swr_init()` 初始化转换器时，会把“未指定”当成这个声道数的默认布局（2 个声道就是立体声）。
以后 `swr_convert_frame()` 每次都要检查“这一帧的布局”和“转换器初始化时的布局”是不是一致，一比较发现不一样，报错：

```text
转换采样失败：Input changed
```

这个错误名字很迷惑：输入明明没有变。`transcode_aac_av` 在打开解码器之前就定下来：

```c
if (c->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
    av_channel_layout_default(&c->dec->ch_layout, c->dec->ch_layout.nb_channels);
```

`ffmpeg` 命令行也是这样处理的（`fftools/ffmpeg_demux.c` 里的 `guess_input_channel_layout()`，选项叫 `-guess_layout_max`）。练习 4 可以把这三行注释掉亲自看看。官方示例没有这个问题，因为它用的是 `swr_convert()`（给裸的指针数组，不带布局信息，没有检查）；
但裸指针数组本身也容易出错（比如搞错声道数），各有利弊。

## 第 7 步：换编码器、换容器

编码器：

```bash
for spec in "m4a aac 96000" "ac3 ac3 192000" "mp2 mp2 128000" "flac flac 96000" "wav pcm_s16le 96000"; do
    set -- $spec
    $B/transcode_aac_av $MP2 out/t.$1 acodec=$2 br=$3 2>&1 | grep -E "^输出：|^编码："
done
```

```text
输出：aac，44100 Hz，stereo，fltp，96000 bit/s，每帧必须 1024 个采样，用 FIFO 切
编码：216 帧，221184 个采样，最后一帧 1024 个
输出：ac3，44100 Hz，stereo，fltp，192000 bit/s，每帧必须 1536 个采样，用 FIFO 切
编码：144 帧，221184 个采样，最后一帧 1536 个
输出：mp2，44100 Hz，stereo，s16，128000 bit/s，每帧必须 1152 个采样，用 FIFO 切
编码：192 帧，221184 个采样，最后一帧 1152 个
输出：flac，44100 Hz，stereo，s16，96000 bit/s，每帧必须 4608 个采样，用 FIFO 切
编码：48 帧，221184 个采样，最后一帧 4608 个
输出：pcm_s16le，44100 Hz，stereo，s16，1411200 bit/s，帧长不限，不用 FIFO
编码：192 帧，221184 个采样，最后一帧 1152 个
```

- 每个编码器要的采样格式不同：AAC、AC-3 是 `fltp`，MP2、FLAC、PCM 是整数，转换器自动换。
- 每帧采样数不同：AAC 1024、AC-3 1536、MP2 1152、FLAC 4608。MP2 转 MP2，输入输出都是 1152，FIFO 每一轮都是满进满出（练习 1）。
- PCM 编码器的 `frame_size` 是 0，没有帧长的限制，输入给多少就编码多少，不用 FIFO，也就没有“切”：编码 192 帧，和解码的 192 帧一一对应。
- 码率参数对 FLAC（无损，码率随内容变）和 PCM（码率由采样格式决定）没有意义：FLAC 打印的是传进去的值，PCM 编码器把它改成了按采样算出来的 1411200。

怎么判断一个编码器要不要 FIFO？`frame_size` 大于 0 还不够：有的编码器带 `AV_CODEC_CAP_VARIABLE_FRAME_SIZE` 标志，表示每帧采样数可以不一样。
`open_output()` 里的判断是这样的：

```c
c->fixed = c->enc->frame_size > 0 && !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
```

要求固定帧长的才用 FIFO。（FLAC 编码器没有这个标志，所以上面 FLAC 也要 FIFO 切成 4608。）

容器：

```bash
for e in m4a mkv ts flv avi aac; do
    $B/transcode_aac_av $MP2 out/t.$e >/dev/null 2>&1
    ffmpeg -v error -y -i $MP2 -c:a aac -b:a 96k -ac 2 out/ff.$e
    cmp -s out/t.$e out/ff.$e && r=相同 || r=不同
    printf "%-4s %7d 字节  命令行 %7d 字节  %s  流时间基 %s\n" $e $(stat -c %s out/t.$e) $(stat -c %s out/ff.$e) $r "$(ffprobe -v error -select_streams a -show_entries stream=time_base -of csv=p=0 out/t.$e)"
done
```

```text
m4a    62656 字节  命令行   62656 字节  相同  流时间基 1/44100
mkv    63081 字节  命令行   63115 字节  不同  流时间基 1/1000
ts     75764 字节  命令行   75764 字节  相同  流时间基 1/90000
flv    64921 字节  命令行   64921 字节  相同  流时间基 1/1000
avi    71964 字节  命令行   71964 字节  相同  流时间基 256/11025
aac    62508 字节  命令行   62508 字节  相同  流时间基 1/28224000
```

输出是什么容器只看扩展名。流的时间基每个容器各不相同，所以 `encode_write()` 里要用 `av_packet_rescale_ts()` 换算（问题一）。有两处细节：

**TS 文件的大小。** 前面第 2 步用官方示例写 TS，是 100392 字节；`transcode_aac_av` 默认是 75764 字节，和命令行相同。差别来自复用器的 `max_delay`：

```bash
$B/transcode_aac_av $MP2 out/t0.ts muxdelay=0 >/dev/null 2>&1; echo "muxdelay=0: $(stat -c %s out/t0.ts) 字节"
```

```text
muxdelay=0: 100392 字节
```

库里 `AVFormatContext.max_delay` 的默认值是 0，`ffmpeg` 命令行默认设成 0.7 秒。MPEG-TS 把时间上靠得近的音频包装进同一个 PES 包，延迟是 0 的时候每个音频帧都单独装一个，
每个 PES 包后面还要补齐到 188 字节，文件就大了 32%。`transcode_aac_av` 默认跟命令行一样用 700 毫秒（`muxdelay=` 参数调整）。其他容器不受这个参数影响。

**MKV 的差别。** 音频包逐字节相同，只是命令行多写了一个流级别的 `ENCODER` 标签：

```bash
for f in out/t.mkv out/ff.mkv; do
    echo "$f: 包 $(ffmpeg -v error -i $f -map 0:a -c copy -f md5 -)  $(ffprobe -v error -show_entries stream_tags -of compact $f)"
done
```

```text
out/t.mkv: 包 MD5=1760cf0e35ac1bbdc691e87c786eefe2  stream|tag:DURATION=00:00:05.038000000
out/ff.mkv: 包 MD5=1760cf0e35ac1bbdc691e87c786eefe2  stream|tag:ENCODER=Lavc63.28.100 aac|tag:DURATION=00:00:05.038000000
```

命令行会把编码器的名字和版本写进输出流的元数据。想要一样，在 `open_output()` 里设置 `ost->metadata` 的 `encoder` 就行了；这里没有必要。

**FLAC 的差别。** 文件大小不同，解码出来的 PCM 却相同：

```bash
$B/transcode_aac_av $MP2 out/t.flac acodec=flac >/dev/null 2>&1
ffmpeg -v error -y -i $MP2 -c:a flac -ac 2 out/ff.flac
for f in out/t.flac out/ff.flac; do
    echo "$f $(stat -c %s $f) 字节 包: $(ffprobe -v error -show_entries packet=duration -of csv=p=0 $f | sort | uniq -c | tr '\n' ' ') PCM: $(ffmpeg -v error -i $f -f s16le - | md5sum | cut -c1-8)"
done
```

```text
out/t.flac 155181 字节 包:      48 4608  PCM: 89566644
out/ff.flac 168173 字节 包:     192 1152  PCM: 89566644
```

我们的 FLAC 用编码器默认的块大小 4608，命令行把第一帧的采样数（1152，MP2 解码出来的）当成块大小。FLAC 的块越大压缩得越好，
所以我们的文件更小。两个文件解码出来的 PCM 逐字节相同，音质没有区别。

## 第 8 步：时间戳

**预热包。** 看输出文件的第一个和最后一个包：

```bash
ffprobe -v error -show_packets -select_streams a -show_entries packet=pts,duration,flags,side_data_type -of csv=p=0 out/t.m4a | sed -n '1,2p;$p'
```

```text
-1024,1024,KD_,Skip Samples,1024,0,0,0
0,1024,K__
220160,1024,K__
```

第一个包的 pts 是 **-1024**，标志里有 `D`（discard），附带一个 “Skip Samples 1024” 的边数据。AAC 编码器有 1024 个采样的延迟，
这个包只是预热，播放器看到 pts 是负数就跳过。文件的时长不包含它：`stream=duration` 是 5.015510，和 5.0155 秒的输入一样。
这是 MP4 的“编辑列表”做的事（L04 讲过），所以包数比帧数多 1（217 对 216）。

**自己数 pts 和命令行跟着输入走，有什么区别。** 大多数输入两者逐字节相同。有几个输入，两者的输出不同：

```bash
hist() { ffprobe -v error -select_streams a -show_entries packet=pts -of csv=p=0 "$1" |
    awk 'NR>1{d=$1-p; c[d]++} {p=$1} END{for(k in c) printf "%s×%s ", k, c[k]}'; echo; }
for f in in.ac3 in.wma; do
    $B/transcode_aac_av $D/$f out/t_$f.m4a >/dev/null 2>&1
    ffmpeg -v error -y -i $D/$f -c:a aac -b:a 96k -ac 2 out/c_$f.m4a
    echo "== $f"
    echo "  本程序  开始 $(ffprobe -v error -show_entries stream=start_time -of csv=p=0 out/t_$f.m4a)  相邻包 pts 之差：$(hist out/t_$f.m4a)"
    echo "  命令行  开始 $(ffprobe -v error -show_entries stream=start_time -of csv=p=0 out/c_$f.m4a)  相邻包 pts 之差：$(hist out/c_$f.m4a)"
    echo "  包数据 md5：$(ffmpeg -v error -i out/t_$f.m4a -map 0:a -c copy -f md5 -) / $(ffmpeg -v error -i out/c_$f.m4a -map 0:a -c copy -f md5 -)"
done
```

```text
== in.ac3
  本程序  开始 0.000000  相邻包 pts 之差：1024×218 
  命令行  开始 0.000000  相邻包 pts 之差：1024×169 1023×49 
  包数据 md5：MD5=0ec134674f934014a2aa45129ac25955 / MD5=0ec134674f934014a2aa45129ac25955
== in.wma
  本程序  开始 0.000000  相邻包 pts 之差：1024×214 
  命令行  开始 0.068776  相邻包 pts 之差：1024×209 1025×1 1026×1 1029×2 1030×1 
  包数据 md5：MD5=def30663fc5f8209333d183ab7ea5dd0 / MD5=def30663fc5f8209333d183ab7ea5dd0
```

每个输入的音频包数据都逐字节相同，只是时间戳不同：

- **AC-3 裸流**（`.ac3`）的时间基是 1/90000，每个 1536 采样的帧精确的时长是 3134.69 个单位，但解复用器每帧只加 3134，每帧少 0.69，
  整个文件累计少了约 49 个采样。命令行跟着输入的时间戳走，49 个间隔变成 1023；我们数采样，每个间隔都是 1024。
- **WMA** 的解码器有延迟：解出来的第一帧时间戳是 92 毫秒而不是 0（正好是第三个包的时间戳，这个解码器比输入晚两个包出帧），
  命令行就照着放，输出的音频从 0.0688 秒开始（92 毫秒减去 AAC 编码器自己的 1024 采样预热）；我们总是从 0 开始。

这不是谁对谁错：要和视频同步的时候（L16），音频应该跟着输入的时间戳走；只想要“一段干净、连续的音频”的时候（音乐库、播客），自己数更好。
本程序和官方示例都是后一种，所以不要拿它们的输出和一路视频合并，时间对不上。

输入是 FLV（时间基 1/1000）也一样：同一个 FLV 视频文件里的音频，`ffmpeg` 命令行输出的相邻包间隔是 `1024×215 1017×1 1022×1`，我们是 `1024×217`，包数据相同。

## 第 9 步：和命令行比一比

`compare_cli.sh` 在 37 个组合上把 `transcode_aac_av` 和 `ffmpeg` 命令行转码的结果逐字节比较：

```bash
L17_transcode_aac/compare_cli.sh | tail -9
```

```text
== 不同的参数
相同  ch1
相同  ar48000
相同  ar22050
相同  coder
相同  faststart

逐字节相同 32 个，不同 5 个： ac3 wma video_flv out_mkv enc_flac
```

比较的内容是：13 种输入（MP2、WAV、FLAC、Vorbis、Opus、AC-3、WMA、ALAC、AAC、单声道、5.1、带封面和标签的 FLAC、一个视频加两路音频的 MKV）、
4 种视频容器里的音轨（MP4、MKV、TS、FLV）、5 个编码器不支持的采样率（192000、100000、88200、11025、4000）、6 种输出容器（MP4、MKV、TS、FLV、AVI、ADTS）、
4 个编码器（AC-3、MP2、FLAC、PCM）、5 个参数组合（单声道和 64 kbit/s、转到 48000 和 22050 Hz、`a:aac_coder=fast`、`movflags=+faststart`）。

5 个不同的，原因前面都看过，**音频数据**是逐包相同的：

| 组合 | 不同在哪 | 看哪一步 |
| --- | --- | --- |
| `ac3`、`wma`、`video_flv` | 输入的时间戳不精确，命令行跟着走，我们自己数 | 第 8 步 |
| `out_mkv` | 命令行多写了一个流级别的 `ENCODER` 标签 | 第 7 步 |
| `enc_flac` | FLAC 块大小不同，解码出来的 PCM 相同 | 第 7 步 |

另外前面看到的几个例子也说明：多路流的输入（`multi_mkv`、带封面的 FLAC、视频文件）、带标签的输入、声道数和采样率的转换，输出都和命令行相同。

## 第 10 步：出错会怎样

```bash
e() { rm -f out/e.m4a; "$@" 2>&1 >/dev/null | grep -v '^\[aac\|^\[mpeg\|^\[mp3' | tr '\n' '|'; echo; }
e $B/transcode_aac_av /nonexistent.mp2 out/e.m4a
e $B/transcode_aac_av samples/video_360p25.m1v out/e.m4a
e $B/transcode_aac_av $MP2 out/e.xyz
e $B/transcode_aac_av $MP2 out/e.m4a acodec=nope
e $B/transcode_aac_av $MP2 out/e.mp3 acodec=aac
e $B/transcode_aac_av $MP2 out/e.m4a a:nosuchopt=1
e $B/transcode_aac_av $MP2 out/e.m4a br=abc
e $B/transcode_aac_av $MP2 out/e.m4a fifo=0
printf "fifo=0 后输出文件: "; ls -l out/e.m4a | awk '{print $5, "字节"}'
e $B/transcode_aac_av $MP2 /dev/full format=mp4
e $B/transcode_aac_av $MP2 /dev/full format=wav acodec=pcm_s16le
e $B/transcode_aac_av $MP2 out/e.m4a nosuchmux=1
printf "未知复用器选项后输出文件: "; ls -l out/e.m4a | awk '{print $5, "字节"}'
```

```text
打不开输入文件 /nonexistent.mp2：No such file or directory|出错：No such file or directory|
输入里没有音频流，或者没有它的解码器：Stream not found|出错：Stream not found|
[AVFormatContext @ 0x5600a3457b80] Unable to choose an output format for 'out/e.xyz'; use a standard extension for the filename or specify the format manually.|不知道输出文件 out/e.xyz 是什么格式，请用 format= 指定|出错：Invalid argument|
找不到音频编码器 nope|出错：Encoder not found|
输出格式 mp3 放不下 aac 编码的音频|出错：Invalid argument|
编码器不认识选项 nosuchopt|出错：Invalid argument|
br= 的值 abc 不对，应该是 1000 到 2147483647 之间的整数|出错：Invalid argument|
送帧给编码器失败：Invalid argument|出错：Invalid argument|
fifo=0 后输出文件: 258 字节
写文件头失败：No space left on device|关闭输出文件失败：No space left on device|出错：No space left on device|
写包失败：No space left on device|关闭输出文件失败：No space left on device|出错：No space left on device|
复用器不认识选项 nosuchmux|出错：Invalid argument|
未知复用器选项后输出文件: 258 字节
```

- 每一步的返回值都检查，出错时打印哪一步、什么原因，释放资源，返回 1，不用 `exit()`。
- 输入打不开、没有音频、输出格式不认识、编码器不存在、编码器参数不对：全都在**创建输出文件之前**就失败了，不会留下任何文件。
- 文件头写出去以后才失败的（`fifo=0`、`nosuchmux`、磁盘满），输出文件里是已经转好的部分：程序在出错以后也会写文件尾（`ffmpeg` 命令行也是这样），所以文件是有效的 MP4，只是没有内容（258 字节）。
  复用器选项是否被认识要等 `avformat_write_header()` 返回才知道，所以 `nosuchmux` 这种错误只能在文件头写完以后发现。
- `/dev/full` 永远写不进去，错误可以出现在写文件头、写包、关闭文件的任何一处。**关闭文件时才真正写出缓冲区里剩下的数据**，所以 `avio_closep()` 的返回值也要检查。
  官方示例没有检查。
- 官方示例对输入打不开的处理：

  ```bash
  $B/transcode_aac /nonexistent.mp2 out/e.m4a 2>&1 | head -3; echo "退出码 ${PIPESTATUS[0]}"
  ```

  ```text
  Could not open input file '/nonexistent.mp2' (error 'No such file or directory')
  退出码 187
  ```

  退出码永远是 187（`AVERROR_EXIT`），不管是什么错，调用脚本没法区分。`transcode_aac_av` 统一返回 1。

## 常见错误

1. **解码器的帧长和编码器的不一样，却直接送。** 编码器报 `nb_samples (1152) > frame_size (1024)`，或者 `frame_size (1024) was not respected for a non-last frame`。
   中间要有 FIFO 或者滤镜图的 `set_frame_size`。例外：编码器带 `AV_CODEC_CAP_VARIABLE_FRAME_SIZE` 标志，或者 `frame_size` 是 0（第 7 步）。
2. **没有冲刷。** 输入结束时，解码器、转换器、FIFO、编码器四个地方都可能还存着采样，要从上游到下游依次冲刷，漏掉哪一个，输出就悄悄少一截，程序不报错
   （练习 2、3；原版的 patch 0079 就是漏了解码器的冲刷）。
3. **FIFO 里的零头丢掉。** 输入读完以后，FIFO 里不足一帧的采样也要送给编码器，不然最后几百个采样就没了（第 4 步：Opus 的 105 个）。
4. **写包之前不换算时间基。** 复用器选的流时间基和编码器的不一定相同，MKV 变成 222 秒、AVI 变成 5159 秒（问题一）。和 L15 说的是同一类错误。
5. **采样率转换不知道要冲刷，也不知道输出的采样数会变。** 转换器会缓存 16 个采样；输出的采样数不等于输入的，用输入的数量去取输出会越界或者丢数据（第 5 步）。
6. **采样率编码器不支持，不处理。** AAC 只有 13 个固定值，192 kHz、4 kHz 的输入直接失败；要先问 `avcodec_get_supported_config()`，取一个最接近的（第 5 步）。
7. **声道布局“未指定”。** WAV、WMA 常见，`swr_convert_frame()` 报 `Input changed`（第 6 步）。
8. **先创建输出文件，再配编码器。** 编码器配置失败时文件已经被清空，留下 0 字节的垃圾（第 5 步，官方示例的做法）。
9. **输入不止一路流就拒绝，或者读到别的流的包也去解码。** 视频文件里的音轨、带封面的音乐文件都不止一路流，要用 `av_find_best_stream()` 选，别的流的包丢掉（问题三）。
10. **不拷贝元数据。** 转出来的文件没有标题、艺术家（问题四）。

## 练习

1. 对 `$D/in.ogg`（Vorbis）、`$D/in.wav`、`$D/in.opus`、`$D/in.ac3`、`$D/in.wma`，分别运行 `transcode_aac_av` 加 `fifo=0`，哪些成功、哪些失败？失败的是什么错误？
   再用 `acodec=mp2 fifo=0` 转 `$MP2`，成功吗？再加上 `trace=3`，FIFO 在每一轮之后是什么样子？
2. 在 `transcode_aac_av.c` 的 `transcode()` 里，把冲刷转换器的那一句 `ret = convert_store(c, NULL);` 连同后面的 `if (ret < 0) return ret;` 注释掉，
   转码 `$D/in_192k.wav` 和 `$D/in.opus`（加 `ar=44100`），看“转换：N 个采样”的数字，各比原来少多少？
3. 把冲刷解码器的那一句 `ret = decode_packet(c, NULL);` 连同后面的 `if (ret < 0) return ret;` 注释掉，分别转码 `$D/in.wma`、`$D/in.opus`、`$D/in.ac3`、`$D/in.ogg`、`$D/in.flac`，
   “解码：N 帧”有没有变化？变化的是哪个？
4. 把 `open_input()` 里“声道布局未指定”那三行注释掉，转码 `$D/in.wav`，会怎样？转码 `$MP2` 呢？为什么？
5. `ar=56000` 选了 64000 而不是 48000，因为一样近的时候取列表靠前的。把 `pick_sample_rate()` 里的 `<` 改成 `<=`，再试一次，选了什么？
   `ar=` 的取值如果要改成和命令行一样（不支持就报错），该改哪里？

<details>
<summary>参考答案</summary>

1. 成功的是 `in.ogg`：Vorbis 每帧正好 1024 个采样，和 AAC 一样，碰巧可以直接送，输出和有 FIFO 的版本完全相同。
   `in.wav` 失败：`nb_samples (4096) > frame_size (1024)`；`in.ac3` 失败：`nb_samples (1536) > frame_size (1024)`；
   `in.wma` 失败：`nb_samples (2048) > frame_size (1024)`；`in.opus` 失败：`frame_size (1024) was not respected for a non-last frame`，
   因为它的第一帧是 840，不足 1024 又不是最后一帧。
   `acodec=mp2 fifo=0` 成功：输入和编码器都是 1152。加上 `trace=3`，每一行都是“送编码器 1 帧（每帧 1152）  FIFO 剩 0”，
   FIFO 在每一轮之后都被取空：对这种输入，有没有 FIFO 都一样。
2. 192 kHz 的输入：“转换”从 192000 变成 191984，少了 16 个采样；`in.opus`：从 221185 变成 221168，少了 17 个
   （16 个缓存的加上取整的一个）。输出的音频就是这样悄悄变短了一点点，听不出来，但是和命令行不再逐字节相同。
3. 只有 `in.wma` 变了：“解码”从 107 帧、219136 个采样变成 106 帧、217088 个采样，少了一整帧 2048 个采样。
   其余四个没变：它们的解码器收到文件尾以后没有缓存的帧（对 Opus，虽然它带 `AV_CODEC_CAP_DELAY` 标志，这个文件里没有帧留在里面）。
   这就是为什么这个错误不容易发现：很多输入没有问题，换一个解码器才暴露（patch 0079）。
4. `$D/in.wav` 报 `转换采样失败：Input changed`，退出码 1：WAV 文件的声道布局是“未指定”，转换器初始化时把它变成了立体声，
   帧上仍然写着“未指定”，两者不一致。`$MP2` 不受影响：MP2 解码器给出的布局本来就是立体声。
5. 改成 `<=` 以后，一样近的时候取列表里后面的（较小的），`ar=56000` 选 48000。
   要报错的话，在 `open_output()` 里 `pick_sample_rate()` 返回的值和用户给的值不一样时，打印错误并返回 `AVERROR(EINVAL)`，
   不要继续（`o->ar` 为 0，即用户没有指定的时候，输入的采样率编码器不支持，还是取最接近的，和命令行一致）。

</details>

## 下一讲

L18 开始第六阶段，换一个方向：前面所有的程序都是从**文件**里读数据，用 `avformat_open_input()` 给一个文件名。
官方示例 `avio_read_callback.c` 展示的是另一种用法：数据不在文件里，在内存里、网络上、数据库里，
这时要自己提供读数据的回调函数，让 libavformat 通过它读，这就是**自定义 I/O**（`AVIOContext`）。
这个示例只提供了读回调，没有提供 seek 回调：`moov` 在文件末尾的 MP4 在这种情况下读不到完整的流信息，下一讲会看到问题，也会给出补上 seek 回调的做法。
