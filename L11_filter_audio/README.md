# L11 滤镜图基础

L09 的缩放和 L10 的重采样，都是“拿到一帧，处理一下，得到一帧”。这样的处理还有很多：调音量、裁剪画面、叠加文字、变速、混音……
FFmpeg 把它们都做成了**滤镜**（filter），放在 libavfilter 库里。几个滤镜接起来就是**滤镜图**（filter graph）。
命令行里的 `-vf scale=640:360`、`-af volume=0.5` 用的就是它。

这一讲用最简单的一条链学清楚滤镜图的基本概念和使用步骤：手工搭一张“调音量”的图，把解码出来的声音送进去，把处理好的取出来。

完成标志：`volume_file` 的输出与 `ffmpeg -af volume=0.5` 的结果逐字节相同；
能说出建图的六个步骤；能解释图里为什么会多出一个名叫 `auto_aresample_0` 的滤镜。

## 本讲要点

**几个名词**

| 名词 | 意思 | 对应的类型 |
| --- | --- | --- |
| 滤镜 | 一种处理，例如 volume（调音量）。全局只有一份定义，按名字查找 | `AVFilter` |
| 滤镜实例 | 某张图里的一个滤镜，带着自己的参数。同一个滤镜可以在一张图里建多个实例 | `AVFilterContext` |
| pad | 滤镜的输入口或输出口。volume 有一进一出；混音滤镜 amix 有多个输入 | `AVFilterPad` |
| 连接 | 上游滤镜的一个输出口接到下游滤镜的一个输入口，帧沿着它流动 | `AVFilterLink` |
| 滤镜图 | 装着所有实例和连接的容器 | `AVFilterGraph` |

`AVFilter` 和 `AVFilterContext` 的关系，与 `AVCodec` 和 `AVCodecContext`（L05）的关系一样：前者是“有这么一种东西”，后者是“我正在用的这一个”。

**入口和出口**

程序和滤镜图之间靠两个特殊的滤镜交接：

| | 音频 | 视频 | 程序怎么用 |
| --- | --- | --- | --- |
| 入口（没有输入口） | `abuffer` | `buffer` | `av_buffersrc_add_frame()` 把帧送进去 |
| 出口（没有输出口） | `abuffersink` | `buffersink` | `av_buffersink_get_frame()` 把帧取出来 |

入口不会去看帧，建它的时候必须明确告诉它帧的参数（音频是采样格式、采样率、声道布局、时间基）。

**建图的六个步骤**

```text
1. avfilter_graph_alloc()                      分配图
2. avfilter_get_by_name("volume")              按名字找滤镜
3. avfilter_graph_alloc_filter()               在图里建一个实例
4. avfilter_init_str() / avfilter_init_dict()  给参数并初始化
     （第 3、4 步可以合成一个函数：avfilter_graph_create_filter()）
   每个滤镜重复第 2 到 4 步
5. avfilter_link()                             把实例一个个接起来
6. avfilter_graph_config()                     配置整张图
```

**给滤镜传参数的三种方式**（官方示例各用了一次）

| 方式 | 写法 |
| --- | --- |
| 逐个设置 | `av_opt_set(ctx, "sample_fmt", "fltp", AV_OPT_SEARCH_CHILDREN)`，然后 `avfilter_init_str(ctx, NULL)` |
| 字典 | `av_dict_set(&dict, "volume", "0.90", 0)`，然后 `avfilter_init_dict(ctx, &dict)` |
| 字符串 | `avfilter_init_str(ctx, "sample_fmts=s16:sample_rates=44100")`，和命令行的写法相同，最常用 |

**格式协商**

每个滤镜能处理的格式不一样：volume 默认只做浮点运算，aformat 则是专门用来“限定格式”的。
`avfilter_graph_config()` 会让每条连接两端的滤镜商量出一个双方都接受的格式；
商量不出来的地方，它**自动插入转换滤镜**：音频插 `aresample`（就是 L10 的 libswresample），视频插 `scale`（就是 L09 的 libswscale）。
所以用了滤镜图之后，格式转换通常不用自己调用 libswscale 和 libswresample，在图的末尾用 aformat（视频用 format）说明想要什么就行。

**送帧、取帧、冲刷**

和解码器的 send/receive（L05）是同一个模式：

```c
av_buffersrc_add_frame(src, frame);                       /* 送一帧 */
while ((ret = av_buffersink_get_frame(sink, out)) >= 0)   /* 取到取不出来为止 */
    ...                                                   /* AVERROR(EAGAIN)：要再送输入 */

av_buffersrc_add_frame(src, NULL);                        /* 输入结束：送 NULL */
while ((ret = av_buffersink_get_frame(sink, out)) >= 0)   /* 再取，直到 AVERROR_EOF */
    ...
```

送一帧进去，可能出来 0 帧、1 帧或多帧。输入结束时一定要送 `NULL` 并把剩下的取完，这就是滤镜图的冲刷。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L11_filter_audio               # 可执行文件在 L11_filter_audio/build/ 下
B=L11_filter_audio/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `filter_info` | 统计有哪些滤镜；查看一个滤镜的输入、输出和选项 |
| `filter_audio` | 官方示例（加了课程注释）：自己生成正弦波，经过 volume 和 aformat，打印每帧输出的 MD5 |
| `volume_file` | 解码任意文件里的音频，经过滤镜图调整音量，写成 16 位 WAV |

## 第 1 步：看看有哪些滤镜

```bash
$B/filter_info
```

```text
这个 FFmpeg 里有 491 个滤镜
  第一个输出是音频的 136 个，是视频的 333 个
  没有输入的（源）37 个，没有输出的（汇）4 个
```

（数量随 FFmpeg 的版本和编译选项变化。）看其中一个：

```bash
$B/filter_info volume
```

```text
滤镜 volume：Change input volume.
输入：1 个
  #0  default    audio
输出：1 个
  #0  default    audio
选项：
  volume         字符串 set volume adjustment expression
  precision      整数   select mathematical precision
      可取 fixed        select 8-bit fixed-point
      可取 float        select 32-bit floating-point
      可取 double       select 64-bit floating-point
  eval           整数   specify when to evaluate expressions
  ……
```

volume 有一个音频输入、一个音频输出。它的选项里最重要的是 `volume`（音量，可以写倍数或分贝），
`precision` 决定用什么精度计算，默认是 `float`，第 4 步会用到。

再看入口滤镜和一个多输入的滤镜：

```bash
$B/filter_info abuffer | head -8
$B/filter_info amix | head -3
```

```text
滤镜 abuffer：Buffer audio frames, and make them accessible to the filterchain.
输入：0 个
输出：1 个
  #0  default    audio
选项：
  time_base      分数   
  sample_rate    整数   
  sample_fmt     采样格式 
滤镜 amix：Audio mixing.
输入：0 个（个数不固定，由参数决定，初始化时才建立）
输出：1 个
```

abuffer 没有输入口，它的数据来自程序。amix 的输入个数由 `inputs` 参数决定。

命令行里对应的是 `ffmpeg -filters`（列出全部）和 `ffmpeg -h filter=volume`（看一个）。以后要用一个没用过的滤镜，先这样查它的选项。

## 第 2 步：官方示例 filter_audio.c

```bash
$B/filter_audio 1 | head -4
$B/filter_audio 1 | grep -c plane
```

```text
plane 0: 0x988FF097992D33E0450DC742B67E215F

plane 0: 0xF80E3B6A0796BB24E43D802CD053AAF9

46
```

参数 1 是时长（秒）。它生成 46 帧正弦波（5.0 声道、48000 Hz、fltp，每帧 1024 个采样），送进这样一张图：

```text
abuffer -> volume（音量 0.9）-> aformat（要求 s16、44100 Hz、立体声）-> abuffersink
```

每从出口取到一帧，就打印它的 MD5。输出只有一个 plane，因为出口给的是 s16，packed 格式只有一个平面（L06）。

读代码时找 `课程注释`。`init_filter_graph()` 就是按“本讲要点”里的六个步骤写的，四个滤镜各重复一遍第 2 到 4 步，
并且故意用了三种不同的方式传参数。主循环是：

```c
err = av_buffersrc_add_frame(src, frame);                    /* 送进入口 */
while ((err = av_buffersink_get_frame(sink, frame)) >= 0) {  /* 从出口取 */
    process_output(md5, frame);
    av_frame_unref(frame);
}
```

注意送和取用的是同一个 `frame`：`av_buffersrc_add_frame()` 会把帧里的数据拿走，返回之后 `frame` 是空的，可以马上拿去接输出。

输入和 aformat 要求的三样（采样格式、采样率、声道布局）都不一样，代码里却没有任何重采样的调用。
转换是 `avfilter_graph_config()` 自动插入的 aresample 做的，第 3 步能亲眼看到它。

这个示例有 6 处问题，已做成 patch（见 `upstream_patches/` 的 0042 到 0047）。和学习最相关的一处：
**它结束时没有冲刷滤镜图**，自动插入的重采样滤镜里留着的最后 16 个采样没有输出，和 L10 官方示例是同一个毛病。
其余的是：建图失败时不释放图、错误信息不完整，字符串缓冲区的类型不对（编译有警告），生成的正弦波在帧边界不连续，分配的 MD5 上下文实际没用上。

## 第 3 步：volume_file，把音量减半

```bash
$B/volume_file samples/small_360p25.mp4 out/half.wav 0.5
```

```text
入口 abuffer 的参数：time_base=1/44100:sample_rate=44100:sample_fmt=fltp:channel_layout=stereo
图里的连接（上游 -> 下游：这条连接上的格式）：
  src -> vol：fltp，44100 Hz，stereo
  vol -> auto_aresample_0：fltp，44100 Hz，stereo
  auto_aresample_0 -> fmt：s16，44100 Hz，stereo
  fmt -> sink：s16，44100 Hz，stereo
出口 abuffersink 的格式：s16，44100 Hz，2 声道
送进 220500 个采样，取出 220500 个采样，其中冲刷时取出 0 个
写出 out/half.wav：44 字节文件头 + 882000 字节数据，时长 5.000 秒
```

程序里建了四个滤镜实例，起名 `src`、`vol`、`fmt`、`sink`。配置之后顺着连接走一遍，图里是**五个**：
`vol` 和 `fmt` 之间多了一个 `auto_aresample_0`。

原因就是格式协商。`vol` 输出 fltp，`fmt`（aformat）只接受 s16，这条连接两端谈不拢，
libavfilter 就在中间插入一个 aresample 把 fltp 转成 s16。名字以 `auto_` 开头的都是自动插入的。

和官方示例相比，`volume_file.c` 有三处不同，都是实际程序需要的：

**图在拿到第一帧之后才建。** 入口滤镜的参数要和送进去的帧一致，最可靠的来源就是第一帧本身：

```c
snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
         time_base.num, time_base.den, frame->sample_rate,
         av_get_sample_fmt_name(frame->format), layout);
ret = create_filter(&c->src, c->graph, "abuffer", "src", args);
```

**建滤镜只用一个函数。** `create_filter()` 里调用的是 `avfilter_graph_create_filter()`，
它把“建实例、给参数（字符串形式）、初始化”合在一起，四个滤镜各一行：

```c
ret = avfilter_graph_create_filter(ctx, filter, instance_name, args, NULL, graph);
```

**出口的格式在配置之后问出来。** WAV 头里要写采样率和声道数，这要等协商完才知道：

```c
c->out_rate     = av_buffersink_get_sample_rate(c->sink);
c->out_channels = av_buffersink_get_channels(c->sink);
```

验证：和 ffmpeg 命令行比较，逐字节相同。

```bash
tail -c +45 out/half.wav | md5sum
ffmpeg -v error -i samples/small_360p25.mp4 -vn -af volume=0.5 -f s16le - | md5sum
```

```text
7c6bf4f3002557d1b43f744e63550e14  -
7c6bf4f3002557d1b43f744e63550e14  -
```

音量真的减半了吗？用 ffmpeg 自带的 volumedetect 滤镜量一下。先生成一个没调音量的对照文件（音量写 1）：

```bash
$B/volume_file samples/small_360p25.mp4 out/orig.wav 1 > /dev/null
for f in out/orig.wav out/half.wav; do
    ffmpeg -hide_banner -i $f -af volumedetect -f null - 2>&1 | grep -E "mean_volume|max_volume" | sed 's/.*] //'
done
```

```text
mean_volume: -24.1 dB
max_volume: -20.6 dB
mean_volume: -30.1 dB
max_volume: -26.6 dB
```

平均音量和最大音量都低了 6.0 dB。幅度减半就是降低 6 分贝（20 × log10(0.5) = -6.02），所以音量也可以直接写成 `-6dB`。

## 第 4 步：格式协商

**换一个输入。** L10 做的测试音 `out/tone_lr.wav` 是 s16 的（没有的话按 L10 第 4 步的命令生成）：

```bash
$B/volume_file out/tone_lr.wav out/t_half.wav 0.5 | head -7
```

```text
入口 abuffer 的参数：time_base=1/44100:sample_rate=44100:sample_fmt=s16:channel_layout=stereo
图里的连接（上游 -> 下游：这条连接上的格式）：
  src -> auto_aresample_0：s16，44100 Hz，stereo
  auto_aresample_0 -> vol：flt，44100 Hz，stereo
  vol -> auto_aresample_1：flt，44100 Hz，stereo
  auto_aresample_1 -> fmt：s16，44100 Hz，stereo
  fmt -> sink：s16，44100 Hz，stereo
```

输入是 s16，输出也要 s16，中间却插了**两个**转换：先转成 flt，调完音量再转回 s16。
因为 volume 默认用浮点计算（第 1 步看到的 `precision` 选项，默认 `float`），它只接受浮点格式的输入。

让它用定点计算，转换就都不需要了。`volume_file` 把第三个参数原样拼进 volume 的参数字符串，所以可以这样带上第二个选项：

```bash
$B/volume_file out/tone_lr.wav out/t_half_fixed.wav 0.5:precision=fixed | sed -n 2,5p
```

```text
图里的连接（上游 -> 下游：这条连接上的格式）：
  src -> vol：s16，44100 Hz，stereo
  vol -> fmt：s16，44100 Hz，stereo
  fmt -> sink：s16，44100 Hz，stereo
```

两种算法的结果有细微的差别（`cmp out/t_half.wav out/t_half_fixed.wav` 会报告不同），浮点的更精确，定点的省去两次转换。
自动插入的转换很方便，但它不是免费的：处理大量数据时，值得看一眼图里到底插了什么。

**同时改采样率。** 给 aformat 加上采样率的要求：

```bash
$B/volume_file samples/small_360p25.mp4 out/v48.wav 0.5 48000 | sed -n 2,8p
```

```text
图里的连接（上游 -> 下游：这条连接上的格式）：
  src -> vol：fltp，44100 Hz，stereo
  vol -> auto_aresample_0：fltp，44100 Hz，stereo
  auto_aresample_0 -> fmt：s16，48000 Hz，stereo
  fmt -> sink：s16，48000 Hz，stereo
出口 abuffersink 的格式：s16，48000 Hz，2 声道
送进 220500 个采样，取出 240000 个采样，其中冲刷时取出 17 个
```

还是那一个 `auto_aresample_0`，这次它把采样格式和采样率一起转了。取出 240,000 个采样，和 L10 的 `resample_file` 一个不差，
其中 17 个是冲刷时取出来的。`main()` 里冲刷的顺序和 L10 说的一样，从上游到下游：

```c
ret = decode(dec, NULL, frame, &c);             /* 先冲刷解码器，吐出的帧照常送进图 */
...
ret = av_buffersrc_add_frame(c.src, NULL);      /* 再告诉图：输入结束了 */
ret = drain(&c);                                /* 取到 AVERROR_EOF 为止 */
```

## 第 5 步：音量调大会怎样

```bash
$B/volume_file samples/small_360p25.mp4 out/loud.wav 16 | tail -1
ffmpeg -hide_banner -i out/loud.wav -af volumedetect -f null - 2>&1 | grep -E "max_volume|histogram_0db" | sed 's/.*] //'
```

```text
写出 out/loud.wav：44 字节文件头 + 882000 字节数据，时长 5.000 秒
max_volume: 0.0 dB
histogram_0db: 249860
```

原来的最大音量是 -20.6 dB，放大 16 倍（+24 dB）之后超过了 0 dB。16 位整数装不下，超出的部分被压在最大值上，这叫**削波**。
`histogram_0db` 说有 249,860 个采样顶在了最大值，声音听起来是破的。

放大音量之前先量一下 `max_volume`：它是 -20.6 dB，就最多放大 20.6 dB（大约 10.7 倍），再大就削波。

## 第 6 步：图建好之后格式就固定了

用 L10 第 5 步做的 `out/change.aac`（前一半 44100 Hz 立体声，后一半 22050 Hz 单声道）：

```bash
$B/volume_file out/change.aac out/c.wav 0.5 > /dev/null
```

```text
[aac @ 0x563e318c6c40] ADTS frame duration changed mid-stream to 1024 samples at 22050 Hz
[aac @ 0x563e318c6500] Estimating duration from bitrate, this may be inaccurate
[src @ 0x563e31993540] filter context - fmt: fltp r: 44100 layout: 3 ch: 2, incoming frame - fmt: fltp r: 22050 layout: 4 ch: 1 pts_time: 1.044898
[src @ 0x563e31993540] Changing audio frame properties on the fly is not supported.
把帧送进滤镜图出错：Invalid argument
```

（方括号里的地址每次运行都不同。）入口滤镜发现送来的帧和建图时说的不一样，拒绝了。
`avfilter_graph_config()` 之后，每条连接上的格式都定死了，图不会中途改变。

L10 的 `swr_convert_frame()` 遇到这种情况可以关掉重来，滤镜图也是同样的办法，只是要自己做：
把旧图冲刷干净，`avfilter_graph_free()` 释放，再按新的帧参数重新建一张。ffmpeg 命令行工具就是这样处理的。
`volume_file` 没有做这一步，练习 5 请你来补上。

## 常见错误

1. **入口滤镜的参数和实际送进去的帧不一致。** 轻则报错，重则声音变调或者变成噪音。参数从帧上取，不要凭印象写。
2. **忘了 `avfilter_graph_config()`。** 连接好只是搭了架子，格式还没有协商，没配置的图不能用。
3. **用 `av_opt_set()` 给滤镜设参数时漏了 `AV_OPT_SEARCH_CHILDREN`。** 选项在滤镜的私有数据上，不加这个标志找不到。
4. **参数字符串里的分隔符写错。** 同一个滤镜的多个参数用 `:` 分隔，一个参数的多个取值用 `|` 分隔（例如 `sample_fmts=s16|flt`）。
5. **送进入口之后还去用那一帧。** `av_buffersrc_add_frame()` 把数据拿走了。还要用的话，改用 `av_buffersrc_add_frame_flags()` 加 `AV_BUFFERSRC_FLAG_KEEP_REF`。
6. **送一帧只取一次。** 一帧输入可能对应多帧输出，要循环取到 `AVERROR(EAGAIN)`。
7. **结束时不送 `NULL`，或者送了之后不再取。** 留在图里的数据就丢了。官方示例漏了这一步（patch 0047）。
8. **从出口取出的帧不 `av_frame_unref()`。** 每帧泄漏一块内存。
9. **逐个释放滤镜实例。** `avfilter_graph_free()` 会释放图里所有的实例，再单独释放就是重复释放。
10. **假定出口的格式和入口一样。** 协商的结果要在配置之后用 `av_buffersink_get_*()` 问出来；想要确定的格式，就在出口前面放 aformat。

## 练习

1. 用 `filter_info` 查 `atempo` 滤镜（变速不变调）。它有几个输入、几个输出？主要的选项叫什么？再查 `anullsink`，它有输出吗？
2. 用 `-6dB` 代替 `0.5` 运行第 3 步的命令。输出文件和 `out/half.wav` 完全相同吗？为什么？
3. 把 `samples/audio_44k_stereo.mp2` 的音量放大到 2 倍，同时转成 22050 Hz。图里自动插入了几个滤镜？各做了什么转换？冲刷时取出多少个采样？
4. 把 `volume_file.c` 的 `main()` 里冲刷滤镜图的那一段（`av_buffersrc_add_frame(c.src, NULL)` 和后面的 `drain()` 及其检查）去掉，重新编译，
   按第 4 步的命令转 48000 Hz。少了多少个采样？不带采样率参数时还少吗？做完把代码改回去。
5. （选做）让 `volume_file` 能处理 `out/change.aac`：在 `process_frame()` 里发现帧的采样格式、采样率或声道布局和建图时不同，就冲刷并释放旧图，
   按新参数重建。要注意输出文件只有一个头，所以两张图的出口格式必须相同，aformat 的参数里要把采样率和声道布局都写死。

<details>
<summary>参考答案</summary>

1. `atempo` 一个音频输入、一个音频输出，主要的选项是 `tempo`（速度的倍数）。`anullsink` 有一个输入、没有输出，它把收到的数据直接丢掉。
2. 不完全相同（`cmp` 会报告差别）。-6 dB 换算成倍数是 10^(-6/20) = 0.5012，不是正好 0.5，相差约 0.2%。
   `-6dB` 的结果与 `ffmpeg -af volume=-6dB` 相同。
3. 命令是 `$B/volume_file samples/audio_44k_stereo.mp2 out/x.wav 2 22050`。自动插入了两个 aresample：
   第一个在 volume 之前，把 s16p 转成 fltp（volume 默认用浮点计算）；第二个在 volume 之后，把 fltp、44100 Hz 转成 s16、22050 Hz。
   送进 221,184 个采样，取出 110,592 个，冲刷时取出 16 个。
4. 转 48000 Hz 时少 17 个采样（239,983 个）。不带采样率参数时不少：只转采样格式的 aresample 没有延迟，图里没有留着的数据。
   但不能因此省掉冲刷，图里有没有带延迟的滤镜，写程序的时候并不知道。
5. 思路：在 `Chain` 里记下建图时的采样格式、采样率和声道布局；`process_frame()` 里先比较，不同就
   `av_buffersrc_add_frame(c->src, NULL)`、`drain()` 到返回 1、`avfilter_graph_free(&c->graph)`，然后走原来“没有图就建图”的分支。
   aformat 的参数写成 `sample_fmts=s16:sample_rates=44100:channel_layouts=stereo` 这样的固定值，并且只在第一次建图时记录出口格式。
   改完后 `out/change.aac` 能完整转出来：送进 69,632 个采样，取出 93,184 个，时长 2.113 秒，和 L10 的 `resample_file` 转成 44100 Hz 立体声的结果一样多。

</details>

## 下一讲

这一讲的图是一个滤镜一个滤镜手工建、手工连的。四个滤镜还好，再多就太啰嗦了。
ffmpeg 命令行里只要写一个字符串 `volume=0.5,aformat=sample_fmts=s16`，libavfilter 就能照着它把图建出来。
L12 学这种用字符串描述滤镜图的方法，并把视频也加进来，例子是官方示例 `decode_filter_video.c` 和 `decode_filter_audio.c`。
