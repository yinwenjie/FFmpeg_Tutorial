# L22 工程化专题

前 21 讲每讲教一个接口。这一讲不教新接口，教怎么让前面写出来的东西**在工程里站得住**：出了问题能查到、改了代码知道有没有改坏、
别人拿去用不会因为版本和许可证出事。六件事：

| 步骤 | 做什么 | 代码 |
| --- | --- | --- |
| 1 | 带检查的构建：ASan、UBSan、TSan、valgrind，以及一次把前面所有程序扫一遍 | `check_all.sh` |
| 2 | 输入参数中途变了，滤镜图要重建（**兑现 L12 的话**） | `reconfig_graph.c` |
| 3 | 自己的日志回调：分级、留下“出错之前发生了什么”、多线程 | `fflog.h`、`log_demo.c` |
| 4 | FFmpeg 的错误码，和进程退出码为什么不能用错误码（**兑现 L17、L20、L21 的话**） | `err_codes.c` |
| 5 | 编译用的和运行时加载的 FFmpeg 是不是同一套；版本、许可证、带了什么 | `version_info.c` |
| 6 | 回归：一条命令跑完前面所有检查，每一项的“算通过”的标准写死在脚本里 | `regress.sh`、`regress_transcode.sh`、`ebml_dump.py` |

完成标志：能给任何一个用 FFmpeg 的 C 程序加上 ASan 构建并看懂报告；知道输入参数中途变化时滤镜图为什么必须重建、怎么重建；
会换掉默认的日志输出，并知道多线程时为什么要加锁；看到退出码 187、234、255 能说出来历，自己的程序不再用错误码当退出码；
能说清楚“编译时的头文件”和“运行时的库”是两回事；`regress.sh` 跑完全部通过。

> **这一讲有几处限制，请先读完。**
> 1. 本机没有显卡，L20、L21 的硬件部分标着【待补】。`regress.sh` 里那两步只跑“没有显卡也能做的检查”，在有显卡的机器上要另外跑 `L20_hw_decode/verify_hw.sh` 和 `L21_hw_encode/verify_hw.sh`。
> 2. 第 5 步“头文件和库的版本不一致”，**在这台机器上做不出来**：同一个大版本、不同次版本的两套 FFmpeg 这里没有（机器上自带的是 FFmpeg 6.1，库的 SONAME 是 `.so.58`、`.so.60`，课程编译的是 `.so.61`、`.so.63`，名字不同，不会混用）。
>    所以那一步只讲机制和怎么检查，不演示“出事”的现场。
> 3. 第 6 步里有三个输出格式和 ffmpeg 命令行做不到文件逐字节相同，只能比到“逐包相同”，原因已经查清楚（第 6 步）。

## 准备

```bash
export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig PATH=$HOME/ffmpeg_build/bin:$PATH LD_LIBRARY_PATH=$HOME/ffmpeg_build/lib
cd L22_engineering
make                                  # reconfig_graph、log_demo、err_codes、version_info
cd .. && B=L22_engineering/build S=samples
```

素材是 `scripts/gen_samples.sh`（见 L02）生成的。第 2 步用到的两个中途变化的文件现做：

```bash
D=out/l22_demo && mkdir -p $D
# 视频：320x180 一秒，接 640x360 一秒，裸 H.264（每个编码出来的码流自带 SPS、PPS，直接拼起来解码器会重新初始化）
ffmpeg -v error -y -f lavfi -i testsrc2=size=320x180:rate=25 -t 1 -c:v libx264 -threads 1 -f h264 $D/a.h264
ffmpeg -v error -y -f lavfi -i testsrc2=size=640x360:rate=25 -t 1 -c:v libx264 -threads 1 -f h264 $D/b.h264
cat $D/a.h264 $D/b.h264 > $D/two_sizes.h264
# 音频：44100 Hz 一秒，接 48000 Hz 一秒，裸 AAC（ADTS）
ffmpeg -v error -y -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -c:a aac -f adts $D/a.aac
ffmpeg -v error -y -f lavfi -i sine=frequency=440:sample_rate=48000:duration=1 -c:a aac -f adts $D/b.aac
cat $D/a.aac $D/b.aac > $D/change.aac
```

## 第 1 步：带检查的构建

C 程序里的内存错误，运行时不一定崩，崩了也不一定在出错的地方。所以**每个程序在交出去之前，至少要在检查工具下跑一遍**。

| 工具 | 查什么 | 怎么用 | 代价（各工具文档里的经验数字，随程序而变） |
| --- | --- | --- | --- |
| ASan | 越界读写、释放后使用、内存泄漏 | `make SANITIZE=address,undefined BUILD=build-asan` | 慢约 2 倍，内存多 2 到 3 倍 |
| UBSan | 有符号溢出、移位越界、空指针算术等未定义行为 | 和 ASan 一起开（上面那条） | 几乎没有 |
| TSan | 数据竞争 | `make SANITIZE=thread BUILD=build-tsan` | 慢 5 到 15 倍；不能和 ASan 一起开，所以另一个目录 |
| valgrind | 和 ASan 差不多，不用重新编译，但能查没初始化的数据 | `valgrind --leak-check=full --error-exitcode=99 程序 …` | 慢 20 到 50 倍 |

两个要点：

- **要加编译选项用环境变量，不要在命令行上写 `make CFLAGS=…`。** 各讲 Makefile 里写的是 `CFLAGS += …`，命令行上的 `CFLAGS` 会盖掉它（连 `-std` 和 pkg-config 给的头文件路径一起盖掉），编译就失败了。
  上面的 `SANITIZE=` 和 `EXTRA_CFLAGS=` 是 Makefile 专门留的入口。
- **`ulimit -v` 会让 ASan 起不来**（它要先保留一大块虚拟地址）：`ulimit -v 4000000` 以后运行，直接是 `AddressSanitizer failed to allocate … (errno: 12)` 和 `ReserveShadowMemoryRange failed … Perhaps you're using ulimit -v`。

### 把前面所有程序扫一遍

```bash
L22_engineering/check_all.sh                  # ASan + UBSan，L02 到 L21 的全部程序
L22_engineering/check_all.sh asan L16 L17     # 只扫某几讲
L22_engineering/check_all.sh valgrind L05     # valgrind（慢，只跑正常输入）
L22_engineering/check_all.sh plain            # 不带检查，只看退出码和超时（对照用）
```

对每个程序，脚本做三件事：用正常的输入跑一遍，退出码必须是 0；用“坏输入”（空文件、乱码、截断的文件、只有视频或只有音频的文件、一张图片、17x15 的奇数尺寸）各跑一遍，
不要求成功，但**不能崩溃、不能超时、不能有任何 sanitizer 的报告**；最后用不存在的输入、不存在的输出目录各跑几个。结果表在 `out/l22/<模式>/results.tsv`。

这台机器上的结果（L02 到 L21）：

| 模式 | 项数 | 通过 | 已知问题 | 失败 |
| --- | --- | --- | --- | --- |
| asan | 416 | 401 | 15 | 0 |
| plain | 416 | 404 | 12 | 0 |
| valgrind | 101 | 98 | 3 | 0 |

（valgrind 只跑正常输入和不存在的路径，不跑坏输入，所以只有 101 项，已知问题是其中的 L03、L15、L17 各一项；全部扫完要 6 到 7 分钟。）

（plain 的 12 项已知问题：L03 `show_metadata` 4 项、L17 `transcode_aac` 7 项，都是退出码不规范；还有 L06 `decode_audio` 空文件段错误（退出码 139）。asan 的 15 项比 plain 多 3 项，是 sanitizer 才看得到的泄漏：L15 `mux` 输出打不开、L16 和 L21 的 libx264 在奇数尺寸打不开编码器。）

**“已知问题”是什么**。脚本里有一张表（`KNOWN`），把每一个已经查清楚的问题对到原因上：要么是官方示例的 bug、已经有 `upstream_patches/` 里的 patch（课程里的拷贝保持上游原样，只加了标注），
要么是库自己的问题。不在表里的任何问题都算失败。这张表也是个文档：新手看它就知道“这个程序为什么在检查下会有报告”。

| 已知问题 | 在哪 | 原因 |
| --- | --- | --- |
| L06 `decode_audio` 空文件段错误 | patch 0019 | 输入是空文件时，`decoded_frame` 还没分配就拿去冲刷 |
| L15 `mux` 输出打不开时泄漏 | patch 0094 | 输出文件打不开直接 `return 1`，所有资源泄漏 |
| L17 `transcode_aac` 退出码 187 | patch 0095 | 出错一律 `return AVERROR_EXIT` |
| L03 `show_metadata` 退出码 183 和 254 | patch 0009 | 直接返回负的 `AVERROR`：坏输入是 `INVALIDDATA`（183），文件不存在是 `ENOENT`（254） |
| L16、L21 在 17x15 上泄漏 166 字节 | 外部：libx264 | 17x15 打不开编码器（宽不是偶数）时 libx264 自己没释放，`ffmpeg` 命令行同样（valgrind 确认） |

**扫出来的、课程自己的问题**只有一个：L18 `avio_mem_in` 里 `memcpy(NULL, …, 0)`，UBSan 报告，已经修了。其余都是上面的官方示例问题。
对写课程的人这是个有用的数据：58 个程序、416 项检查，自己的代码只扫出一个问题，而前面按“每一步都检查返回值、每一条失败路径都释放资源”的写法，是起了作用的。

看懂一份 ASan 报告：

```text
==1234==ERROR: AddressSanitizer: heap-use-after-free on address 0x… at pc 0x…
READ of size 8 at 0x… thread T0
    #0 0x… in encode_write transcode.c:112      ← 读的位置
freed by thread T0 here:
    #0 0x… in av_frame_free
    #1 0x… in main transcode.c:187              ← 谁释放的
previously allocated by thread T0 here:
    #1 0x… in main transcode.c:140              ← 谁分配的
```

三段：在哪里错用了、谁释放的、谁分配的。按这三个位置看代码，一般就能找到。泄漏报告（`LeakSanitizer`）列的是**分配的位置**，要找的是“谁应该释放它”。

## 第 2 步：输入的参数中途变了，滤镜图要重建

L11 第 6 步看过：滤镜图建好以后，入口滤镜（`buffer`、`abuffer`）的格式就固定了，之后送进去的帧必须和建图时说的一致。
可是**输入不一定守规矩**：拼接起来的流、网络直播的流、有的摄像头，分辨率、采样率、声道数都会在中途变。
L12 说过“输入参数中途变化时重建滤镜图”留到 L22。`reconfig_graph.c` 兑现这句话。

```console
$ build=L22_engineering/build; $build/reconfig_graph $D/two_sizes.h264 $D/o.yuv video null
第 1 张图：输入 320x180 yuv420p
第 26 帧：参数变了，尺寸 320x180 → 640x360；重建滤镜图（新输入 640x360 yuv420p）
建了 2 张图；送进 50 帧，取出 50 帧（重建时冲刷旧图取出 0 帧，最后冲刷取出 0 帧）
输出：320x180 yuv420p，每帧 86400 字节
```

输入是 320x180 一秒接 640x360 一秒，第 26 帧解码器吐出来的帧变了尺寸。程序自己发现，重建，而且**输出文件保持 320x180**（每帧 86400 字节，50 帧共 4,320,000 字节）。
重建的四步（`feed()` 函数）：

1. **冲刷旧图**：把图里剩下的帧取出来。
2. **释放旧图**。
3. **按新参数建新图**。
4. **新图的出口必须和旧图一样**：输出文件只有一种格式。做法是在描述末尾加一个固定参数的 `scale`（视频）或 `aformat`（音频）。`ffmpeg` 命令行用的是同一个办法。

“参数变了”看哪些值，也是照着命令行的做法：视频是像素格式、宽、高、宽高比、色彩空间、色彩范围；音频是采样格式、采样率、声道布局。时间戳、帧类型、码率变了不需要重建。

**不重建会怎样**（`reconfig=0`）：

```console
$ $build/reconfig_graph $D/two_sizes.h264 $D/o0.yuv video null reconfig=0
[in @ 0x…] Changing video frame properties on the fly is not supported by all filters.
[in @ 0x…] filter context - w: 320 h: 180 fmt: 0 … incoming frame - w: 640 h: 360 fmt: 0 …
第 1 张图：输入 320x180 yuv420p
建了 1 张图；送进 50 帧，取出 50 帧（重建时冲刷旧图取出 0 帧，最后冲刷取出 0 帧）
输出：320x180 yuv420p，每帧 86400 字节
$ echo $?
0
$ stat -c %s $D/o.yuv $D/o0.yuv
4320000
10800000
```

只有一条警告，`null` 滤镜照样让帧通过，程序也成功退出（退出码 0）。再看输出文件：重建时是 4,320,000 字节（50 帧 x 86400），不重建是 10,800,000 字节——
后 25 帧是 640x360 的原样数据（每帧 345600 字节），而程序自己说的是“每帧 86400 字节”。把这个文件按 320x180 去读，从第 26 帧起画面就是一条条斜着错开的色带：

```console
$ ffmpeg -v error -f rawvideo -pix_fmt yuv420p -s 320x180 -i $D/o0.yuv -vf "select=eq(n\,30)" -frames:v 1 o0_f30.png
```

这是最危险的一种失败：没有报错，退出码是 0，要有人去看输出才发现。换成别的滤镜，结果也不一样：

| 滤镜图 | `reconfig=0` 的结果 |
| --- | --- |
| `null` | 警告一条，帧原样通过，输出错乱（上面） |
| `hflip` | 第 26 帧 `Frame size or format changed without filter graph reinitialization`，`EINVAL`，程序失败（退出码 1），输出只有前 25 帧（2,160,000 字节） |
| `scale=-2:360` | 警告一条，`scale` 自己适应了新尺寸，输出和重建的逐字节相同（17,280,000 字节） |

**所以不能指望库替你发现，要自己比对帧的参数。**
音频更严格，入口滤镜在参数变了时直接 `EINVAL`：

```console
$ $build/reconfig_graph $D/change.aac $D/o0.pcm audio volume=0.5 reconfig=0
[in @ 0x…] filter context - fmt: fltp r: 44100 layout: 4 ch: 1, incoming frame - fmt: fltp r: 48000 layout: 4 ch: 1 pts_time: 1.044898
[in @ 0x…] Changing audio frame properties on the fly is not supported.
第 46 帧：送进滤镜图出错：Invalid argument
```

音频的例子（`[aac @ …]` 开头的两行是解码器自己的提示：“ADTS 帧时长在流中间变了”“按码率估算时长”，拼接的 AAC 文件会有）：

```console
$ $build/reconfig_graph $D/change.aac $D/o.pcm audio volume=0.5
[aac @ 0x…] ADTS frame duration changed mid-stream to 1024 samples at 48000 Hz
[aac @ 0x…] Estimating duration from bitrate, this may be inaccurate
第 1 张图：输入 fltp 44100 Hz mono
第 46 帧：参数变了，采样率 44100 → 48000 Hz；重建滤镜图（新输入 fltp 48000 Hz mono）
建了 2 张图；送进 93 帧，取出 94 帧（重建时冲刷旧图取出 0 帧，最后冲刷取出 1 帧）
输出：s16，44100 Hz，mono
```

**重建时要不要给旧图送“结束”？`flush=` 两种做法，结果不同：**

| | `flush=1`（默认） | `flush=0` |
| --- | --- | --- |
| 做法 | 送一个空帧，把图里攒着的帧全取出来 | 只取现在能取的，直接丢掉旧图 |
| 好处 | 一个采样、一帧都不丢 | 和 ffmpeg 命令行一样（`fftools/ffmpeg_filter.c` 的 `send_frame()` 里调 `read_frames()`，没有送空帧） |
| 代价 | 所有“收到结束就做点什么”的滤镜（`tpad` 补帧、`apad`、`loop`）**每次重建都触发一次** | 图里攒着的帧丢了 |

用 `tpad=stop_mode=clone:stop_duration=0.5`（结束时复制最后一帧补 0.5 秒）和 `aresample=48000` 量一下：

```console
$ $build/reconfig_graph $D/two_sizes.h264 $D/t1.yuv video "tpad=stop_mode=clone:stop_duration=0.5"
建了 2 张图；送进 50 帧，取出 76 帧（重建时冲刷旧图取出 13 帧，最后冲刷取出 13 帧）
$ $build/reconfig_graph $D/two_sizes.h264 $D/t0.yuv video "tpad=stop_mode=clone:stop_duration=0.5" flush=0
建了 2 张图；送进 50 帧，取出 63 帧（重建时冲刷旧图取出 0 帧，最后冲刷取出 13 帧）
$ $build/reconfig_graph $D/change.aac $D/a1.pcm audio aresample=48000          # 198,616 字节
$ $build/reconfig_graph $D/change.aac $D/a0.pcm audio aresample=48000 flush=0  # 198,580 字节
```

`tpad` 那一行：`flush=1` 在中途换分辨率时也补了 13 帧（总共 76 帧，6,566,400 字节），而 `flush=0` 只在真正的结尾补一次（63 帧，5,443,200 字节）——输入明明只有一个结尾。
音频那一行：`flush=1` 在重建时多取出 1 帧，是重采样器攒着的 36 字节（18 个采样）。**哪个是对的，要看你的滤镜图里放了什么**，没有唯一答案。

`flush=0` 就是 `ffmpeg` 命令行的做法：命令行遇到参数变化时调用 `read_frames()` 取走旧图里现在能取的帧，然后直接重建（`fftools/ffmpeg_filter.c` 的 `send_frame()`），没有给旧图送结束。亲自对一下：

```console
$ ffmpeg -v error -y -f aac -i $D/change.aac -af aresample=48000 -f s16le -ar 48000 -ac 1 $D/cli_a.pcm
$ cmp $D/cli_a.pcm $D/a0.pcm && echo 音频逐字节相同           # flush=0
音频逐字节相同
$ cmp $D/cli_a.pcm $D/a1.pcm | head -1                       # flush=1
out/l22_demo/cli_a.pcm out/l22_demo/a1.pcm differ: char 100397, line 1507
$ ffmpeg -v error -y -f h264 -i $D/two_sizes.h264 -vf "tpad=stop_mode=clone:stop_duration=0.5,scale=320:180:flags=bicubic,format=yuv420p" -f rawvideo $D/cli_t.yuv
$ cmp $D/cli_t.yuv $D/t0.yuv && echo 视频逐字节相同           # flush=0
视频逐字节相同
```

（视频那条命令末尾的 `scale=320:180,format=yuv420p` 是 `reconfig_graph` 自己加的固定出口，所以命令行也要写上才一样。）

## 第 3 步：自己的日志回调

FFmpeg 所有的库都通过 `av_log()` 输出日志，默认写 stderr。做成产品，这不够：要分级、要进自己的日志系统、要在**出错的时候知道出错之前库在干什么**。
`fflog.h` 是一个头文件，可以直接拷进你的项目。它做三件事：

1. 屏幕上只显示 `level` 以上的重要日志；
2. 同时把更详细的日志（例如 debug）记进一个**环形缓冲区**，只保留最近的 N 行；平时不显示，出错时用 `fflog_dump_recent()` 打印，当作出错前的上下文；
3. 统计警告和错误的行数。

```console
$ $build/log_demo levels
这是一条 fatal（值 8）
这是一条 error（值 16）
这是一条 warning（值 24）
这一行分两次写，第二次不应该再有前缀
屏幕显示 warning 及更重要的，缓冲区记 debug 及更重要的
共 7 行，其中错误 2 行，警告 2 行
---- 缓冲区里的内容（最近 7 行，缓冲区 32 行）----
  [fatal  ] 这是一条 fatal（值 8）
  …
  [debug  ] 这是一条 debug（值 48）
  [warning] 这一行分两次写，第二次不应该再有前缀
----
```

屏幕上只有 warning 以上；缓冲区里 7 行都有，包括 info、verbose、debug。成功的时候你看不到这些：

```console
$ $build/log_demo open $S/small_360p25.mp4
解出 10 帧，共 84 行日志（错误 0，警告 0）
```

失败的时候，倒出来：

```console
$ head -c 30000 /dev/urandom > $D/garbage.bin
$ $build/log_demo open $D/garbage.bin lines=8
失败：Invalid data found when processing input
---- 出错之前的日志（最近 3 行，缓冲区 8 行）----
  [debug  ] [AVFormatContext @ 0x…] Opening 'out/l22_demo/garbage.bin' for reading
  [debug  ] [file @ 0x…] Setting default whitelist 'file,crypto,data'
  [verbose] [AVIOContext @ 0x…] Statistics: 30000 bytes read, 0 seeks
----
```

“Invalid data” 只告诉你结果，缓冲区告诉你它读了 30000 字节还是 0 字节。这就是“出错时才打印的详细日志”的价值：平时屏幕干净，出了事不用重新跑一遍加 `-v debug`。

**必须知道的几件事**（`fflog.h` 开头的注释里都有，每一条都验证过）：

- **自己的回调会收到所有级别**，不管 `av_log_get_level()` 是多少。默认回调自己做了级别判断，换了回调就要自己做。
  不过有的库在拼日志之前先看一眼 `av_log_get_level()`，想要完整的 debug 日志，同时要 `av_log_set_level(AV_LOG_DEBUG)`，`fflog_init()` 替你做了。
- **一次 `av_log()` 调用不一定是完整的一行**（没有换行符的调用会被后面的接上）。`av_log_format_line2()` 用 `print_prefix` 记住“上一次有没有以换行结尾”，这样接着写的时候不重复前缀。
  这个状态要放在回调外面，而且所有线程共用。
- **回调里不能再调 `av_log()`**，会递归。
- **`av_log_set_callback()` 本身不是线程安全的**，要在启动其他线程之前调用。

**多线程。** 帧线程、HTTP 服务器的每个连接都会同时调 `av_log()`，所以回调里的共享状态（环形缓冲区、统计行数、`print_prefix`）要加锁：

```console
$ $build/log_demo threads 8 2000 2>/dev/null     # 8 个线程各写 2000 行；这 16000 行日志本身写在 stderr，这里不看，只看统计
8 个线程各写 2000 行，应该有 16000 行警告，实际 16000 行
```

不加锁会怎样？编译时加 `-DFFLOG_NO_LOCK` 去掉锁，用 TSan 构建：

```bash
make -C L22_engineering SANITIZE=thread BUILD=build-tsan                                   # 有锁
make -C L22_engineering SANITIZE=thread BUILD=build-tsan-nolock EXTRA_CFLAGS=-DFFLOG_NO_LOCK   # 没有锁
```

| | 有锁 | 没有锁 |
| --- | --- | --- |
| TSan 的 `WARNING: ThreadSanitizer` 报告 | 0 | 9（运行了两次，都是 9 条） |
| 统计到的行数（应该 16000） | 16000 | 7747（每次运行不同，我们见过 7965） |

没有锁时统计的行数少了一半，每次运行还不一样——这种 bug 在产品里表现为“偶尔数据不对”，不会崩溃，很难查。每一行本身没有被撕开（stdio 自己有锁），丢的是**统计**。
没有 TSan 就看不到这个问题；这是“带检查的构建”必须包括 TSan 的原因。

## 第 4 步：FFmpeg 的错误码

```console
$ $build/err_codes
名字                                   数值  四个字符  av_strerror() 的原文
AVERROR_EOF                          -541478725  EOF       End of file
                                                           → 输入结束了，不算出错：读完了就结束循环，不要当失败处理
AVERROR(EAGAIN)                             -11            Resource temporarily unavailable
                                                           → 送包/取帧的接口：现在不能做这一步，先做另一步（…）
……（共 39 项）
```

不带参数列出常用的：名字、数值、`av_strerror()` 的原文、遇到了该怎么办。带数字可以解释任意值：`err_codes -541478725 -2 0xdfb9b0bb`。

**约定：**

- 返回值 `>= 0` 是成功（有的函数用它表示数量、字节数），`< 0` 是错误。
- 系统错误（`errno`）转成 `AVERROR(e)`，也就是 `-e`，数值取决于平台（Linux 上 `EAGAIN` 是 11，macOS 上是 35）。**所以不要在代码里写死数字，永远用名字。**
- FFmpeg 自己定义的错误是 `FFERRTAG(a, b, c, d)`：四个字符拼成一个整数取负，所以能从数值里“读”出名字：`AVERROR_EOF` 是 `-0x20464f45`，`0x45 0x4f 0x46 0x20` 就是 `EOF `。
  这在 `err_codes` 的“四个字符”一列里能看到：`AVERROR_INVALIDDATA` 是 `INDA`，`AVERROR_ENCODER_NOT_FOUND` 是 `\xf8ENC`。

`err_codes demo` 演示了三个容易踩的坑：

1. **缓冲区太小**：`av_strerror()` 把原文截断，仍然以 `\0` 结尾，返回值照样是 0。`AV_ERROR_MAX_STRING_SIZE` 是 64，库里最长的一条原文放得下。
2. **不认识的码**：返回 `-22`，缓冲区里是 `Error number %d occurred`；正数也不会崩。
3. **`av_err2str(e)` 是个宏**，展开成 `av_make_error_string((char[64]){0}, 64, e)`，缓冲区是复合字面量，只活到所在的语句结束。
   C 里可以直接放在 `printf` 的参数里；`const char *s = av_err2str(e);` 马上悬空。C++ 里 g++ 13 编译不过（`taking address of temporary array`），clang++ 18 当扩展接受。要写可移植的代码，自己准备缓冲区：
   `char buf[AV_ERROR_MAX_STRING_SIZE]; av_strerror(e, buf, sizeof(buf));`。

### 兑现：为什么退出码是 187、234、255

L17、L20、L21 都出现了一堆奇怪的退出码。真相只有一条：**进程退出码只有 8 位。** `main()` 返回一个负的错误码，退出码是它的低 8 位。
`err_codes` 把每一个都算出来了：

| 返回值 | 退出码 | 在哪里遇到过 |
| --- | --- | --- |
| `AVERROR_EOF`、`AVERROR_EXIT`、`AVERROR_EXTERNAL` | **187**（三个全是！） | L17 `transcode_aac`（0095）；L20、L21 里 `-hwaccel vaapi` 没有设备时的 `ffmpeg` 命令行 |
| `AVERROR(EINVAL)`（-22） | **234** | L21：`-vaapi_device` 指向不存在的设备、`hwupload` 找不到设备 |
| `AVERROR(ENOSYS)`（-38） | **218** | L21：内存里的 nv12 帧直接送给 `h264_vaapi`（缺 `hwupload`） |
| `AVERROR_ENCODER_NOT_FOUND` | **8** | `ffmpeg -c:v 不存在的名字` |
| `AVERROR(ENOENT)`（-2） | **254** | 输入文件不存在（`ffmpeg`、L03 `show_metadata`） |
| `return -1` | **255** | L03、L15、L20、L21 官方示例的用法错误和文件错误 |

三个问题：

- **互相撞车**：`EOF`、`EXIT`、`EXTERNAL` 都以 `E` 开头，退出码都是 187。“读完了”（正常）和“出错了”分不开。官方示例 `transcode_aac.c` 就是这样（patch 0095）。
- **同一个原因，不同的路径给不同的码**：L21 里“没有设备”在 `hw_encode_av` 里可以是 `Generic error in an external library` 也可以是 `Invalid argument`，命令行是 234 或 187。
  **不要靠退出码判断原因。**
- **脚本里没法用**：`if [ $? -eq 187 ]`，换了平台、换了 FFmpeg 版本就变了。

**自己的程序的约定**：成功 0，一般错误 1，用法错误 2；要区分原因，自己定一张表并写在用法里；**错误的原因写在 stderr**（用 `av_strerror()` 翻译），而不是编码在退出码里。
前面 21 讲的自己的程序都是这样做的，官方示例里的这类问题做成了 patch（0009、0095、0096、0100、0102、0103 等）。

## 第 5 步：编译用的 FFmpeg 和运行时加载的，是同一套吗

```console
$ $build/version_info
库            编译时头文件 运行时的库 结论
libavutil      61.20.100    61.20.100    一致
libswresample  7.7.100      7.7.100      一致
libswscale     10.6.100     10.6.100     一致
libavcodec     63.28.100    63.28.100    一致
libavformat    63.18.100    63.18.100    一致
libavfilter    12.12.100    12.12.100    一致

版本字符串（av_version_info）：git-2026-11-11-a337e8d
许可证（avcodec_license）：GPL version 2 or later
  含 GPL 部分：是（整个库按 GPL 发布）；含 nonfree：否；LGPL 版本 3：否
configure 参数（avcodec_configuration）：
  --prefix=/root/ffmpeg_build --enable-gpl --enable-libx264 --enable-libfreetype …
```

`version_info -q` 只在有问题时输出，退出码 0 没问题、1 有问题，可以放进构建脚本或 CI（`regress.sh` 就放了）。

**为什么要关心。**

- 编译时用的是**头文件**（`-I` 指定的目录）里的声明，运行时加载的是 **.so**（`LD_LIBRARY_PATH`、rpath 或系统目录里找到的）。两者可以不是一套：
  机器上同时装了系统自带的 FFmpeg 和自己编译的，`PKG_CONFIG_PATH` 指向一个，`LD_LIBRARY_PATH` 指向另一个，程序能编译、能启动，
  直到调用到一个新版本才有的函数（`undefined symbol`），或者结构体的布局对不上（崩溃或者输出乱码）。
- **同一个大版本号（major）里，库保证二进制兼容**（只加不改）：运行时的库比头文件新可以，比头文件旧不行（可能缺函数）。大版本号不同就是不兼容，所以 SONAME 里带着它（`libavutil.so.61`）。
  这台机器上自带的 FFmpeg 6.1 是 `libavutil.so.58`，和我们的 `.so.61` 名字不同，装在一起也不会互相顶替。（**所以文章开头说了：同一个大版本、旧库配新头文件的“现场”，这台机器做不出来。**）
- **许可证决定你能不能发布自己的程序**：LGPL 的库可以动态链接进闭源程序；带了 `--enable-gpl` 的库（上面的 `--enable-libx264` 要求它）整个就是 GPL；`--enable-nonfree` 的库不能再分发。
  `avcodec_license()` 和 `avcodec_configuration()` 就能在运行时查到，不用去翻 `ffmpeg -version`。
- **用编码器、功能前先查有没有**：`version_info` 最后列的“运行时能查到的编码器”（`libx265`、`libfdk_aac`、`libmp3lame`、`libvpx-vp9` 都没有，`libx264`、`h264_vaapi` 有）是 `avcodec_find_encoder_by_name()` 的结果。
  **自己的程序不应该假设编码器存在**（L13、L21 都是先查再用）。
- **按版本号选择接口**：`doc/APIchanges` 里记着每个接口是哪个版本加的。`avio_open_dir2()` 是 lavf 63.11.100 加入的、`avfilter_graph_set_max_buffered_frames()` 是 lavfi 12.11.100 加入的；
  用 `#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(63, 11, 100)` 在编译时选，`version_info` 里“按版本号选择接口”一节演示了。

检查一个可执行文件到底会加载哪套库：`ldd 程序 | grep libav`，`readelf -d 程序 | grep -E 'NEEDED|RUNPATH|RPATH'`。

## 第 6 步：回归

“改了代码，有没有改坏别的？”回归就是把前面所有的检查**一条命令**跑完：

```bash
L22_engineering/regress.sh            # 全部：编译、版本检查、ASan 扫描、各种对比、没有显卡的硬件检查、单元测试，约 2 分钟
L22_engineering/regress.sh quick      # 跳过 ASan 扫描和 CMake 构建，约 75 秒
L22_engineering/regress.sh valgrind   # 把 ASan 扫描换成 valgrind，6 分钟以上
```

每一步的输出在 `out/l22_regress/<步骤>.log`。这台机器上的结果（全部通过，13 步，112 秒）：

| 步骤 | 做什么 | 通过的标准 |
| --- | --- | --- |
| 编译全部 | 各讲的 Makefile | 全部编译 |
| 版本检查 | `version_info -q` | 头文件和库一致 |
| 错误码演示、日志多线程 | `err_codes demo`、`log_demo threads 8 2000` | 退出码 0；16000 行一行不少 |
| 扫描_ASan | `check_all.sh`（第 1 步） | 416 项，0 失败 |
| L16 转码对比命令行 | `regress_transcode.sh`（下面） | 16 项，0 不同 |
| L17 转码对比命令行 | `compare_cli.sh` | 和命令行不同的恰好是已知的那 5 项 |
| L18 自定义 I/O 对比 | `compare_file.sh` | 28 项相同，11 项已知，0 不同 |
| L19 目录对比、服务和读取对比 | `compare_ls.sh`、`compare_serve.sh` | 11 项和 40 项相同，另有 3 项和 6 项是已知的、符合预期的差别，0 不同 |
| L20、L21 没有显卡的检查 | 各自的 `test_nodevice.sh` | 43 项、52 项全部通过 |
| CMake 构建和 ctest | 用 CMake 把整个仓库编译一遍，再跑 L22 的 ctest | 编译成功，ctest 通过 |

**每一项“算通过”的标准写死在脚本里，不是“看着差不多”。** 有两处是“已知的不同”，同样写死：
L17 的 `compare_cli.sh` 里和命令行不同的恰好是 `ac3 enc_flac out_mkv video_flv wma` 这 5 项（原因在 L17 README 里）：**多了或者少了都算失败**——
少了说明某个问题被修好了（要去更新这张表），多了说明改坏了。已知的问题要“被盯着”，而不是“被忽略”。

### regress_transcode.sh：两种“相同”的标准

L16 的 `transcode_av` 和 `ffmpeg` 命令行转码同一个输入，输出要相同。**“相同”有两个层次，从严到宽：**

- **字节相同**：两个输出文件 `cmp` 逐字节一样。
- **逐包相同**：两个文件里每一个包的流序号、pts、dts、时长、大小、校验和都相同（`ffmpeg -i 文件 -c copy -f framecrc -`）。容器里除了包以外的东西不比。

脚本先比字节，比不出才比逐包，而且每一项**事先**写明用哪个标准，所以“为什么用宽的标准”是写在脚本里、可以检查的，不是随手放宽的：

```console
$ L22_engineering/regress_transcode.sh
== 不同的输入，输出 MP4（L16 第 4 步）
字节相同  in_mp4        字节相同  in_mkv        字节相同  in_ts        字节相同  in_flv
== 不同的输出格式
字节相同  out_mov
字节相同  out_avi
逐包相同  out_mkv               （文件字节不同：568785 对 568800）
逐包相同  out_flv               （文件字节不同：509510 对 509530）
逐包相同  out_ts                （文件字节不同：576220 对 575468）
== 滤镜、裁剪、编码参数
字节相同  scale_copya  fps10  af_volume  crf18_fast  faststart  cut_exact  cut_copy_va

共 16 项：字节相同 13，逐包相同 3，不同或失败 0
```

（上面为了排版把几行并成了一行，实际输出每项一行。）13 项字节相同：4 种输入、MOV、AVI、缩放加拷贝音频、`fps=10`、`volume`、`crf` 加 `preset`、`faststart`、精确裁剪、拷贝裁剪。

**三个只能比到逐包的输出格式，原因都查清楚了：**

| 输出 | 文件字节差 | 原因 |
| --- | --- | --- |
| FLV | 命令行多 20 字节 | 命令行的 `onMetaData` 里多一个 `framerate`（2+9+1+8 = 20 字节：名字的长度、`framerate` 九个字符、类型、一个 double）：命令行把输出流的 `avg_frame_rate` 设了，`transcode_av` 没设。 |
| TS | 命令行少 752 字节（4 个 TS 包）；不加 `max_delay` 时所有时间戳还差 1.4 秒 | (1) `ffmpeg` 命令行默认把复用器的 `max_delay` 设成 0.7 秒，TS 复用器因此把所有时间戳加 2 倍的 0.7 秒；`transcode_av` 不设，第一个视频包是 pts 0.04、dts 0，命令行是 pts 1.44、dts 1.40。所以脚本里给 `transcode_av` 加 `max_delay=700000`（和 L17 里看到的“TS 的 max_delay 0.7 秒”是同一件事）。(2) 加了以后还差 752 字节，同样是 `avg_frame_rate` 没设（下面验证过，原因没有再追到 mpegts 复用器的代码里去）。 |
| MKV | 命令行多 15 字节，而且内容也不同 | 15 字节：(1) 命令行的视频轨多一个 `DefaultDuration`（8 字节，`02625a00` 是 40,000,000 纳秒，就是每帧 1/25 秒，也来自 `avg_frame_rate`）；(2) 多两个元素：`FlagInterlaced`（3 字节，值 2 是逐行）和颜色范围 `Range`（4 字节，值 1 是有限范围），命令行从解码出来的帧里知道，`transcode_av` 没设。内容不同：`SegmentUID`、每条轨的 `TrackUID` 都是**随机的**（长度一样、值不一样），还有包住它们的 CRC-32：`ffmpeg` 命令行自己运行两次，这几处也不相同，所以 MKV 永远做不到字节相同。 |

怎么知道的？**不靠猜，一个一个量出来的**：`cmp -l` 找出第一个不同的字节，在 `od` 里看是哪个元素；FLV 用 `strings` 看 `onMetaData`；TS 用 `ffprobe -show_packets` 看时间戳；
MKV 用本讲的 `ebml_dump.py` 把头部的元素列出来，两个文件各列一遍，`diff` 就是上面表格里的内容：

```console
$ python3 L22_engineering/ebml_dump.py out/l22_transcode/out_mkv.mkv     > mine.txt
$ python3 L22_engineering/ebml_dump.py out/l22_transcode/out_mkv_cli.mkv  > cli.txt
$ diff mine.txt cli.txt
2c2
< Segment 568745
> Segment 568760
…
20c20,21
<       Video 21
>       DefaultDuration 8 02625a00
>       Video 28
23c24,26
<         Colour 11 55b7810155b88102
---
>         FlagInterlaced 3 02
>         Colour 15 55b9810155b7810155b88102
>           Range 4 01
```

（中间是随机的 `SegmentUID`、`TrackUID` 和三处 CRC-32，每次运行都不同，这里省略。）

然后做一个验证：**在 `transcode_av` 里给输出流加上两行 `avg_frame_rate`**（临时的试验副本，没有改 L16；两行的位置在练习 6），结果：

```console
共 16 项：字节相同 15，逐包相同 1，不同或失败 0
```

FLV 变成字节相同；TS（同时带 `max_delay=700000`）也变成字节相同；MKV 的差从 15 字节变成 7 字节（568793 对 568800），只剩 `FlagInterlaced` 和 `Range`，以及随机的那几处。
这两行**没有**并进 L16 的代码：它是一个有意义的改进，但 L16 已经讲完了，改它要连着讲稿一起改，留作练习 6。

这一节要带走的方法：**“一致”不是非黑即白的。** 先定好标准（字节、逐包、解码后逐帧），每一项事先写明用哪一个，比不出的时候**去查为什么**，而不是把标准放宽就算了。

## 常见错误

| 现象 | 原因 | 怎么办 |
| --- | --- | --- |
| `make CFLAGS=-fsanitize=address` 编译失败，找不到头文件 | 命令行上的 `CFLAGS` 盖掉了 Makefile 里的 `-std` 和 pkg-config 的路径 | 用 `SANITIZE=address,undefined`，或者环境变量 `CFLAGS=… make` |
| ASan 起不来，说无法保留内存 | 设了 `ulimit -v` | 去掉 |
| ASan 报“泄漏”，但程序明明释放了 | 多半是**出错路径**没释放，或者第三方库（libx264）自己泄漏 | 看报告里的分配栈：在你的代码里就修，在 `.so` 里就记进已知问题 |
| TSan 报告里全是库内部的栈 | 没有给 TSan 加抑制，或者库本身没有用 TSan 构建 | 只看栈里有你的代码的；确认是真竞争后再处理 |
| 重建滤镜图后输出的尺寸还是变了 | 新图的出口没有加固定参数的 `scale`、`aformat` | 描述末尾加固定出口（第 2 步） |
| 日志回调里的统计偶尔不对 | 回调里的共享状态没加锁 | 第 3 步 |
| `av_err2str()` 在 C++ 里编译不过 | 复合字面量取地址 | 自己准备缓冲区，用 `av_strerror()` |
| 脚本里 `if [ $? -eq 187 ]` 之类 | 用错误码当退出码 | 自己程序的退出码只用 0/1/2，原因写 stderr（第 4 步） |
| 程序能编译能启动，调用到某个函数时崩溃、`undefined symbol` | 编译用的头文件比运行时的库新 | `version_info`、`ldd`（第 5 步） |
| 和命令行对比，只有 MKV、TS、FLV 不一样 | 容器层的差别（随机 UID、复用器延迟、`avg_frame_rate`） | 比逐包（第 6 步） |

## 练习

1. **ASan 报告**。在 `err_codes.c` 的 `demo()` 里给 `big` 数组越界写一个字节（`big[AV_ERROR_MAX_STRING_SIZE + n] = 1;`，`n` 是个 `volatile int`，否则编译器先给警告），用 `make SANITIZE=address,undefined BUILD=build-asan` 构建并运行，读报告的三段。
2. **重建**。用 `reconfig_graph` 加一个 `scale=-2:360` 的滤镜图（输入 320x180 和 640x360 混合），输出是多大？为什么？再把它改成 `reconfig=0`，输出有什么变化？
3. **日志**。改 `log_demo`，让它在解码失败时只倒出级别是 `warning` 和 `error` 的行（不要 debug）。要改 `fflog.h` 的什么地方？
4. **错误码**。写一个 `exit_code_of`：给一个 `AVERROR` 值，返回 1（一般错误）或者 2（用法错误），并且不要用错误码当退出码。你的程序里“输入文件不存在”应该是 1 还是 2？为什么？
5. **版本**。给 `version_info` 加一个 `-v 6.1` 之类的选项，检查运行时的 libavformat 主版本号是不是不低于给定的 FFmpeg 版本对应的。（提示：`doc/APIchanges` 里能查到 FFmpeg 版本和库版本的对应。）
6. **补上 `avg_frame_rate`**。在 `L16_transcode/transcode_av.c` 的 `add_stream()` 里，`if (s->copy) { … } else { … }` 的后面加两行：
   ```c
   if (s->type == AVMEDIA_TYPE_VIDEO && s->enc->framerate.num > 0)
       s->ost->avg_frame_rate = s->enc->framerate;
   ```
   重新运行 `regress_transcode.sh`：哪几项从“逐包相同”变成“字节相同”？还要给 TS 加什么参数？MKV 为什么仍然不同？（想一想颜色范围和 `field_order` 该从哪里来。）
7. **加一项回归**。给 `regress.sh` 加一步：L13 的 `encode_yuv` 的输出和 `ffmpeg` 命令行逐字节比较（L13 README 里有命令）。通过标准是什么？

### 练习的参考答案

1. 实际的报告开头是 `ERROR: AddressSanitizer: stack-buffer-overflow on address 0x…`、`WRITE of size 1 at 0x… thread T0`，下面是调用栈，第一行就是你写的那一行（`demo` 被内联进 `main`，所以栈里 `demo` 和 `main` 的地址一样）；
   再下面一段 `Address 0x… is located in stack of thread T0 at offset 384 in frame`，列出这个栈帧里的所有局部变量和各自的范围（`[64, 72) 'small'` 之类），越界的地址落在哪个变量的后面，就是哪个变量写穿了。栈上的变量没有“freed by”，那一段只有堆上的错误才有。
2. 输出是 `640x360`，50 帧共 17,280,000 字节（每帧 345600）。原因：出口固定的是**第一张图**的输出尺寸；第一张图的输入是 320x180，`scale=-2:360` 把它放大到 640x360，
   所以后面所有帧（包括第二段本来就是 640x360 的）都固定成 640x360。`reconfig=0` 时输出**逐字节相同**：`scale` 自己适应了输入尺寸的变化，只多一条警告。
   换成 `hflip` 就不一样了：第 26 帧 `EINVAL`，只留下前 25 帧。所以这个练习里没有出错，不代表不需要重建，要看滤镜图里是什么滤镜。
3. 给 `fflog_dump_recent()` 加一个最低级别参数，打印时跳过比它更详细的行：环形缓冲区里每行已经记了级别（`ring_level`），打印时比较就行。
4. 用法错误（参数个数不对、选项不认识）是 2；输入文件不存在是**运行时的错误**，是 1，原因写在 stderr（`av_strerror(AVERROR(ENOENT))` 给出 `No such file or directory`）。区分“我用错了”和“环境有问题”对脚本有用。
5. `LIBAVFORMAT_VERSION_MAJOR` 是编译时的，`avformat_version() >> 16` 是运行时的，比较两个就能知道是不是“运行时比头文件旧”（要报警）。
6. 运行结果是“共 16 项：字节相同 15，逐包相同 1”：FLV 变成字节相同；TS 在脚本已经带着 `max_delay=700000` 的前提下也变成字节相同（不带 `max_delay` 时时间戳仍差 1.4 秒）。
   MKV 仍然不同：随机的 `SegmentUID`、`TrackUID`（和包住它们的 CRC-32）永远不同，另外还差 7 字节（568793 对 568800）：`FlagInterlaced` 和颜色范围 `Range`。
   这两个值要从解码后的帧里取（`frame->color_range`、`frame->flags & AV_FRAME_FLAG_INTERLACED`），再通过 `codecpar` 或者流的字段交给复用器；`transcode_av` 没取。（改完再用 `ebml_dump.py` 对一遍，看差是不是消失了。）
7. 通过标准：字节相同。`encode_yuv` 的 `threads=1` 默认，命令行加 `-threads 1`；输出同样是裸码流或同一个容器。比不出来时，先看是不是线程数和 `sample_aspect_ratio` 这两个老问题（L13、L21）。

## 对前面和后面的交代

- **兑现的话**：L12 说的“输入参数中途变化时重建滤镜图”（第 2 步，并回到 L11 第 6 步的“入口格式固定”）；L17、L20、L21 说的“负的错误码取低 8 位、187 的来历、完整的错误码表”（第 4 步）。
- **课程 L2 到 L21 里，各讲的自带检查**（`test_nodevice.sh`、`compare_*.sh`）现在都被 `regress.sh` 串起来了。以后改任何一讲的代码，先跑 `regress.sh quick`。
- **接下来**：综合项目——把前面学的接口组合成一个命令行工具（mtool）。它沿用这一讲的做法：用 ASan 构建开发，错误原因写 stderr、退出码只用 0/1/2，版本和许可证用 `version_info` 的办法查，改完跑 `regress.sh`。
