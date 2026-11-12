# L16 完整转码

前面十几讲把一条流水线拆开，一段一段做过：L03 读文件，L05、L06 解码，L11、L12 滤镜，L13、L14 编码，L15 封装。
这一讲把它们接起来：读一个媒体文件，解码，过滤镜，重新编码，写成另一个文件，这就是**转码**。
例子是官方示例 `transcode.c`，另外我们自己写一个更完整的 `transcode_av`。

完成标志：`transcode_av` 的输出与 `ffmpeg` 命令行逐字节相同（MP4、FLV、TS、MKV 输入都试过，流拷贝裁剪、缩放加音频拷贝也一样）；
能说清楚时间戳从读包到写包一共换算了几次、每次的单位是什么；
知道编码器的参数为什么要问滤镜图的出口；
能解释精确裁剪和流拷贝裁剪的区别，知道什么时候该用流拷贝、什么时候必须重新编码。

## 本讲要点

**转码的数据流**

```text
输入文件 ─► 解复用 ─► 包 ─► 解码器 ─► 帧 ─► 滤镜图 ─► 帧 ─► 编码器 ─► 包 ─► 复用器 ─► 输出文件
              │                                                            ▲
              └─────────────────── 流拷贝：包原样拷贝 ─────────────────────┘
```

视频和音频各走一条，互不相干，只在最后的复用器里汇合（L15 的交织）。每一段都已经讲过，这一讲的新东西是接口处的细节：

| 接口 | 要做的事 | 讲过的地方 |
| --- | --- | --- |
| 解复用 → 解码 | `pkt_timebase` 告诉解码器包的时间基；失败要停下来 | L07 |
| 解码 → 滤镜 | 帧的 pts 用 `best_effort_timestamp`；滤镜入口的参数照着解码器 | L12 |
| 滤镜 → 编码 | 时间戳用出口的时间基换算；清掉帧类型；时长一起换 | L12、L13 |
| 编码 → 复用 | 包的时间戳从编码器时间基换到流时间基 | L15 |
| 结束 | 从上游到下游依次冲刷：解码器、滤镜图、编码器；再写文件尾 | L11、L13、L15 |

**时间戳一路换算**

同一帧画面，从读进来到写出去，时间戳要经过好几次换算，每一次的单位都不一样。下面是 `small_360p25.mp4` 里第 2 帧画面（序号 1，第 1 帧是序号 0）经过的每一站：

| 环节 | 单位（时间基） | 这一帧的 pts | 换算由谁做 |
| --- | --- | --- | --- |
| 读包、解码出帧 | 输入流的时间基 1/12800 | 512 | 解复用器；解码器靠 `pkt_timebase` 沿用 |
| 滤镜图出口 | 出口的时间基，`av_buffersink_get_time_base()` | 512（滤镜不改时间基时和输入相同） | 滤镜（`fps`、`setpts` 会改） |
| 编码器输入 | 编码器时间基 1/25，视频是帧序号 | 1 | 我们：`av_rescale_q()` |
| 编码器输出的包 | 编码器时间基 1/25 | pts 1，dts 1（这个包是第 4 个出来的：有 B 帧，编码顺序和显示顺序不同，见第 3 步） | 编码器 |
| 写进文件 | 输出流时间基 1/12800 | pts 512，dts 512 | 我们：`av_packet_rescale_ts()` |

音频更特别：容器的时间基很粗（FLV、MKV 是毫秒）的时候，音频帧的 pts 是取整过的，直接换成 1/44100 以后会抖，要先整理（第 4 步）。

**流拷贝和重新编码怎么取舍**

| | 流拷贝（不解码、不编码） | 重新编码 |
| --- | --- | --- |
| 速度 | 极快，只是搬运数据 | 慢，编码最耗时 |
| 画质 | 没有损失，每个包和原文件一样 | 有损失，每编一次损失一次 |
| 裁剪 | 只能从关键帧开始 | 可以精确到帧 |
| 滤镜（缩放、叠加、变速……） | 不能用 | 可以 |
| 改编码格式、码率 | 不能 | 可以 |
| 目标容器 | 必须放得下这种编码（H.264 放进 MP4、MKV 都行，放进 WAV 不行） | 随便 |

经验：只想换容器、拼接、粗略裁剪，用流拷贝；要改画面、改编码、精确裁剪，必须重新编码。常见的折中是**只重新编码需要改的那一路**，另一路拷贝（比如缩小画面但不动声音）。

**编码器的参数从哪来**

官方示例的做法是“先打开编码器，参数抄解码器的”，滤镜图的出口再照着编码器设置。输入是什么尺寸，输出就是什么尺寸，滤镜只能做不改画面参数的事。
`transcode_av` 反过来：先配好滤镜图，出口只告诉滤镜图“编码器接受这些格式”，配置完成以后向出口问“你实际给我的是什么尺寸、像素格式、帧率”，
再用这些值打开编码器。这样 `scale`、`fps`、`format` 这些改变画面参数的滤镜都能用。

**精确裁剪**

流拷贝只能从关键帧开始（L04）。要精确到帧，只能解码，把想要的那一段之前的帧丢掉。做法分三步：

1. **定位**：`avformat_seek_file()` 跳到起点之前最近的关键帧，不用从头解码；
2. **丢弃**：滤镜图最前面放 `trim` 和 `setpts`（音频是 `atrim` 和 `asetpts`），只留下 `[start, end)` 里的帧，再把时间戳减去 `start`；
3. **停止**：结束时间过了以后不再读包。

代价是起点之前的那一小段（最多一个 GOP）也要解码。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L16_transcode                  # 可执行文件在 L16_transcode/build/ 下
B=L16_transcode/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `transcode` | 官方示例（加了课程注释，打了 11 个 patch）：每路流用同样的编码器重新编码 |
| `transcode_av` | 自己写的：编码器按输出格式选，滤镜、裁剪、流拷贝都可以用，输出与 `ffmpeg` 命令行相同 |

## 第 1 步：官方示例 transcode.c

```bash
$B/transcode samples/small_360p25.mp4 out/official.mp4 > out/official.log 2>&1; echo "退出码 $?"
wc -l < out/official.log
grep -c 'Pushing decoded frame to filters' out/official.log
ffprobe -v error -count_frames -show_entries stream=codec_name,nb_read_frames,duration -of csv=p=0 out/official.mp4
```

```text
退出码 0
1426
343
h264,5.000000,125
aac,5.000000,216
```

示例把每路流都解码一遍，过一个“什么都不做”的滤镜，再用同样的编码器重新编码。输入是 H.264 和 AAC，输出也是。
日志很长：每一帧打印三行（送进滤镜图、从滤镜图取出、编码）。输出的 125 帧视频和 216 帧音频和输入一样多。

> 这里用的是打过补丁的版本。提交 `a337e8d` 的原版一运行就失败：`Cannot set output pixel format`，因为它设置的几个 buffersink 选项已经不存在了
> （`upstream_patches/` 的 0001）。L02 说“L16 会给出修正后的版本”，就是这一份。读 `transcode.c` 的注释，凡是出过问题的地方都标了 patch 编号。

读 `transcode.c`，注意这几处：

- `open_input_file()`：给**每一路**流开一个解码器；`stream_ctx[i]`、`filter_ctx[i]` 的下标 `i` 都是输入流的序号，输出文件的第 `i` 路流和它一一对应。
- `open_output_file()`：编码器是 `avcodec_find_encoder(dec_ctx->codec_id)`，输入是什么编码输出就是什么；参数全部抄解码器的。
- `encode_write_frame()`：两次时间戳换算都在这里，一次是帧，一次是包。
- `main()`：读包的循环和结束时的冲刷。

官方示例把输入里的元数据、语言和默认标志带到输出，还把色彩属性交给了编码器。用一个带 BT.709 色彩属性、标题和语言标签的小视频验证：

```bash
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25 -t 1 \
    -vf setparams=colorspace=bt709:color_primaries=bt709:color_trc=bt709:range=tv \
    -c:v libx264 -pix_fmt yuv420p -metadata title="My Title" -metadata:s:v:0 language=chi out/bt709.mp4
$B/transcode out/bt709.mp4 out/bt709_o.mp4 > /dev/null 2>&1
for f in out/bt709.mp4 out/bt709_o.mp4; do
    ffprobe -v error -select_streams v -show_entries stream=color_range,color_space,color_transfer,color_primaries:stream_tags=language:format_tags=title \
        -of default=nw=1:nk=0 $f | tr '\n' ' '; echo
done
```

```text
color_range=tv color_space=bt709 color_transfer=bt709 color_primaries=bt709 TAG:language=chi TAG:title=My Title 
color_range=tv color_space=bt709 color_transfer=bt709 color_primaries=bt709 TAG:language=chi TAG:title=My Title 
```

原版没有这两样：输出的四个色彩属性全部是 `unknown`，标题和语言也没了（`upstream_patches/` 的 0074、0075）。
颜色信息丢了，播放器只能猜：高清视频当成标清的 BT.601 来显示，颜色会偏。

### 官方示例的局限

这个示例的滤镜字符串是写死的 `"null"`（视频）和 `"anull"`（音频）。想让转码的同时缩小画面，改成 `"scale=320:-2"` 试试：

```bash
sed 's/filter_spec = "null"/filter_spec = "scale=320:-2"/' L16_transcode/transcode.c > out/transcode_scale.c
gcc -std=c11 -g $(pkg-config --cflags libavfilter libavformat libavcodec libavutil) out/transcode_scale.c \
    -o out/transcode_scale $(pkg-config --libs libavfilter libavformat libavcodec libavutil)
out/transcode_scale samples/small_360p25.mp4 out/scale_off.mp4 2>&1 | grep -v 'Pushing\|Pulling\|Encoding' | tail -3
```

```text
[libx264 @ 0x557ab6ef9040] Frame size or pixel format does not match the encoder
[aac @ 0x557ab7116d00] Qavg: nan  Tr: 0.0%  TNS(L): 0.0%  TNS(S): 0.0%  M/S: 0.0%  I/S: 0.0%  PNS: 0.0%
Error occurred: Invalid argument
```

（地址每次运行不同。）原因：编码器是在建滤镜图之前打开的，宽高已经按 640×360 定死了，滤镜图却吐出 320×180 的帧。
这就是前面“编码器的参数从哪来”说的区别：官方示例只能用不改画面参数的滤镜。

## 第 2 步：transcode_av，做同样的事

```bash
$B/transcode_av samples/small_360p25.mp4 out/t.mp4 2>/dev/null
ffmpeg -v error -y -i samples/small_360p25.mp4 -threads 1 out/ff.mp4
cmp out/t.mp4 out/ff.mp4 && echo "与 ffmpeg 命令行的输出逐字节相同"
```

```text
输出格式：mp4（MP4 (MPEG-4 Part 14)）
视频滤镜图：null
音频滤镜图：anull
视频：h264 → libx264，输入流时间基 1/12800，编码器时间基 1/25，输出流时间基 1/12800
音频：aac → aac，输入流时间基 1/44100，编码器时间基 1/44100，输出流时间基 1/44100
视频：解码 125 帧，滤镜输出 125 帧，写出 125 个包，445659 字节
音频：解码 216 帧，滤镜输出 216 帧，写出 217 个包，76191 字节
与 ffmpeg 命令行的输出逐字节相同
```

`-threads 1` 是让命令行也用单线程编码，和我们的程序一致（L13 说过：多线程时每台机器的结果可能不同）。
几处值得看：

- 音频解码 216 帧，写出 217 个包：AAC 编码器有 1024 个采样的延迟（L14），冲刷时多吐出一个包。
- 视频编码器时间基 1/25，文件里的流时间基是 1/12800：复用器把它改了（L15）。
- 官方示例（修好以后）输出的每个包的 pts、dts、时长也与命令行完全相同，只是编码线程数不同，文件的字节不同。

读 `transcode_av.c`。它的主要函数：

| 函数 | 做什么 |
| --- | --- |
| `init_filter()` | 搭一路流的滤镜图：入口（照着解码器）、`trim`、用户的滤镜、出口（告诉它编码器接受的格式） |
| `open_encoder()` | 配置好以后向出口问尺寸、像素格式、帧率，用它们打开编码器 |
| `decode_packet()` | 包 → 解码器 → 帧，每一帧交给 `filter_encode_write()` |
| `filter_encode_write()` | 帧 → 滤镜图 → 出口取帧 → `encode_write()` |
| `encode_write()` | 帧换算时间基 → 编码器 → 包换算时间基 → 复用器 |
| `copy_packet()` | 流拷贝：换算时间基以后直接写出去 |
| `flush_stream()` | 结束时从上游到下游依次冲刷 |

## 第 3 步：看时间戳一路怎么变

`trace=N` 把前 N 帧、N 个包在每个环节的时间戳打印出来：

```bash
$B/transcode_av samples/small_360p25.mp4 out/t.mp4 trace=3 2>/dev/null | grep '^视频'
```

```text
视频：h264 → libx264，输入流时间基 1/12800，编码器时间基 1/25，输出流时间基 1/12800
视频 解码输出 #0   pts        0  时长   512  时间基 1/12800  = 0.000000 秒
视频 滤镜出口 #0   pts        0  时长   512  时间基 1/12800  = 0.000000 秒
视频 编码输入 #0   pts        0  时长     1  时间基 1/25  = 0.000000 秒
视频 解码输出 #1   pts      512  时长   512  时间基 1/12800  = 0.040000 秒
视频 滤镜出口 #1   pts      512  时长   512  时间基 1/12800  = 0.040000 秒
视频 编码输入 #1   pts        1  时长     1  时间基 1/25  = 0.040000 秒
视频 解码输出 #2   pts     1024  时长   512  时间基 1/12800  = 0.080000 秒
视频 滤镜出口 #2   pts     1024  时长   512  时间基 1/12800  = 0.080000 秒
视频 编码输入 #2   pts        2  时长     1  时间基 1/25  = 0.080000 秒
视频 编码输出 #0   pts        0  dts       -2  时间基 1/25  →  流 pts        0  dts    -1024  时间基 1/12800
视频 编码输出 #1   pts        4  dts       -1  时间基 1/25  →  流 pts     2048  dts     -512  时间基 1/12800
视频 编码输出 #2   pts        2  dts        0  时间基 1/25  →  流 pts     1024  dts        0  时间基 1/12800
```

（实际输出中音频的行穿插在视频的行之间，这里用 `grep` 只留下视频。）逐行看：

- **解码输出 → 滤镜出口**：滤镜是 `null`，什么都没改，数字和时间基都相同。如果用了 `fps=10` 或者 `setpts`，这两行就不一样了（第 5 步）。
- **滤镜出口 → 编码输入**：pts 从 `512`（1/12800）变成 `1`（1/25），时长从 `512` 变成 `1`：每一帧占一个时间基单位。
  时间戳和时长都要换；只换时间戳、忘了时长，MP4 最后一帧会丢（练习 2）。
- **编码输出**：第 0 个包是第 0 帧，pts 0；第 1 个包的 pts 是 4，是第 4 帧，不是第 1 帧。因为有 B 帧，编码顺序和显示顺序不同（L08），
  第 0 个包的 dts 是 -2 而不是 0：解码器要先收到 2 帧才能开始输出（L15 讲过）。
- **→ 流**：`av_packet_rescale_ts()` 把 1/25 换成 1/12800（乘 512），pts、dts 一起换。第 0 个包的 `dts = -2` 变成 `-1024`。

## 第 4 步：容器的时间基很粗时，音频时间戳会抖

FLV 和 MKV 的时间基是毫秒，TS 是 1/90000。毫秒容器里的音频帧 pts 都是取整过的。用 FLV 样本看看：

```bash
$B/transcode_av samples/small_360p25.flv out/f.mp4 trace=5 2>/dev/null | grep '^音频 \(解码输出\|整理时间戳\)'
```

```text
音频 解码输出 #0   pts       57  时长    23  时间基 1/1000  = 0.057000 秒
音频 整理时间戳 #0   pts     2514  时长  1024  时间基 1/44100  = 0.057007 秒
音频 解码输出 #1   pts       80  时长    23  时间基 1/1000  = 0.080000 秒
音频 整理时间戳 #1   pts     3538  时长  1024  时间基 1/44100  = 0.080227 秒
音频 解码输出 #2   pts      103  时长    23  时间基 1/1000  = 0.103000 秒
音频 整理时间戳 #2   pts     4562  时长  1024  时间基 1/44100  = 0.103447 秒
音频 解码输出 #3   pts      126  时长    23  时间基 1/1000  = 0.126000 秒
音频 整理时间戳 #3   pts     5579  时长  1024  时间基 1/44100  = 0.126508 秒
音频 解码输出 #4   pts      150  时长    23  时间基 1/1000  = 0.150000 秒
音频 整理时间戳 #4   pts     6603  时长  1024  时间基 1/44100  = 0.149728 秒
```

1024 个采样在 44100 Hz 下是 23.22 毫秒，容器里存的是 57、80、103、126、150：相邻差 23、23、23、24。
如果用普通的换算（`av_rescale_q`），相邻两帧在 1/44100 下的间隔就是 1014、1015、1058……，而不是 1024：编码出来的每个音频包的时间戳也不均匀。
`fix_audio_pts()` 用 `av_rescale_delta()` 利用“每帧正好 1024 个采样”这个已知条件，把取整的误差接平：整理后相邻帧的间隔基本是 1024（上面的第 3 帧到第 4 帧差 1017，
是因为它不能偏离毫秒取整的范围太远）。看整个文件，把输出里相邻音频包的 pts 之差数一数：

```bash
hist() { ffprobe -v error -select_streams a -show_entries packet=pts -of csv=p=0 "$1" |
    awk 'NR>1{d=$1-p; c[d]++} {p=$1} END{for(k in c) printf "%s×%s ", k, c[k]}'; echo; }
$B/transcode_av samples/small_360p25.flv out/f.mp4 >/dev/null 2>&1; hist out/f.mp4
$B/transcode samples/small_360p25.flv out/f_off.mp4 >/dev/null 2>&1; hist out/f_off.mp4
ffmpeg -v error -y -i samples/small_360p25.flv -threads 1 out/f_ff.mp4; hist out/f_ff.mp4
cmp out/f.mp4 out/f_ff.mp4 && echo "与 ffmpeg 命令行逐字节相同"
```

```text
1024×215 1017×1 1022×1 
1024×215 1017×1 1022×1 
1024×215 1017×1 1022×1 
与 ffmpeg 命令行逐字节相同
```

三个程序的结果完全一样：215 个间隔是 1024，有两处（1017 和 1022）是输入文件本身的小偏差。官方示例的原版没有这一步，间隔是
`1014×120 1015×49 1058×27 1059×20 1024×1`（`upstream_patches/` 的 0077，练习 3 可以自己试）。输入是 MP4 时，音频时间基本来就是 1/44100，这一步什么都不改。

用 TS、MKV 输入也一样，输出都与命令行逐字节相同：

```bash
for f in small_360p25.ts small_360p25.mkv small_360p25.flv; do
    $B/transcode_av samples/$f out/x.mp4 >/dev/null 2>&1
    ffmpeg -v error -y -i samples/$f -threads 1 out/x_ff.mp4
    cmp -s out/x.mp4 out/x_ff.mp4 && echo "$f 相同" || echo "$f 不同"
done
```

```text
small_360p25.ts 相同
small_360p25.mkv 相同
small_360p25.flv 相同
```

## 第 5 步：编码器的参数问滤镜图出口

在滤镜图里改画面参数，编码器跟着变：

```bash
$B/transcode_av samples/small_360p25.mp4 out/fps10.mp4 vf=fps=10 2>/dev/null | grep '^视频：'
ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames,r_frame_rate,duration -of compact out/fps10.mp4
$B/transcode_av samples/small_360p25.mp4 out/small.mp4 vf=scale=320:-2 2>/dev/null > /dev/null
ffprobe -v error -select_streams v -show_entries stream=width,height -of compact out/small.mp4
$B/transcode_av samples/small_360p25.mp4 out/y444.mp4 vf=format=yuv444p 2>/dev/null > /dev/null
ffprobe -v error -select_streams v -show_entries stream=pix_fmt,profile -of compact out/y444.mp4
```

```text
视频：h264 → libx264，输入流时间基 1/12800，编码器时间基 1/10，输出流时间基 1/10240
视频：解码 125 帧，滤镜输出 50 帧，写出 50 个包，253724 字节
stream|r_frame_rate=10/1|duration=5.000000|nb_read_frames=50
stream|width=320|height=180
stream|profile=High 4:4:4 Predictive|pix_fmt=yuv444p
```

- `fps=10`：滤镜图每 2.5 帧留一帧，出口的帧率变成 10，编码器时间基跟着变成 1/10；125 帧变成 50 帧，时长仍是 5 秒。
- `scale=320:-2`：宽 320，高按比例算并取偶数，出口告诉我们是 320×180，编码器就按这个打开。官方示例做不到。
- `format=yuv444p`：用户要什么像素格式，只要编码器支持，就用什么。`transcode_av` 没有取编码器列表的第一项，而是把整个列表交给出口，
  由滤镜图挑一个最接近输入的，转换最少。这里挑了用户指定的 `yuv444p`；不指定时就是输入本身的 `yuv420p`，不用转换。

## 第 6 步：精确裁剪，和流拷贝裁剪

流拷贝裁剪的起点只能在关键帧上（L04）。先做一个每 25 帧（1 秒）一个关键帧的素材：

```bash
ffmpeg -v error -y -i samples/small_360p25.mp4 -c:v libx264 -g 25 -keyint_min 25 -sc_threshold 0 -threads 1 -c:a copy out/gop25.mp4
ffprobe -v error -select_streams v -show_entries packet=pts_time,flags -of csv=p=0 out/gop25.mp4 | grep K | tr '\n' ' '; echo
```

```text
0.000000,K__ 1.000000,K__ 2.000000,K__ 3.000000,K__ 4.000000,K__ 
```

从 2.32 秒开始，截 1.4 秒：精确裁剪（解码再编码）和流拷贝裁剪各做一份，和 `ffmpeg` 命令行比较：

```bash
$B/transcode_av out/gop25.mp4 out/cut_exact.mp4 ss=2.32 t=1.4 2>/dev/null | grep '滤镜图\|^视频：解码\|^音频：解码'
$B/transcode_av out/gop25.mp4 out/cut_copy.mp4 copy=va ss=2.32 t=1.4 2>/dev/null | grep '拷贝 '
for f in out/cut_exact.mp4 out/cut_copy.mp4; do
    echo "$f $(stat -c %s $f) 字节"
    ffprobe -v error -count_frames -show_entries stream=codec_type,start_time,duration,nb_read_frames -of compact $f
done
ffmpeg -v error -y -ss 2.32 -t 1.4 -i out/gop25.mp4 -threads 1 out/cut_ff.mp4
cmp out/cut_exact.mp4 out/cut_ff.mp4 && echo "精确裁剪：与命令行逐字节相同"
ffmpeg -v error -y -ss 2.32 -t 1.4 -i out/gop25.mp4 -c copy out/cut_ffcopy.mp4
cmp out/cut_copy.mp4 out/cut_ffcopy.mp4 && echo "流拷贝裁剪：与命令行逐字节相同"
```

```text
视频滤镜图：trim=start=2.320000:end=3.720000,setpts=PTS-2.320000/TB
音频滤镜图：atrim=start=2.320000:end=3.720000,asetpts=PTS-2.320000/TB
视频：解码 45 帧，滤镜输出 35 帧，写出 35 个包，131983 字节
音频：解码 78 帧，滤镜输出 61 帧，写出 62 个包，18205 字节
视频：拷贝 45 个包，179315 字节
音频：拷贝 79 个包，29366 字节
out/cut_exact.mp4 152778 字节
stream|codec_type=video|start_time=0.000000|duration=1.400000|nb_read_frames=35
stream|codec_type=audio|start_time=0.000000|duration=1.400000|nb_read_frames=61
out/cut_copy.mp4 211610 字节
stream|codec_type=video|start_time=0.000000|duration=1.480000|nb_read_frames=37
stream|codec_type=audio|start_time=0.000000|duration=1.418413|nb_read_frames=62
精确裁剪：与命令行逐字节相同
流拷贝裁剪：与命令行逐字节相同
```

- **精确裁剪**：视频刚好 35 帧、1.4 秒（`start_time` 是 0 说明第一帧就是 2.32 秒那一帧）。代价是解码了 45 帧，多出来的 10 帧里，8 帧在起点之前（从 2 秒处的关键帧开始解码，到 2.32 秒才是要的），2 帧在终点之后（解码器的延迟），解出来都被 `trim` 丢掉。
  滤镜图是 `trim` 加 `setpts`：留下 `[2.32, 3.72)` 里的帧，时间戳减去 2.32。
- **流拷贝裁剪**：45 个视频包都写进了文件，因为起点之前最近的关键帧在 2 秒处，从那里开始才能解码。
  2.00 到 2.32 秒之间的那 8 帧被**编辑列表**遮住：时间戳是负数，播放器看不到，但数据在文件里。所以文件反而比精确裁剪的大（211,610 对 152,778 字节）。
  播放时看到的是 37 帧、1.48 秒：终点是按包的 `dts` 判断的，最后几个包的 `pts` 可以超过终点，所以比要求的 1.4 秒多一点。
- 音频包都是“关键”的，可以在任意包上开始，但流拷贝仍然是整包整包地取，精度是一个包（23 毫秒）。

结束时间的处理：`t=` 规定结束时刻以后，只要读到一个包的 `dts` 已经过了，这一路流就不用再读了（`dts` 单调增加，后面的包解出来的帧都在结束时间之外）。
判断用 `dts` 而不是 `pts`：视频有 B 帧时包的 `pts` 不单调，用 `pts` 可能丢掉后面还要引用的包。

## 第 7 步：什么时候用流拷贝

对一个 10 秒、1080p 的视频（`samples/demo_1080p30.mp4`）：

```bash
for m in "" "copy=a" "copy=v" "copy=va"; do
    s=$(date +%s.%N); $B/transcode_av samples/demo_1080p30.mp4 out/tm.mp4 $m >/dev/null 2>&1; e=$(date +%s.%N)
    printf "[%-8s] %5.2f 秒  %9d 字节\n" "$m" $(echo "$e - $s" | bc) $(stat -c %s out/tm.mp4)
done
```

```text
[        ] 15.65 秒    7422554 字节
[copy=a  ] 14.86 秒    7422562 字节
[copy=v  ]  0.21 秒    7833981 字节
[copy=va ]  0.03 秒    7833989 字节
```

（耗时因机器而异，这台机器上编码用单线程，看相对的比例。）

- 整个重新编码花了 15.65 秒。音频只占很少的一点：只拷贝音频（`copy=a`）才省了不到 1 秒。
- 视频拷贝（`copy=v`）：0.21 秒，剩下的是音频编码的时间。视频是大头，**只要不改视频，就别重新编码视频**。
- 两路都拷贝：0.03 秒。文件大小与原文件相同（7,833,989 字节），每个包的 pts、dts、时长、大小、标志与原文件逐个相同
  （`ffprobe -show_entries packet=... | md5sum` 两边一样）。

重新编码有损失。把第 2 步的输出和原文件逐帧比较（PSNR 越高越接近，无穷大是完全相同）：

```bash
ffmpeg -v info -y -i out/t.mp4 -i samples/small_360p25.mp4 -lavfi psnr -f null - 2>&1 | grep 'PSNR' | tail -1
```

```text
[Parsed_psnr_0 @ 0x7fea94001c80] PSNR y:45.738172 u:44.708823 v:45.096365 average:45.439790 min:43.452903 max:50.934096
```

45 dB 左右，肉眼基本看不出来，但已经不是原来的画面了；每转码一次就再损失一次。画质要求高的时候可以降低 CRF（编码器选项 `v:crf=18`，
L13 讲过），代价是文件变大：同一个样本加上 `v:crf=18` 以后 PSNR 是 49.26 dB，文件从 527,420 字节变成 707,551 字节（`ffmpeg -crf 18` 的结果与它逐字节相同）。

同样，**滤镜图的那一路不能流拷贝**：

```bash
$B/transcode_av samples/small_360p25.mp4 out/e.mp4 copy=v vf=scale=320:-2 2>&1 | head -2
```

```text
流拷贝（copy=）的那一路不能用滤镜，请去掉 vf= 或 af=，或者不要拷贝这一路
出错：Invalid argument
```

## 第 8 步：出错会怎样

```bash
e() { "$@" 2>&1 >/dev/null | grep -v '^\[\|^  \|^Input\|^Output\|^Stream\|^$' | tr '\n' '|'; echo; }
e $B/transcode_av /nonexistent.mp4 out/e.mp4
e $B/transcode_av samples/small_360p25.mp4 out/e.xyz
e $B/transcode_av samples/small_360p25.mp4 out/e.mp4 vcodec=nope
e $B/transcode_av samples/small_360p25.mp4 out/e.mp4 vf=nosuchfilter
e $B/transcode_av samples/small_360p25.mp4 out/e.mp4 af=volume=abc
e $B/transcode_av samples/small_360p25.mp4 out/e.mp4 v:nosuchopt=1
e $B/transcode_av samples/small_360p25.mp4 out/e.mp4 ss=100
e $B/transcode_av samples/small_360p25.mp4 /dev/full format=matroska
$B/transcode_av samples/small_360p25.mp4 out/e.gif 2>/dev/null | grep '丢掉'
```

```text
打不开输入文件 /nonexistent.mp4：No such file or directory|出错：No such file or directory|
不知道输出文件 out/e.xyz 是什么格式，请用 format= 指定|出错：Invalid argument|
找不到视频编码器 nope|出错：Encoder not found|
解析视频滤镜图“nosuchfilter”失败：Filter not found|出错：Filter not found|
解析音频滤镜图“volume=abc”失败：Invalid argument|出错：Invalid argument|
视频编码器不认识选项 nosuchopt|出错：Invalid argument|
视频一个包也没有写出来，ss= 是不是超过了输入的长度，或者 t= 太短？|音频一个包也没有写出来，ss= 是不是超过了输入的长度，或者 t= 太短？|出错：Invalid argument|
写文件尾失败：No space left on device|关闭输出文件失败：No space left on device|出错：No space left on device|
输出格式 gif 不能放音频，丢掉输入里的音频
```

- 每一步的返回值都检查，出错时打印哪一步、什么原因，释放资源，返回 1，不用 `exit()`。
- **输出格式放不下的那一路流直接丢掉**（GIF 没有声音，WAV 没有画面），并且告诉用户；`ffmpeg` 命令行也是这样。
- **`ss=` 超过文件长度**：不报错的话会得到一个有文件头、没有内容的“空壳”文件，所以最后检查每一路都写出了包。
- **`/dev/full`** 永远写不进去：错误出现在写文件尾和关闭文件。官方示例原来这两步的结果都不检查，照常返回 0（patch 0072）。
- 官方示例对输入打不开的处理：

  ```bash
  $B/transcode /nonexistent.mp4 out/e.mp4 2>&1 | tail -2
  ```

  ```text
  Cannot open input file
  Error occurred: No such file or directory
  ```

  原版在这里段错误：清理代码访问还没创建的结构（patch 0071）。

## 常见错误

1. **送给编码器的帧时间戳用错了时间基。** 帧要从滤镜图出口的时间基换到编码器的时间基，用 `av_buffersink_get_time_base()` 问出口，
   不能直接用输入流的时间基：滤镜（`fps`、`setpts`）会改它（第 3、5 步）。和 L15 的“忘了换算包的时间基”是同一类错误，只是多了一层。
2. **帧没有时长，或者没设 `AV_CODEC_FLAG_FRAME_DURATION`。** 编码器输出的视频包没有时长，MP4 的最后一帧时长是 0，落在编辑列表之外，播放时丢掉（练习 2，官方示例的 patch 0011）。
3. **解码出来的帧原样交给编码器，没有清 `pict_type`。** 输入里的 I 帧都会被编成关键帧，输出的关键帧间隔被输入带偏（练习 1）。
4. **输入的属性没有带到输出。** 色彩属性没有交给编码器，输出的 `color_range` 等全是 `unknown`，播放器要猜（patch 0074）；
   容器和流的元数据、`default` 等标志没有拷贝，标题、语言丢了，播放器选不对默认音轨（patch 0075）。
5. **冲刷漏了一步，或者顺序反了。** 要从上游到下游依次冲刷：解码器、滤镜图、编码器。漏掉哪一步，那一步里缓存的帧就丢了，输出少几帧，程序不报错（练习 4）。
6. **取帧只取一次。** 一帧进去，滤镜图和编码器可能吐出 0 个、1 个或多个，要用循环取到 `EAGAIN` 或 `EOF` 为止；
   这两个返回值是“现在没有了”和“全部结束了”，不是错误，不能当错误返回。
7. **音频帧长和编码器要求的不符。** AAC 要求每帧正好 1024 个采样（最后一帧可以短）。滤镜图出口要用 `av_buffersink_set_frame_size()` 设成这个长度。
   输入本来就是 1024 一帧的时候看不出问题，一旦加上 `af=aresample=48000`，每帧变成 1098 个采样，编码器就报
   `nb_samples (1098) > frame_size (1024)`，送帧失败。
8. **音频时间戳不整理。** 毫秒时间基的容器（FLV、MKV）输入，音频包的时间戳相邻差 1014、1015、1058 而不是 1024（第 4 步，练习 3，官方示例的 patch 0077）。
9. **不检查解码失败、写文件尾和关闭文件的结果。** 官方示例原版：解码失败只打印一句就收尾返回 0，输出被悄悄截断（patch 0073）；
   写文件尾、关闭文件失败照常返回 0，留下一个打不开的文件（patch 0072）；输入打不开时清理代码直接崩溃（patch 0071）。
10. **裁剪用错了办法。** 要精确到帧只能解码再编码，用流拷贝起点只能落在关键帧上（第 6 步）；
    流拷贝和滤镜不能同时用，否则滤镜被悄悄忽略；起点和终点都不要超过文件长度（第 8 步）。

## 练习

1. 在 `transcode_av.c` 里把 `s->fframe->pict_type = AV_PICTURE_TYPE_NONE;` 注释掉，重新编译。先做一个每 5 帧一个关键帧的输入：
   `ffmpeg -v error -y -i samples/small_360p25.mp4 -c:v libx264 -g 5 -keyint_min 5 -sc_threshold 0 -threads 1 -c:a copy out/gop5.mp4`，
   转码前后，输出里各有多少个关键帧？（数关键帧：`ffprobe -v error -select_streams v -show_entries packet=flags -of csv=p=0 文件 | grep -c K`）做完把代码改回去。
2. 在 `open_encoder()` 里注释掉 `enc->flags |= AV_CODEC_FLAG_FRAME_DURATION;`，转码 `samples/small_360p25.mp4`，
   用 `ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames,duration -of csv=p=0` 看输出。
   再把这一句改回来，把 `encode_write()` 里 `frame->duration = 1;` 改成 `frame->duration = 0;`，结果一样吗？说明什么？
3. 把 `fix_audio_pts()` 里的 `av_rescale_delta(...)` 一句换成 `frame->pts = av_rescale_q(frame->pts, s->ist->time_base, sr_tb);`，
   转码 `samples/small_360p25.flv`，用第 4 步的 `hist` 看相邻音频包的间隔。再转码 `samples/small_360p25.mp4`，输出和原来比有区别吗？
4. 分别注释掉 `flush_stream()` 里的三步（冲刷解码器、冲刷滤镜图、冲刷编码器），每次只注释一步，转码 `samples/small_360p25.mp4`，
   用 `ffprobe -v error -count_frames -show_entries stream=codec_type,nb_read_frames -of csv=p=0` 看视频和音频各剩几帧。每一步丢的是什么帧？
5. 把定位那一段（`if (ss > 0) {` 开头的整个 `avformat_seek_file()` 调用）去掉，对 `samples/demo_1080p30.mp4` 运行 `ss=9 t=1`，
   和去掉之前比，输出文件有区别吗？程序打印的“解码 N 帧”变成了多少？

<details>
<summary>参考答案</summary>

1. 输入有 25 个关键帧。正常转码输出只有 1 个关键帧（编码器自己决定，默认每 250 帧一个）；注释掉以后输出有 25 个。
   解码器把 I 帧的类型留在帧上，编码器看到“这是 I 帧”就照做，输入的每个关键帧都变成输出的关键帧，输出的关键帧间隔被输入带偏。
   这个行为对 libx264 成立；其他编码器可能忽略这个字段。
2. 注释掉标志：视频时长 4.960000 秒，`nb_read_frames` 是 124，最后一帧丢了（正常是 5.000000 和 125）。
   标志留着、把帧时长改成 0：结果完全一样，也是 4.960000 和 124。说明两个条件缺一不可：
   帧上要有时长，编码器还要被告知“请把它带到包上”。官方示例的 patch 0011 就是补了这两处。
3. FLV：相邻间隔变成 `1014×120 1015×49 1058×27 1059×20 1024×1`，和官方示例原版一样，不再是 `1024×215 1017×1 1022×1`。
   MP4：输出与原来逐字节相同，因为音频时间基本来就是 1/44100，两个函数的结果一样。
4. 注释掉冲刷解码器：视频 123 帧、音频 216 帧。解码器为了 B 帧和多线程解码有几帧的延迟，不送空包它们就出不来，丢了 2 帧视频。
   注释掉冲刷滤镜图：视频 125 帧、音频 215 帧，音频时长 4.992290 秒，少了 340 个采样。滤镜图出口按 1024 个采样切帧，
   最后不足 1024 的那一点要等输入结束的信号才会放出来；视频用 `null` 滤镜没有缓存，所以没有区别，换成 `vf=fps=25` 才会少 1 帧（124）。
   注释掉冲刷编码器：视频只剩 84 帧（时长 3.36 秒）、音频 213 帧。编码器的前瞻和 B 帧缓存了 41 帧，不冲刷就全部丢了。
   三种情况程序都正常退出，没有任何提示。
5. 输出文件逐字节相同（`cmp` 无输出）。有定位时视频解码 50 帧（从 8.33 秒处的关键帧到 10 秒），音频 81 帧；
   去掉以后视频解码 300 帧，音频 469 帧，全部从头解码，然后由 `trim` 把前面丢掉。这台机器上耗时从 1.65 秒变成 2.81 秒。
   素材越长，差别越大。

</details>

## 下一讲

L17 做音频转码：官方示例 `transcode_aac.c`，把音频转成 AAC。这一讲的 `transcode_av` 里，音频的采样数匹配是靠滤镜图出口的
`av_buffersink_set_frame_size()` 做的；`transcode_aac.c` 不用滤镜图，自己用 L14 讲过的 `AVAudioFifo` 把解码出来的帧攒起来，再切成编码器要的长度，
还要用 `libswresample` 转换格式。结构可以套用 L14。
