# L10 音频重采样

L09 解决了图像的转换，声音有同样的问题。解码器给的音频不一定是下一步想要的样子：
声卡要 48000 Hz，文件是 44100 Hz；语音识别要 16000 Hz 单声道，文件是立体声；编码器要 fltp，手里是 s16。
L06 手写过一种转换（转成 16 位交错），当时说通用的办法留到这一讲。

FFmpeg 里做音频转换的库是 libswresample。这一讲用它写一个“任何音频文件进来，指定的采样率和声道数出去”的工具。

完成标志：`resample_file` 的输出与 ffmpeg 命令行的结果逐字节相同；
能算出 1024 个 48000 Hz 的采样转成 44100 Hz 是多少个；知道为什么转换结束时必须冲刷。

## 本讲要点

**libswresample 转三样东西，可以一次全转**

| 转什么 | 例子 | 库内部叫 |
| --- | --- | --- |
| 采样格式 | fltp 变 s16，planar 变 packed | 格式转换 |
| 采样率 | 44100 Hz 变 48000 Hz | resample（重采样） |
| 声道布局 | 立体声变单声道，立体声变 5.1 | rematrix（混音） |

“重采样”严格说只指改采样率，习惯上把这三样统称重采样。

**两种用法**（`libswresample/swresample.h`）

| | 底层用法（官方示例） | 按帧的用法（`resample_file.c`） |
| --- | --- | --- |
| 数据放在哪里 | 自己分配的缓冲区 | `AVFrame` |
| 配置 | `swr_alloc()`，逐项 `av_opt_set_*()`，再 `swr_init()`；或者 `swr_alloc_set_opts2()` 加 `swr_init()` | `swr_alloc()` 之后不用配置，第一次转换时从两个帧上取参数 |
| 转换 | `swr_convert(ctx, 输出, 输出容量, 输入, 输入采样数)` | `swr_convert_frame(ctx, 输出帧, 输入帧)` |
| 输出缓冲区多大 | 自己算，见下面的公式 | 库按需要分配 |
| 输入参数中途变了 | 自己发现，自己重建 | 返回 `AVERROR_INPUT_CHANGED`，`swr_close()` 后再调用即可 |
| 冲刷 | 输入传 `NULL` 调用，直到返回 0 | 输入帧传 `NULL` 调用，直到输出帧的 `nb_samples` 是 0 |

**采样数怎么换算**

改采样率之后，同样长的声音采样数不同：

```text
输出采样数 = 输入采样数 × 输出采样率 / 输入采样率
1024 个 48000 Hz 的采样 → 1024 × 44100 / 48000 = 940.8 个 44100 Hz 的采样
```

940.8 不是整数，所以每次调用实际输出 940 或 941 个，多出来的零头留在重采样器里攒到下一次。
用 `av_rescale_rnd(输入采样数, 输出采样率, 输入采样率, AV_ROUND_UP)` 算上限，不会溢出。

**延迟和冲刷**

改采样率用的是滤波器，算一个输出采样要用到它前后的一段输入，所以重采样器里总留着一小段还没处理完的输入。

- `swr_get_delay(ctx, 采样率)`：现在里面留着多少（按给定的采样率折算）。
- 输入结束后要**冲刷**：不给输入再调用转换函数，把留着的取出来。不冲刷，声音的结尾就少一小截。

这和解码器的冲刷（L05）是同一个道理：有内部缓冲的东西，结束时都要冲刷。

**混音规则**

声道数变化时，libswresample 按声道的位置决定怎么混：

- 立体声变单声道：左右各取一半相加。
- 立体声变 5.1：左、右原样放到前左、前右，其余声道是静音。它不会凭空造出环绕声。
- 5.1 变立体声：中置和环绕按一定的系数混进左右。

所以混音必须知道每个声道是什么。文件里只有声道数、没有声道布局时（WAV 和裸 PCM 常见），要先补上默认布局，见第 4 步。

## 准备

```bash
scripts/gen_samples.sh                 # 已经生成过可以跳过
make -C L10_resample_audio             # 可执行文件在 L10_resample_audio/build/ 下
B=L10_resample_audio/build
mkdir -p out
```

下面的命令都在仓库根目录执行。

| 程序 | 用途 |
| --- | --- |
| `resample_audio` | 官方示例（加了课程注释）：自己生成正弦波，改采样率、声道布局和采样格式 |
| `resample_file` | 解码任意文件里的音频，转成指定的采样率和声道布局，写成 16 位 WAV |

## 第 1 步：官方示例 resample_audio.c

```bash
$B/resample_audio out/r.pcm 2>/dev/null | head -4
```

```text
t:0.021333 in:1024 out:925
t:0.042667 in:1024 out:941
t:0.064000 in:1024 out:940
t:0.085333 in:1024 out:941
```

它生成 10 秒的 440 Hz 正弦波（48000 Hz、立体声、dbl 格式），转成 44100 Hz、3.0 声道（左、右、中）、s16 格式。每次送 1024 个采样进去：

- 之后的每一次输出 940 或 941 个，平均正好是 940.8。
- 第一次只输出 925 个，少了 16 个。这 16 个留在重采样器里，就是上面说的延迟。

读代码时找 `课程注释`。主线是四步：

```c
swr_ctx = swr_alloc();                                           /* 1. 分配 */

av_opt_set_chlayout(swr_ctx, "in_chlayout",    &src_ch_layout, 0);  /* 2. 输入是什么 */
av_opt_set_int(swr_ctx, "in_sample_rate",       src_rate, 0);
av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", src_sample_fmt, 0);
av_opt_set_chlayout(swr_ctx, "out_chlayout",    &dst_ch_layout, 0); /*    输出要什么 */
av_opt_set_int(swr_ctx, "out_sample_rate",       dst_rate, 0);
av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

swr_init(swr_ctx);                                               /* 3. 初始化 */

ret = swr_convert(swr_ctx, dst_data, dst_nb_samples,             /* 4. 转换，返回实际输出的采样数 */
                  (const uint8_t **)src_data, src_nb_samples);
```

转换之前，示例每次都估算这一次最多能输出多少，缓冲区不够就重新分配：

```c
dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, src_rate) + src_nb_samples,
                                dst_rate, src_rate, AV_ROUND_UP);
```

意思是：里面留着的，加上这次新给的，一起换算到输出采样率，向上取整。

**少了冲刷。** 把每一行的输入、输出采样数加起来：

```bash
$B/resample_audio out/r.pcm 2>/dev/null | awk '{split($2,a,":"); split($3,b,":"); i+=a[2]; o+=b[2]} END {print i, o}'
```

```text
480256 441219
```

输入 480,256 个采样，按比例应该输出 480256 × 44100 / 48000 = 441,235.2 个，实际 441,219 个，少了 16 个。
就是第一次留在里面的那些，最后没有取出来。16 个采样只有 0.36 毫秒，听不出来；
但如果把很多段音频各自重采样再拼起来，每一段都少一点，音画就会慢慢对不上。

这个示例有 4 处问题，已做成 patch（见 `upstream_patches/` 的 0038 到 0041）：结束时不冲刷；
提示的 ffplay 命令用了已经删除的选项（要写成 `ffplay -f s16le -ch_layout 3.0 -ar 44100 out/r.pcm`）；
不检查写文件是否成功；缓冲区重新分配失败后仍报告成功。

## 第 2 步：resample_file，先只转格式

```bash
$B/resample_file samples/small_360p25.mp4 out/a.wav
```

```text
输入：fltp，44100 Hz，stereo（2 声道）
输出：s16，44100 Hz，stereo（2 声道）
输入 220500 个采样，输出 220500 个采样（按采样率的比例应该是 220500.0），其中最后冲刷出 0 个
写出 out/a.wav：44 字节文件头 + 882000 字节数据，时长 5.000 秒
```

不带采样率和声道参数时，只把采样格式转成 s16。AAC 解码器给的是 fltp（浮点、每个声道一个平面），
L06 的 `decode_wav.c` 用了几十行代码手工做交错、换算和限幅，这里交给库了。只转格式没有延迟，所以冲刷出 0 个。

转换的代码在 `convert_and_write()` 里，核心是：

```c
av_frame_unref(out);
out->sample_rate = r->out_rate;                 /* 在输出帧上写明想要什么 */
out->format      = AV_SAMPLE_FMT_S16;
av_channel_layout_copy(&out->ch_layout, &r->out_layout);

ret = swr_convert_frame(r->swr, out, in);       /* 输出缓冲区由它分配，结果在 out->data[0]，共 out->nb_samples 个采样 */
```

和 L09 的 `sws_scale_frame()` 是同一个思路：`main()` 里只调用了 `swr_alloc()`，没有设置任何参数，也没有调用 `swr_init()`。
第一次调用 `swr_convert_frame()` 时，它从输入帧和输出帧上读出两边的参数，自己完成初始化。

验证：和 ffmpeg 转出的 s16 数据比较（`tail -c +45` 跳过 44 字节的 WAV 头）。

```bash
tail -c +45 out/a.wav | md5sum
ffmpeg -v error -i samples/small_360p25.mp4 -vn -f s16le - | md5sum
```

```text
1abe542df3a973c7596f8e5bde4b7b46  -
1abe542df3a973c7596f8e5bde4b7b46  -
```

## 第 3 步：改采样率

```bash
$B/resample_file samples/small_360p25.mp4 out/a48.wav 48000
```

```text
输入：fltp，44100 Hz，stereo（2 声道）
输出：s16，48000 Hz，stereo（2 声道）
输入 220500 个采样，输出 240000 个采样（按采样率的比例应该是 240000.0），其中最后冲刷出 17 个
写出 out/a48.wav：44 字节文件头 + 960000 字节数据，时长 5.000 秒
```

220,500 × 48000 / 44100 = 240,000，一个不差，时长还是 5 秒。其中 17 个是最后冲刷出来的。
`resample_file.c` 的冲刷是这样写的：

```c
while ((ret = convert_and_write(r, NULL)) > 0)   /* 输入帧传 NULL */
    total += ret;
```

`main()` 里的顺序是：文件读完，先冲刷解码器（送 `NULL` 包，L05），解码器吐出的最后几帧照常送进重采样器，然后再冲刷重采样器。顺序不能反。

和 ffmpeg 比较，逐字节相同：

```bash
tail -c +45 out/a48.wav | md5sum
ffmpeg -v error -i samples/small_360p25.mp4 -vn -ar 48000 -f s16le - | md5sum
```

```text
a1b74d23acecdda5fe369716ed5205e5  -
a1b74d23acecdda5fe369716ed5205e5  -
```

降采样也一样。降到 16000 Hz 单声道（语音识别常用的输入）：

```bash
$B/resample_file samples/small_360p25.mp4 out/a16m.wav 16000 mono
```

```text
输入：fltp，44100 Hz，stereo（2 声道）
输出：s16，16000 Hz，mono（1 声道）
输入 220500 个采样，输出 80000 个采样（按采样率的比例应该是 80000.0），其中最后冲刷出 16 个
写出 out/a16m.wav：44 字节文件头 + 160000 字节数据，时长 5.000 秒
```

采样率决定了能表示的最高频率：16000 Hz 的采样率最高只能表示 8000 Hz 的声音。降采样时，高于这个频率的成分必须先滤掉，
否则会变成本来没有的低频杂音（混叠）。libswresample 的重采样滤波器已经做了这件事，这也是不能用“每隔几个采样取一个”来降采样的原因。

## 第 4 步：改声道布局

先准备 L06 用过的测试音：左声道 441 Hz、幅度 0.5，右声道 882 Hz、幅度 0.25。

```bash
ffmpeg -v error -y -f lavfi -i "aevalsrc=0.5*sin(2*PI*441*t)|0.25*sin(2*PI*882*t):s=44100:d=2" out/tone_lr.wav
$B/resample_file out/tone_lr.wav out/t_51.wav 0 5.1
```

```text
文件没有写明声道布局，2 个声道按 stereo 处理
输入：s16，44100 Hz，stereo（2 声道）
输出：s16，44100 Hz，5.1（6 声道）
输入 88200 个采样，输出 88200 个采样（按采样率的比例应该是 88200.0），其中最后冲刷出 0 个
写出 out/t_51.wav：44 字节文件头 + 1058400 字节数据，时长 2.000 秒
```

看输出文件里的几个采样时刻，每行是 6 个声道的值（前左、前右、中置、低音、后左、后右）：

```bash
od -An -td2 -j$((44+12*25)) -N48 -w12 out/t_51.wav
```

```text
  16384      0      0      0      0      0
  16352  -1027      0      0      0      0
  16255  -2037      0      0      0      0
  16094  -3016      0      0      0      0
```

左右声道原样放在前两个位置，后四个声道全是 0。声道变多时库只是把已有的声道放到对应的位置，不会生成环绕声。

**第一行输出“文件没有写明声道布局”。** 这个 WAV 文件头里只有声道数 2，没有说这两个声道是左和右。
解码出来的帧，布局是“未指定”（`AV_CHANNEL_ORDER_UNSPEC`），打印出来是 `2 channels` 而不是 `stereo`。
混音要按声道的位置来算，所以 `resample_file.c` 在转换之前先补上默认布局：

```c
if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
    const int nb_channels = frame->ch_layout.nb_channels;

    av_channel_layout_uninit(&frame->ch_layout);
    av_channel_layout_default(&frame->ch_layout, nb_channels);   /* 1 个是 mono，2 个是 stereo，6 个是 5.1 */
}
```

这和 L09 里“文件没写色彩矩阵就按高度猜”是同一类问题：文件缺信息时按通行的约定补上，并且让用户知道是猜的。

混成单声道时，左右各取一半相加：

```bash
$B/resample_file out/tone_lr.wav out/t_mono.wav 0 mono | tail -1
od -An -td2 -j$((44+2*25)) -N8 -w8 out/t_mono.wav
```

```text
写出 out/t_mono.wav：44 字节文件头 + 176400 字节数据，时长 2.000 秒
   8192   7663   7109   6539
```

第一个值 8192 就是上面第一行的 (16384 + 0) / 2。各取一半是为了不溢出：两个满幅的声道直接相加会超出 16 位的范围。

说明：`resample_file` 写的是最简单的 WAV 头，只有声道数。两个声道以内没有问题；超过两个声道时，
规范的做法是用带声道掩码的扩展头，这里从简，所以 `t_51.wav` 用播放器打开时声道的含义靠约定。写规范的多声道文件用 L15 的复用器。

## 第 5 步：输入参数中途变了

L06 提过：少数码流的参数会中途变化，当时的 `decode_wav` 遇到就报错。造一个这样的文件：
两段 AAC（ADTS 裸流）首尾相接，前一段 44100 Hz 立体声，后一段 22050 Hz 单声道。

```bash
ffmpeg -v error -y -f lavfi -i "sine=440:d=1" -ar 44100 -ac 2 -c:a aac out/p1.aac
ffmpeg -v error -y -f lavfi -i "sine=880:d=1" -ar 22050 -ac 1 -c:a aac out/p2.aac
cat out/p1.aac out/p2.aac > out/change.aac
L06_decode_audio/build/decode_wav aac out/change.aac out/c6.wav 2>&1 >/dev/null | tail -1
```

```text
第 46 帧的参数和第一帧不同，本程序不处理
```

用 `resample_file` 统一成 48000 Hz 立体声：

```bash
$B/resample_file out/change.aac out/change.wav 48000 stereo 2>/dev/null
```

```text
输入：fltp，44100 Hz，stereo（2 声道）
输出：s16，48000 Hz，stereo（2 声道）
输入的参数变了：fltp，22050 Hz，mono（1 声道）
输入 69632 个采样，输出 101426 个采样（按采样率的比例应该是 101424.8），其中最后冲刷出 35 个
写出 out/change.wav：44 字节文件头 + 405704 字节数据，时长 2.113 秒
```

（`2>/dev/null` 挡掉的是解复用器打印的两行提示，其中一行说的就是参数中途变了。）

输出文件从头到尾是一种参数，可以正常播放。处理的代码在 `process_frame()` 里：

```c
ret = convert_and_write(r, frame);
if (ret == AVERROR_INPUT_CHANGED) {      /* 输入帧的参数和初始化时不一样了 */
    ret = flush(r);                      /* 先把按旧参数留在里面的采样取完 */
    if (ret >= 0) {
        swr_close(r->swr);               /* 关掉，回到没有初始化的状态 */
        ret = convert_and_write(r, frame);   /* 再调用一次，它按新参数重新初始化 */
    }
}
```

直播流切换信号源、把几个文件拼起来播放时，都会遇到这种情况。输出端（声卡、编码器、文件头）通常只接受一种固定的参数，
所以实际的程序都是这个结构：**输出参数固定，输入是什么就从什么转过来**。

## 常见错误

1. **结束时不冲刷重采样器。** 改采样率时结尾少一小截。官方示例就漏了（patch 0041）。
2. **冲刷的顺序反了。** 先冲刷解码器，把它吐出来的帧送进重采样器，最后才冲刷重采样器。
3. **假设输出采样数等于输入采样数，或者每次都一样多。** 改了采样率之后不相等，而且每次调用的输出数量会差一个。以返回值（或 `out->nb_samples`）为准。
4. **用底层接口时输出缓冲区按输入的采样数分配。** 升采样时装不下。装不下的不会丢，会留在里面越积越多，内存一直涨。要按公式算上限。
5. **设完参数忘了 `swr_init()`，或者初始化之后又改参数。** 改了参数要重新初始化。
6. **只比较采样率，不管声道布局和采样格式有没有变。** 三样里任何一样变了，原来的上下文都不能用了。
7. **对“未指定”的声道布局直接做混音。** 库不知道声道的位置。先用 `av_channel_layout_default()` 补上。
8. **以为立体声转 5.1 会有环绕声。** 新增的声道是静音。
9. **用“隔几个取一个”降采样。** 会产生混叠，声音里多出刺耳的杂音。
10. **拿采样数当时间。** 采样数除以采样率才是秒。改了采样率之后，时间戳要用新的采样率重新算（L14 讲编码时会用到）。

## 练习

1. 把 `small_360p25.mp4` 的音频转成 22050 Hz 和 96000 Hz。不看输出，先算出各应该有多少个采样，再运行核对。最后冲刷出的各是多少个？
2. 用 `resample_file` 把 `samples/audio_44k_stereo.mp2` 转成 11025 Hz 单声道。输入是什么采样格式？输入和输出各多少个采样？为什么输入不是 220,500 个？
3. 把 `resample_file.c` 的 `main()` 里冲刷重采样器的那一段去掉（让 `flushed` 等于 0），重新编译，转 48000 Hz。输出少了多少个采样？和 ffmpeg 的结果比，前面的数据相同吗？做完把代码改回去。
4. 用官方示例输出的 `out/r.pcm` 验证它是 3 个声道：先按文件大小算出每个声道的采样数，再用 `od -An -td2 -N18 -w6 -j6000 out/r.pcm` 看几个采样时刻。第三个声道（中置）的值和前两个是什么关系？
5. `ffmpeg -v error -y -f lavfi -i "sine=440:d=1" -ac 6 -c:a pcm_s16le out/six.wav` 生成一个 5.1 的文件。用 `resample_file` 把它混成立体声，再混成单声道，看程序打印的输入布局。它为什么没有提示“文件没有写明声道布局”？

<details>
<summary>参考答案</summary>

1. 22050 Hz：220500 × 22050 / 44100 = 110,250 个，冲刷出 16 个。96000 Hz：220500 × 96000 / 44100 = 480,000 个，冲刷出 34 个。
   冲刷出的数量和输出采样率有关：里面留着的是一小段固定长度的输入，折算到越高的输出采样率，采样数越多。
2. 输入是 s16p（MP2 解码器输出 16 位 planar）。输入 221,184 个采样，输出 55,296 个，正好是四分之一。
   MP2 一帧固定 1152 个采样，5 秒的声音要 192 帧，192 × 1152 = 221,184，比 5 秒多出不到一帧（L06）。
3. 少 17 个：输出 239,983 个采样。前面的 239,983 × 4 字节与 ffmpeg 的结果相同，只是结尾少了一截。
4. 文件 2,647,314 字节，每个采样时刻 3 个声道 × 2 字节 = 6 字节，共 441,219 个采样时刻。
   前两个声道的值相同（示例的输入就是两个声道放同一个正弦波），第三个声道（中置）是 0：
   立体声转 3.0 时左右原样保留，中置静音，和第 4 步立体声转 5.1 的规则一样。
5. 程序打印的输入是 `5.1（6 声道）`。ffmpeg 写超过两个声道的 WAV 时用的是带声道掩码的扩展头，文件里写明了布局，所以不需要猜。
   混成立体声、单声道都能直接完成，中置和环绕声道按系数混进了左右。

</details>

## 下一讲

到这里，“解码之后把数据转成想要的格式”的两个库都学完了：图像用 libswscale，声音用 libswresample。
缩放和重采样其实只是两种最常用的处理。裁剪、叠加文字、调音量、变速……更一般的处理由 libavfilter 完成。
L11 学滤镜图的基本概念，例子是官方示例 `filter_audio.c`。
