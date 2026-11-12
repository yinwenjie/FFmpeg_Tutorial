# L20 硬件解码

前面 19 讲，帧都在内存里：`AVFrame` 的 `data[0]` 指向一块 CPU 能读的像素。这一讲换一种帧：解码在显卡上做，解出来的画面留在**显存**里，
`AVFrame` 里放的不再是像素地址，而是显存里“表面”的句柄。这样的帧 CPU 读不了，要读就得先**搬回内存**。
围绕这件事，FFmpeg 有一套自己的概念：设备上下文、帧上下文、`get_format` 回调、`av_hwframe_transfer_data()`。

官方示例一个：`hw_decode.c`。自己写的程序两个：`hw_info`（这台机器、这份 FFmpeg 能做哪些硬件解码）、
`hw_decode_av`（会自动选设备、没有显卡时退回软件解码、能指定输出格式、会统计耗时）。外加两个脚本：
`test_nodevice.sh`（没有显卡也能做的检查）、`verify_hw.sh`（有显卡的机器上跑，把结果补进这份 README）。

> **这一讲有一个特别的限制，请先读完这一节。**
> 写这一讲的机器没有显卡（`/dev/dri` 不存在），所以**硬件解码本身没有在这台机器上跑过**。
> 这份 README 里凡是要显卡才看得到的输出，都标成了 **【待补 N】**，不编造任何数字；其余全部是在这台机器上实际跑出来的。
> 没有显卡的人（包括写这一讲的人）能学到的：概念、API 的调用顺序、怎么在没有设备时让程序正确地失败或者退回软件解码、
> 软件路径的全部细节、这一路上找到的官方示例的问题。有显卡的人，运行 `verify_hw.sh`，它会把【待补 1】到【待补 5】
> 全部跑一遍，写成 `out/l20/results.md`（第 8 步）。录课时如果用显卡机器录，也请直接跑这个脚本。

完成标志：说得清楚硬件帧和内存帧的区别，知道为什么硬件帧不能直接当图像读；
能说出“FFmpeg 编译时带了没有、解码器支持不支持、机器上有没有设备、设备支持不支持这个视频”这四层各用什么 API 查；
看得懂 `hw_decode.c` 里设备、`get_format`、搬运这三个部分的作用；
会在没有设备时让程序报出能看懂的原因并以 1 退出，不泄漏；
`test_nodevice.sh` 全部通过；有显卡的话，`verify_hw.sh` 跑完，结果补进了【待补】。

## 本讲要点

**硬件帧和内存帧**

```text
软件解码：   包 ──► [CPU 解码] ──► AVFrame（format=yuv420p）：data[0] 指向内存里的 Y 平面
                                      │ 想怎么读就怎么读：写文件、sws_scale、送进滤镜

硬件解码：   包 ──► [GPU 解码] ──► AVFrame（format=vaapi）：data[3] 里是显存表面的句柄，data[0] 没有像素
                                      │                         帧的 hw_frames_ctx 指向“这些表面是从哪个池子里来的”
                                      ├─ 要读像素：av_hwframe_transfer_data() 拷回内存，得到 format=nv12 的帧
                                      └─ 不读：把句柄交给下一个硬件步骤（缩放、编码），帧一直留在显存里（L21）
```

`format=vaapi` 不是一种像素排列，是一个**标记**：“这一帧在 VAAPI 的表面里”。所以对硬件帧调用 `sws_scale()`、`av_image_copy_to_buffer()`
都没有意义，得先搬回来。搬回来的格式由硬件帧上下文决定：VAAPI 通常是 `nv12`（Y 平面加一个 UV 交错的平面，L9 说过），能传成哪些用
`av_hwframe_transfer_get_formats()` 问（第 5 步）。

**四层：每一层都可能没有**

| 层 | 问题 | 怎么查 | 要显卡吗 |
| --- | --- | --- | --- |
| 1 | 这份 FFmpeg 编译时带了哪几种设备类型 | `av_hwdevice_iterate_types()` | 不要 |
| 2 | 这个解码器支持哪几种 | `avcodec_get_hw_config(codec, i)` 一项一项问 | 不要 |
| 3 | 机器上有没有这种设备，能不能打开 | `av_hwdevice_ctx_create()` | **要** |
| 4 | 设备支持这个视频的格式、profile、尺寸吗 | `av_hwdevice_get_hwframe_constraints()` 能问个大概；最终要等解码器在 `get_format` 里表态 | **要** |

前两层是 FFmpeg 这份库的事，查得出来就说明“代码能走到这里”；后两层才是机器的事。写硬件解码的程序，出问题先判断是哪一层：
编译时没带（换一份 FFmpeg）、解码器没有这种配置（换设备类型或者软件解码）、没有设备（驱动、权限、虚拟机）、设备不支持这个视频（退回软件）。

**设备上下文和帧上下文**

| 对象 | 是什么 | 谁创建 |
| --- | --- | --- |
| `AVHWDeviceContext` | 一个设备的会话：VAAPI 是一个打开的 `/dev/dri/renderD128`（加一个 `VADisplay`），cuda 是一个 CUDA 上下文 | 我们：`av_hwdevice_ctx_create()` |
| `AVHWFramesContext` | 一个设备上的一个**表面池**：宽、高、硬件格式（vaapi）、内存格式（nv12）、池子里有几个表面 | 解码器自己建（我们给了设备上下文的话）；L21 编码时我们自己建 |
| `AVFrame.hw_frames_ctx` | 每个硬件帧都带一个引用，指向它所在的池子 | 解码器 |

两个对象都是 `AVBufferRef`（引用计数，前面各讲里 `AVPacket`、`AVFrame` 的数据缓冲区底下也是它）：设备要等解码器和我们都放手才真正释放。

**`AVCodecHWConfig` 的“方法”**

`avcodec_get_hw_config()` 返回的每一项有 `device_type`、`pix_fmt`（这种配置下帧的硬件格式）、`methods`：

| 方法 | 意思 |
| --- | --- |
| `HW_DEVICE_CTX` | 给解码器一个设备上下文，它自己建帧池。最常用，官方示例用的就是它 |
| `HW_FRAMES_CTX` | 我们自己建帧上下文交给解码器 |
| `INTERNAL` | 解码器自己搞定一切，不用设置什么（名字带后缀的专用解码器是这种） |
| `AD_HOC` | 需要额外的设置或调用，按设备类型不同而不同 |

**两条路：hwaccel 和专用解码器**

同一个 H.264，FFmpeg 里有两种用硬件的办法：一是用**普通的 `h264` 解码器加 hwaccel**（本讲的做法，`avcodec_get_hw_config()` 查得到），
解析、参考帧管理还是 FFmpeg 自己做，只把“算像素”交给显卡；二是用名字带后缀的**专用解码器**（`h264_cuvid`、`h264_qsv`、`h264_v4l2m2m`……），
整个解码交给厂商的库，和软件解码器一样用 `avcodec_find_decoder_by_name()` 找，输出格式也是它自己定。本机编译进来的专用解码器只有 `v4l2m2m` 一族（ARM 板子上的），
没有设备时和 hwaccel 一样打不开（第 2 步末尾）。后面各讲只讲 hwaccel 这一条。

**硬件解码不一定更快**

要说“更快”，得先说比什么。通常的收益是：**CPU 空出来**（解码交给显卡，CPU 时间大幅下降）、**省电**、**同时解很多路**。不一定有的收益是“这一路解得更快”：
小视频启动设备就要时间；每一帧都要等显卡解完、再通过总线搬回内存（第 5、6 步）；搬回来以后如果还要转格式（nv12 转 yuv420p、转 RGB），CPU 又忙起来。
最划算的用法是**根本不搬回来**——帧一直留在显存里，缩放、编码都在显卡上做，这是 L21 的内容。

## 准备

```bash
export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig PATH=$HOME/ffmpeg_build/bin:$PATH LD_LIBRARY_PATH=$HOME/ffmpeg_build/lib
cd L20_hw_decode
make                                  # 不需要显卡就能编译
S=../samples
```

素材用 `scripts/gen_samples.sh`（见 L02）生成的那几个：`small_360p25.mp4`（H.264，640×360，125 帧）、同内容的 `.mkv`、`.ts`、`.flv`，
`demo_1080p30.mp4`（H.264，1920×1080，300 帧），`video_360p25.m1v`（MPEG-1，硬件配置表里没有它，第 7 步用）。

本机环境：`/dev/dri` 不存在，FFmpeg 编译时带了 `--enable-vaapi`（所以设备类型里有 `vaapi` 和 `drm`），没有 CUDA、QSV 等。

## 第 1 步：hw_info，一次看清四层

```console
$ build/hw_info try=1
1. 这份 FFmpeg 编译进来的硬件设备类型（git-2026-11-11-a337e8d）：
   vaapi drm

2. 解码器的硬件配置表（avcodec_get_hw_config，不需要显卡就能查）：
  h264       vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  hevc       vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  vp8        vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  vp9        vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  av1        vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  mpeg2video vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  mpeg4      vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  vc1        vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
  mjpeg      vaapi    帧格式 vaapi    方法 设备上下文+帧上下文+需额外设置
   专用的硬件解码器（单独的解码器，名字带后缀）：
   有的： h264_v4l2m2m hevc_v4l2m2m vp9_v4l2m2m

3. 机器上的设备：
   /dev/dri 不存在：没有显卡，或者这是没有直通显卡的虚拟机、容器。VAAPI 和 DRM 肯定用不了。

4. 逐个创建设备（device=默认）：
   vaapi    创建失败：Generic error in an external library（-542398533）
   drm      创建失败：Bad address（-14）
```

读法：

- 第 1 层，本机有 `vaapi` 和 `drm`。`ffmpeg -hwaccels` 看到的是同一份名单。
- 第 2 层，常见的几个解码器各自只有一项 `vaapi`。MPEG-1 不在表里（`codec=mpeg1video` 看，是空的）：显卡一般不解它。
  `vaapi` 这一项的方法是三个位：给设备上下文就行、也可以自己建帧上下文、还要一些额外设置。
- 第 3 层，没有显卡。**有显卡的机器上这里会列出 `/dev/dri/renderD128`，并显示读写权限**：没有权限（没加入 `render` 用户组）是最常见的“有显卡却用不了”。
- 第 4 层，两个都创建失败，错误是 `AVERROR_EXTERNAL`（-542398533）和 `EFAULT`（-14）。加 `verbose=1` 能看到原因（下一步）。

**【待补 1】有显卡的机器上的输出**：运行 `verify_hw.sh`，“待补 1”一节就是这条命令的输出。有设备时第 4 层会多出“创建成功”以及这个设备支持的硬件帧格式、能传回内存的格式、尺寸范围。
回来补进这里：_（待补）_

## 第 2 步：官方示例 hw_decode，没有设备时会怎样

`hw_decode.c` 的用法是 `hw_decode <设备类型> <输入> <输出>`，把解出来的帧写成原始像素文件。`make` 已经编译了本目录的拷贝，
它是打了三个 patch（0096 到 0098）的版本，代码和讲解见第 3 步。先看没有设备时的现象：

```console
$ build/hw_decode vaapi $S/small_360p25.mp4 /tmp/x.nv12
Failed to create specified HW device: Generic error in an external library
Raise the log level (av_log_set_level(AV_LOG_VERBOSE)) to see the reason reported by the hardware backend.
$ echo $?
1
```

第一行是 patch 0097 加的：原版只说 `Failed to create specified HW device.`，没有错误码，也没有原因。原因在 verbose 级别的日志里，用 `hw_decode_av` 看：

```console
$ build/hw_decode_av $S/small_360p25.mp4 - device=vaapi loglevel=verbose 2>&1 | grep -i "render\|VA display"
[VAAPI @ 0x…] No render device /dev/dri/renderD128, try next device for DRM render node.
[VAAPI @ 0x…] No render device /dev/dri/renderD129, try next device for DRM render node.
… （一直试到 renderD134）
[VAAPI @ 0x…] No available render device for DRM render node.
[VAAPI @ 0x…] Cannot open a VA display from DRM device (null).
```

这一段说明两件事：创建设备时设备名传 `NULL`，VAAPI 会从 `renderD128` 开始，往后试 7 个渲染节点；
有两块显卡、想用第二块时，要自己传设备名（`hw_decode_av` 的 `device_path=`，对应 `av_hwdevice_ctx_create()` 的第 3 个参数）。

**原版和修复后的退出码**（patch 0096）。原版的 `main()` 在每个出错的地方 `return -1`，退出码是 255，而且什么都不释放：

| 命令 | 原版退出码 | 修复后 |
| --- | --- | --- |
| `hw_decode vaapi small_360p25.mp4 out`（没有设备） | 255，泄漏 | 1 |
| `hw_decode bogus …`（不认识的设备类型） | 255 | 1 |
| `hw_decode vaapi video_360p25.m1v out`（MPEG-1 没有 vaapi 配置） | 255，泄漏 | 1 |
| `hw_decode vaapi /nonexistent out` | 255，泄漏 | 1 |
| `hw_decode vaapi audio_44k_stereo.mp2 out`（没有视频流） | 255 | 1 |
| `hw_decode vaapi`（缺参数） | 255 | 1 |

“泄漏”是用 ASan 构建（`make SANITIZE=address,undefined BUILD=build-asan`）看到的：没有设备那一条泄漏了 880+104 字节直接泄漏和 353125 字节间接泄漏
（包、格式上下文、解码器上下文，以及它们引用的全部内存）。修复后这六条都不泄漏。

这里还有一个原版的隐患，在没有设备时看不到：开始解码以后，`main()` 最后无条件 `return 0`，读包、解码、写文件出错都是“成功”。patch 0096 一起改了。
这部分只能在有设备时验证，所以 patch 说明里写明了“解码路径没有运行”。

**和 ffmpeg 命令行比一比。** 命令行有两种用法，效果不一样：

```console
$ ffmpeg -hide_banner -v error -hwaccel vaapi -i $S/small_360p25.mp4 -f null -
Device creation failed: -542398533.
[vist#0:0/h264 @ …] [dec:h264 @ …] No device available for decoder: device type vaapi needed for codec h264.
[vist#0:0/h264 @ …] [dec:h264 @ …] Hardware device setup failed for decoder: Generic error in an external library
Error opening output files: Generic error in an external library
$ echo $?
187

$ ffmpeg -hide_banner -v error -hwaccel auto -i $S/small_360p25.mp4 -f null -
Device creation failed: -542398533.
$ echo $?
0
```

`-hwaccel vaapi` 是“我要用这个”：用不了就失败，退出码 187（`AVERROR_EXTERNAL` 的低 8 位：和 L17 里 `AVERROR_EXIT` 的 187 是同一个现象，负的错误码做退出码，只剩低 8 位；L22 的错误码表会把这件事讲全）。
`-hwaccel auto` 是“能用就用”：用不了只打印一行，继续用软件解码，退出码 0。`hw_decode_av` 的 `device=` 照着这两种做：写设备类型名是前者，`auto` 是后者，
写了类型名又想退回软件，加 `fallback=1`。

专用解码器也一样，只是错误信息不同：

```console
$ ffmpeg -hide_banner -v error -c:v h264_v4l2m2m -i $S/small_360p25.mp4 -f null -
[h264_v4l2m2m @ …] Could not find a valid device
[h264_v4l2m2m @ …] can't configure decoder
… Error while opening decoder: Invalid argument
```

**【待补 2】有设备时官方 hw_decode 的运行结果**：`hw_decode vaapi small_360p25.mp4 out.nv12` 的输出、退出码，`out.nv12` 的大小（预期 125 帧 × 345600 = 43200000 字节，如果传回来的是 nv12），
以及它和软件解码传成 nv12 的结果是不是逐字节一致。`verify_hw.sh` 的“待补 2”一节：_（待补）_

## 第 3 步：读 hw_decode.c

用编辑器打开 `hw_decode.c`，上面的“课程注释”逐段讲了。骨架是这样：

```text
main()
 ├─ av_hwdevice_find_type_by_name()        "vaapi" → AV_HWDEVICE_TYPE_VAAPI；不认识就列出这份 FFmpeg 支持的
 ├─ 打开输入，av_find_best_stream() 选出视频流和解码器（和 L07 一样）
 ├─ for (i = 0;; i++) avcodec_get_hw_config()   ← 第 2 层：解码器支持这种设备吗？记下 config->pix_fmt（硬件帧格式）
 ├─ avcodec_alloc_context3() + avcodec_parameters_to_context()
 ├─ decoder_ctx->get_format = get_hw_format      ← 解码器问“用哪种像素格式”时，回答硬件格式
 ├─ hw_decoder_init(): av_hwdevice_ctx_create()  ← 第 3 层：创建设备，av_buffer_ref() 交给解码器
 ├─ avcodec_open2()
 └─ 读包 → decode_write() → 冲刷
decode_write()
 ├─ avcodec_send_packet() / avcodec_receive_frame()    （和 L07 一样的循环）
 ├─ frame->format == hw_pix_fmt ？ av_hwframe_transfer_data(sw_frame, frame, 0) 搬回内存 : 直接用
 └─ av_image_copy_to_buffer() 拷成紧密排列 → fwrite()
```

和 L07 的软件解码比，多出来的只有三处：**查配置**、**设备**、**搬运**，`get_format` 是把它们连起来的那一环：

1. 解码器开始解第一帧之前，调用 `get_format`，把能用的像素格式列成一个以 -1 结尾的数组。
   如果显卡能解这个视频，硬件格式（`vaapi`）排在前面，后面跟软件格式（`yuv420p`）。
2. 回调里挑一个返回。挑了 `vaapi`，解码器就在显卡上解，用我们给的设备上下文建帧池，之后每个输出帧的 `format` 都是 `vaapi`。
3. **挑不到硬件格式**时（显卡不支持这个视频的 profile、位深或尺寸）：官方示例返回 `AV_PIX_FMT_NONE`，解码直接失败；
   `hw_decode_av` 挑第一个软件格式，让 CPU 来解（第 7 步）。

官方示例的其他毛病（patch 0096 到 0098 修的，没有显卡也能看出来）：

- 每个出错的地方 `return -1`：退出码 255、泄漏；开始解码以后退出码永远是 0（0096）；
- 创建设备失败不说原因（0097）；`av_buffer_ref()` 的结果没检查（0097）；
- `fwrite()` 的返回值和 `int` 比较（`-Wsign-compare`）；`av_image_get_buffer_size()` 出错返回负数，没检查就传给 `av_malloc()`（0098）。

## 第 4 步：hw_decode_av，先走软件路径

`hw_decode_av` 的选项：`device=auto|none|vaapi…`、`device_path=`、`fallback=`、`pix_fmt=`、`frames=`、`threads=`、`extra=`、`md5=1`、`stats=1`、`loglevel=`（文件开头的注释里有完整说明）。
输出文件写 `-` 表示不写文件（只解码）。先用 `device=none`，在没有显卡的机器上把整条路走一遍：

```console
$ build/hw_decode_av $S/small_360p25.mp4 - device=none md5=1 stats=1
解码器 h264，软件解码
md5=60b1eeb6c8e8b9013fd097033352fe72
解码方式：软件（CPU）
设备：无
帧数：125（硬件帧 0，内存帧 125）
输出：43200000 字节
耗时（墙钟）：0.110 秒，1134.1 帧/秒
CPU 时间（用户+系统，所有线程）：0.203 秒
搬运耗时（显存拷回内存）：0.000 秒
转换耗时（libswscale）：0.000 秒
```

`md5=` 是输出的全部字节的 MD5，不用写出 43 MB 的文件就能比。拿它和命令行比（`-fps_mode passthrough` 的原因在下面）：

```console
$ ffmpeg -v error -i $S/small_360p25.mp4 -fps_mode passthrough -f rawvideo - | md5sum
60b1eeb6c8e8b9013fd097033352fe72  -
$ ffmpeg -v error -i $S/small_360p25.mp4 -fps_mode passthrough -f rawvideo -pix_fmt nv12 - | md5sum
72489c67…                          -               # hw_decode_av … pix_fmt=nv12 md5=1 是同一个值
```

`test_nodevice.sh` 把这个对照做全了：6 个素材（MP4、MKV、TS、FLV、MPEG-1、1080p）× 3 种输出（原样、`pix_fmt=nv12`、`pix_fmt=gray`），再加 `frames=10`、
`threads=1` 与 `threads=4` 一致、写出的文件和 `md5=` 一致，**全部逐字节相同**。所以软件这一侧的代码是可信的参照：之后有显卡时，硬件的输出和它比。

**`-fps_mode passthrough` 是怎么回事。** 不加的话，FLV 那一项比不上：

```console
$ ffmpeg -v info -i $S/small_360p25.flv -f rawvideo - 2>&1 >/dev/null | grep "frame="
frame=  126 … dup=1 drop=0 …
```

命令行默认按帧率“补帧、丢帧”：FLV 的时间戳只有毫秒精度，25 fps 的帧间隔 40 毫秒，凑巧有一个地方差了 1 毫秒，被它当成漏了一帧，补了一帧，一共 126 帧。
这是命令行的帧率同步（L7 里同样的现象），不是解码器的事。拿命令行当解码器的参照时，要关掉它：`-fps_mode passthrough`。

**出错和退回：全部是退出码 1，不泄漏。**

```console
$ build/hw_decode_av $S/small_360p25.mp4 - device=vaapi                 # 指定了设备类型，用不了：失败
用不了硬件设备：vaapi：Generic error in an external library
（加 fallback=1 可以退回软件解码，加 loglevel=verbose 看硬件后端报告的原因）
$ echo $?
1
$ build/hw_decode_av $S/small_360p25.mp4 - device=vaapi fallback=1 stats=1   # 退回软件
没有可用的硬件设备（vaapi：Generic error in an external library），改用软件解码
解码器 h264，软件解码
解码方式：软件（CPU）
…
$ build/hw_decode_av $S/small_360p25.mp4 - device=drm                   # h264 解码器没有 drm 这一项
用不了硬件设备：解码器 h264 没有drm的硬件配置
$ build/hw_decode_av $S/small_360p25.mp4 - device=bogus
用不了硬件设备：这份 FFmpeg 不认识设备类型 bogus（有： vaapi drm）
```

`device=auto`（不写 `device=` 就是它）对没有设备的机器等于 `fallback=1`。其余的出错路径（输入不存在、没有视频流、输出写不了、截断的 MP4、随机数据、不认识的选项）
也都是 1，`test_nodevice.sh` 在 ASan 构建下逐一检查过。

## 第 5 步：搬回内存的那一步

硬件解码拿到的帧要搬回内存时，有三件事要想：搬成什么格式、搬多少数据、搬完要不要转。

**搬成什么格式。** `hw_decode_av` 在第一次拿到硬件帧时问 `av_hwframe_transfer_get_formats()`，把答案打印出来，然后决定：
`pix_fmt=` 要的格式在答案里就直接传成它；不在，传成第一种，再用 libswscale 转。对 VAAPI，**第一种一般是 nv12**，这就兑现了 L9 说过的“硬件解码出来的是 nv12”：
不是显卡“只会”输出 nv12，而是 VAAPI 解码用的表面本来就是 nv12 排列（8 位 4:2:0），传回来不做转换，最便宜。
想要 `yuv420p`，先看答案里有没有；没有就要转，转要花 CPU（下面）。

设置的方法有个细节：`av_hwframe_transfer_data(dst, src, 0)` 的 `dst->format` 如果是 `AV_PIX_FMT_NONE`（`av_frame_alloc()` 之后和 `av_frame_unref()` 之后都是），就选第一种；
设了就用设的（必须是支持的格式之一）。所以在循环里复用目标帧时，每次 `av_frame_unref()` 之后都要重新设。

**【待补 3】真实硬件帧能传成哪些格式。** `verify_hw.sh` 的“待补 3”一节会打印 hw_decode_av 第一次拿到硬件帧时的那一行：

```text
硬件帧 vaapi 640x360，可以直接传回内存的格式： … ；…
```

_（待补）_

**搬多少数据。** 搬回内存的数据量，就是原始像素的大小。一帧的 nv12 是宽 × 高 × 1.5 字节：

| 分辨率 | 一帧 | 每秒（30 帧） | 每秒（60 帧） |
| --- | --- | --- | --- |
| 640×360 | 345,600 B | 10.4 MB | 20.7 MB |
| 1920×1080 | 3,110,400 B（3.1 MB） | 93.3 MB | 186.6 MB |
| 3840×2160 | 12,441,600 B（12.4 MB） | 373 MB | 746 MB |

1080p 素材 300 帧，输出 933,120,000 字节，和 `3,110,400 × 300` 对得上（第 4 步的统计里就是这个数）。
这些数据要从显存经总线拷到内存，CPU 还要把它们写进文件或者送去下一步。这就是为什么“硬件解码 + 搬回内存 + 软件编码”这条路经常**不比纯软件快多少**，
而“硬件解码 + 硬件编码，帧一直留在显存里”才是能发挥显卡的用法（L21）。

**搬完要不要转。** 转格式也是有成本的，本机上 1080p、300 帧（软件解码，输出转换，不用显卡）：

```console
$ build/hw_decode_av $S/demo_1080p30.mp4 - device=none stats=1 pix_fmt=nv12 | grep "转换\|输出"
输出：933120000 字节
转换耗时（libswscale）：0.098 秒
$ build/hw_decode_av $S/demo_1080p30.mp4 - device=none stats=1 pix_fmt=rgb24 | grep "转换\|输出"
输出：1866240000 字节
转换耗时（libswscale）：0.312 秒
```

yuv420p 转 nv12（只是把 U、V 两个平面交错）每帧约 0.33 毫秒；转 RGB24（每个像素算三个分量，数据量翻倍）每帧约 1 毫秒。
所以：要显示、要送编码器的话，尽量让两边用同一种格式；要截图、要 RGB，才付这个代价。

## 第 6 步：软件解码的速度，和硬件要比什么

`stats=1` 给出四个数：墙钟时间、CPU 时间（用户态加内核态，所有线程加起来）、搬运耗时、转换耗时。
本机（4 核，没有显卡）软件解码 1080p 300 帧，每种跑 3 次取居中的：

| 方式 | 墙钟（秒） | 帧/秒 | CPU 时间（秒） |
| --- | --- | --- | --- |
| 软件，1 线程（`threads=1`） | 1.340 | 223.8 | 1.336 |
| 软件，自动线程 | 0.649 | 462.6 | 1.502 |

读法：

- 1 线程时，墙钟和 CPU 时间几乎一样：解码器一直在忙。自动线程时，墙钟减半，CPU 时间反而略多（线程之间同步要花一点）。
  **“CPU 时间”才是硬件解码要省的东西**：显卡解码时，CPU 时间应当降到只剩读包、搬运、写文件的那一点。
- 速度对比不能只看墙钟。软件解码 1 线程已经有 220 帧/秒，对一个 30 帧/秒的视频，“更快”没有意义；
  真正有意义的是：同时解 10 路、4K、HEVC/AV1 这类软件解起来很重的格式时，CPU 能不能腾出来干别的。

**【待补 4】硬件解码的数字。** `verify_hw.sh` 的“待补 4”一节会在同一个素材上量三行：软件 1 线程、软件自动线程、硬件（传成 nv12）。
补进来以后，对照三件事：硬件那一行的 CPU 时间是不是明显低于软件；墙钟有没有变化；“搬运耗时”占墙钟的多大比例。_（待补）_

## 第 7 步：显卡解不了的视频，退回软件

`get_format` 回调里选不到硬件格式，有三个典型的原因：视频的 profile 太新或太旧（10 位 H.264、Main 4:4:4）、尺寸超出显卡的限制、显卡根本不支持这个编码。
官方示例这时返回 `AV_PIX_FMT_NONE`，解码失败。`hw_decode_av` 退回软件格式，并记下来：

```c
static enum AVPixelFormat get_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    Decoder *d = ctx->opaque;                       // 自己的数据放在 opaque 里，回调不用全局变量

    for (p = fmts; *p != AV_PIX_FMT_NONE; p++)      // 先找硬件格式
        if (*p == d->hw_pix_fmt) return *p;
    for (p = fmts; *p != AV_PIX_FMT_NONE; p++)      // 没有，找第一个软件格式
        if (!(av_pix_fmt_desc_get(*p)->flags & AV_PIX_FMT_FLAG_HWACCEL)) { d->fell_back = 1; return *p; }
    return AV_PIX_FMT_NONE;
}
```

统计里的“硬件帧 / 内存帧”两个数能看出到底有没有用上硬件：全部是内存帧，说明虽然创建了设备，但每一帧都是软件解的。
这个检查很重要，否则统计里写着“设备：vaapi”，实际上是 CPU 在解。

本机上能验证的一种：解码器根本没有这种硬件配置（MPEG-1）：

```console
$ build/hw_decode_av $S/video_360p25.m1v - device=vaapi fallback=1 stats=1
没有可用的硬件设备（解码器 mpeg1video 没有vaapi的硬件配置），改用软件解码
…
帧数：125（硬件帧 0，内存帧 125）
```

**【待补 5】显卡能创建设备、但解不了这个视频。** `verify_hw.sh` 生成一个 10 位 H.264（High 10）的小视频，加上 MPEG-1，用 `device=vaapi fallback=1` 解，
并和软件解码比 MD5。结果因显卡而异：有的显卡支持 10 位 H.264（用上硬件，硬件帧 > 0），有的不支持（走“警告：显卡解不了这个视频”那一条，内存帧 > 0，输出和软件一样）。
补进来，并写上显卡型号：_（待补）_

## 第 8 步：verify_hw.sh，把【待补】补上

有显卡的机器上：

```bash
cd L20_hw_decode && make
./verify_hw.sh                  # 默认测 vaapi
DEVICE_PATH=/dev/dri/renderD129 ./verify_hw.sh        # 有多块显卡时指定一块
./verify_hw.sh cuda             # 其他设备类型（要对应的硬件、驱动和编译选项）
```

脚本先用 `hw_info try=1` 确认设备创建得出来，创建不出来就打印“跳过”，退出码 0（所以它可以放进任何机器的流程里）。
然后依次做“待补 1”到“待补 5”，加一个和 `ffmpeg -hwaccel` 的对照，全部写进 `../out/l20/results.md`，每一节的标题就是这份 README 里的编号，
可以整节复制过来。硬件的输出和软件不一致**不算脚本失败**，会写在结果里（并算 PSNR）：H.264 解码按标准是确定的，通常应该逐字节一致，
如果不一致，把 PSNR 和显卡型号一起记下来，这本身就是值得讲的内容。

**没有显卡的机器上怎么确认这个脚本本身没有问题**：替身模式，“硬件”一侧也用软件解码：

```console
$ STANDIN=1 ./verify_hw.sh
替身模式：不使用显卡，只检查脚本本身
待补 1：hw_info
…
结果写在 ../out/l20/results.md
```

替身模式的 `results.md` 开头写着“**没有使用显卡**”，所有对比必然是“逐字节一致”，速度表里“硬件”那一行的数字和“软件自动线程”几乎一样。
它只证明：脚本的解析、对比、PSNR、写文件这些地方是通的。我们也验证了其中一个会在“不一致”时用到的地方：把一份输出调亮一点再算 PSNR（`ffmpeg -lavfi psnr`），
能取到 37.85 这样的数字。**硬件那一侧的任何结论，都必须来自真正的硬件。**

## 常见错误

| 现象 | 原因和办法 |
| --- | --- |
| `Failed to create specified HW device` | 第 3 层：设备没有。先看 `/dev/dri` 在不在、有没有读写权限（加入 `render` 用户组）；容器和虚拟机要把设备直通进去；用 `loglevel=verbose` 看后端的原因 |
| `Device type xxx is not supported` | 第 1 层：这份 FFmpeg 编译时没带。`ffmpeg -hwaccels` 看有哪些；换设备类型，或者换一份带了的 FFmpeg（L02 的 configure 选项） |
| `Decoder xxx does not support device type yyy` | 第 2 层：解码器没有这种配置。`hw_info codec=xxx` 看它有哪些 |
| `Failed to get HW surface format` | 第 4 层：`get_format` 里没有硬件格式，显卡不支持这个视频。官方示例直接失败；`hw_decode_av` 会退回软件 |
| 输出的画面是绿的、花的，或者大小不对 | 把硬件格式当成了像素：对 `format=vaapi` 的帧直接读 `data[0]`。先 `av_hwframe_transfer_data()`，用搬回来的那一帧的 `format` 去算大小 |
| 统计里写着设备 vaapi，CPU 却很忙 | 每一帧其实是软件解的（硬件帧 0）：看 `get_format` 有没有退回软件格式 |
| 解码了一会儿卡住，或者报帧池用尽的错（CUDA、D3D11 的日志是 “Static surface pool size exceeded.”，各后端措辞不同） | 硬件帧池耗尽：程序把解出来的帧（`av_frame_ref()`）攒着没释放，超过了池子的大小。要攒帧的程序给解码器多留几个（`extra=` 即 `extra_hw_frames`），或者尽快搬回内存并 `av_frame_unref()` |
| `-hwaccel vaapi` 失败退出码 187，脚本里不好判断 | 这是 `AVERROR_EXTERNAL` 的低 8 位（L17 说过这种退出码，L22 会讲错误码表）。自己的程序用 1 表示失败，把原因写在 stderr |

## 练习

1. 在没有显卡的机器上，用 `hw_info codec=hevc` 和 `ffmpeg -hwaccels` 回答：这份 FFmpeg 的 HEVC 解码器能用哪些设备类型？`codec=av1` 呢？换成 `h264_v4l2m2m` 看到的是什么，为什么？
2. 给 `hw_decode_av` 加 `device_path=` 的验证：传一个不存在的路径，比如 `device_path=/dev/dri/renderD999`，输出是什么？和不传时的 verbose 日志比，有什么不同？
3. `hw_decode_av` 的 `sink_frame()` 每次都对 `s->sw` 先 `av_frame_unref()` 再设 `format`。去掉 `unref`，只在第一次设 `format`：会发生什么？（提示：看 `av_hwframe_transfer_data()` 里 `dst->buf[0]` 有没有分配，对应的两条路径。）
4. 官方示例 `decode_write()` 里每一轮 `av_frame_alloc()` 两个帧、用完释放。改成在 `main()` 里分配一次、循环里 `av_frame_unref()` 复用。软件路径上可以先测：把 `hw_decode_av` 的帧分配改成每帧分配一次，用 `stats=1` 量 300 帧的差别。
5. `hw_decode_av` 的 `frames=` 到了就不冲刷解码器直接退出。会丢什么？为什么这里可以不冲刷？
6. 为什么 `get_format` 回调里用 `ctx->opaque` 取数据，而官方示例用全局变量 `hw_pix_fmt`？同时解两路视频（两个解码器、两个线程）时，官方的写法会出什么问题？
7. 有显卡的话：用 `verify_hw.sh` 的结果回答，搬运耗时占墙钟的几分之几？如果把 `pix_fmt=` 换成 `yuv420p`，有没有变化？（取决于答案里有没有 yuv420p）
8. 有显卡的话：`extra=` 设成 0、4、16，1080p 的耗时有变化吗？它在什么情况下才有用？

练习的参考答案和思路：

1. 本机：HEVC、AV1 都只有 `vaapi` 一项。`h264_v4l2m2m` 在配置表里**没有**任何项：专用解码器不走 hwaccel 这套机制（`avcodec_get_hw_config()` 查不到），
   它自己找设备（`/dev/video*`），所以 `hw_info` 才特别另列了一张“专用解码器”的名单。
2. 本机的实测：
   ```text
   [VAAPI @ …] Failed to open /dev/dri/renderD999 as DRM device node.
   [VAAPI @ …] Cannot open X11 display /dev/dri/renderD999.
   [VAAPI @ …] No VA display found for device /dev/dri/renderD999.
   用不了硬件设备：vaapi：Invalid argument
   ```
   传了路径，VAAPI 只试这一个（先当 DRM 节点，再当 X11 显示名），不会去试 renderD128 到 renderD134；错误码是 `EINVAL`（Invalid argument），
   而不传路径时是 `AVERROR_EXTERNAL`（Generic error in an external library）。所以不能靠错误码区分“没有显卡”和“路径写错了”，要看 verbose 日志。
3. 这一条是读 `libavutil/hwcontext.c` 和 `hwcontext_vaapi.c` 的源码得出的，本机验证不了。第一次 `av_hwframe_transfer_data()` 时 `dst->buf[0]` 是空的，
   函数自己分配缓冲区（用 `dst->format`，是 NONE 就选第一种），传完以后 `dst` 有了缓冲区。第二次起 `dst->buf[0]` 不空，函数认为你已经准备好了缓冲区，
   直接往里拷：不选格式，也不分配。所以稳态下去掉 `unref` **看不出区别**，甚至省了每帧的分配。问题出在视频中途分辨率变化时：
   `dst->width/height` 还是旧的，VAAPI 的 `transfer_data_from` 只检查 `dst` 的宽高不超过帧池的，`av_frame_copy()` 按旧的宽高拷，
   得到被裁掉的画面，不报错。所以每次 `unref` 再设格式最稳；想复用缓冲区，也要自己检查尺寸有没有变。
4. `av_frame_alloc()` 只分配 `AVFrame` 结构本身（几百字节），像素缓冲区是解码器从自己的缓冲池里给的，不是每次新分配。
   所以示例的写法不好看，但通常不是性能瓶颈，差别很小，以自己量到的为准。写自己的程序时还是在循环外分配、循环里 `av_frame_unref()`。
5. 丢的是解码器里延迟的帧（B 帧重排，L7、L8）：1 到几帧。`frames=N` 是“只要前 N 帧”，解码器里那几帧是第 N+1 帧之后的，不要，所以不用冲刷。
   如果是“解码整个文件”，必须冲刷，否则丢最后几帧。
6. 全局变量只有一份，两路视频的 `hw_pix_fmt` 会互相覆盖：两路设备类型不同（一路 vaapi、一路 cuda）时，其中一路的 `get_format` 找不到自己的格式。
   放在 `opaque` 里每个解码器上下文一份，就没有这个问题。
7. 和 8. 看自己机器的 `results.md`。

## 下一讲

L21 硬件编码与全 GPU 转码：官方示例 `vaapi_encode.c`、`vaapi_transcode.c`。这一讲把帧从显存搬回了内存，下一讲看另一个方向：把内存里的帧**上传**到显存让显卡编码，
以及帧**一直留在显存里**，从硬件解码直接进硬件编码（不搬运）。`AVHWFramesContext` 要自己建了，帧池的大小、`hwupload` 滤镜也要登场。
这一讲发现的官方示例的 8 个问题，一半在那两个示例里（patch 0099 到 0103）。
