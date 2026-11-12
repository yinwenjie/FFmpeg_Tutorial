# L21 硬件编码与全 GPU 转码

L20 把帧从显存**下载**回内存。这一讲反过来：把内存里的帧**上传**到显存，交给显卡上的编码器；再往前一步，解码和编码都在显卡上，
帧从头到尾**不离开显存**——这就是“全 GPU 转码”，硬件加速最划算的用法。

官方示例两个：`vaapi_encode.c`（读 NV12 文件，上传，硬件编码）、`vaapi_transcode.c`（硬件解码接硬件编码）。
自己写的程序三个：`hw_enc_info`（这份 FFmpeg、这台机器能做哪些硬件编码）、`hw_encode_av`（自动选设备、没有显卡时退回 libx264、
会建帧上下文、统计耗时）、`nv12_linesize`（演示官方 `vaapi_encode.c` 的一个 bug：行距）。外加两个脚本：
`test_nodevice.sh`（没有显卡也能做的检查）、`verify_hw.sh`（有显卡的机器上跑，把结果补进这份 README）。

> **和 L20 一样，这一讲有一个限制，请先读完这一节。**
> 写这一讲的机器没有显卡（`/dev/dri` 不存在），所以**硬件编码本身没有在这台机器上跑过**。
> 这份 README 里凡是要显卡才看得到的输出，都标成了 **【待补 N】**，不编造任何数字；其余全部是在这台机器上实际跑出来的。
> 没有显卡也能学到的：概念和 API 的调用顺序、帧上下文怎么建、没有设备时怎么让程序正确地失败或者退回软件编码、
> 软件路径的全部细节（和命令行逐字节一致）、官方示例里找到的问题（patch 0099 到 0103）。
> 有显卡的人运行 `verify_hw.sh`，它把【待补 1】到【待补 5】全部跑一遍，写成 `out/l21/results.md`（第 8 步）。录课时如果用显卡机器录，也请直接跑这个脚本。

完成标志：说得清楚“上传”是什么，为什么编码器要的帧上下文要自己建；知道建帧上下文的几个字段和调用顺序；
知道全 GPU 转码里编码器的帧上下文从哪里来；会用命令行的四种写法（`hwupload`、`-hwaccel_output_format`、`scale_vaapi`、
`-init_hw_device`）；看得出行距（`linesize`）为什么会让一个读文件的程序画面变歪；
`test_nodevice.sh` 全部通过；有显卡的话，`verify_hw.sh` 跑完，结果补进了【待补】。

## 本讲要点

**三种用法，帧在哪里不一样**

```text
A  上传再编码（vaapi_encode、hw_encode_av）：
   文件/内存 ──► AVFrame（nv12，在内存）──► av_hwframe_transfer_data() ──► AVFrame（vaapi，在显存）──► [GPU 编码] ──► 包
                                              ▲ 上传：内存 → 显存，要过总线，所以不是免费的

B  全 GPU 转码（vaapi_transcode）：
   包 ──► [GPU 解码] ──► AVFrame（vaapi，在显存）──► [GPU 编码] ──► 包
                          └── 解码器的帧池，同时也是编码器的帧池：帧一次也不回内存

C  只硬件解码（L20）：  [GPU 解码] ──► 下载到内存 ──► 软件滤镜/软件编码
```

A 的好处是：输入本来就在内存里（文件、摄像头、网络、软件滤镜的结果）时只有这一条路。B 的好处是：解码、缩放（`scale_vaapi`）、编码都在显卡上，
CPU 几乎不参与，也没有总线上的来回搬运。

**编码器的帧上下文要自己建**

L20 里，帧上下文（`AVHWFramesContext`，显存里的一个表面池）是**解码器**建的，我们只给它设备。编码器不一样：
它不知道要编多大的画面，所以 VAAPI 编码器的硬件配置只有一种方法，`avcodec_get_hw_config()` 里写的是 `帧上下文`（第 1 步能看到），
意思是“你得自己建帧上下文，在打开编码器**之前**交给我”。顺序是固定的：

| 步骤 | 调用 | 为什么 |
| --- | --- | --- |
| 1 | `av_hwdevice_ctx_create(&dev, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0)` | 先要有设备 |
| 2 | （可选，但建议）`av_hwdevice_get_hwframe_constraints()` | 问设备：能接受什么内存格式、多大的画面。不问的话，错了只会在下一步得到一个笼统的 `EINVAL` |
| 3 | `av_hwframe_ctx_alloc(dev)`，设 `format=AV_PIX_FMT_VAAPI`、`sw_format=AV_PIX_FMT_NV12`、`width`、`height`、`initial_pool_size` | 描述表面池：硬件格式、上传时的内存格式、尺寸、一次备几个表面 |
| 4 | `av_hwframe_ctx_init()` | 真正去显卡上分配 |
| 5 | `avctx->hw_frames_ctx = av_buffer_ref(frames)`，`avctx->pix_fmt = AV_PIX_FMT_VAAPI` | 交给编码器。**必须在 `avcodec_open2()` 之前**，之后改没有用 |
| 6 | `avcodec_open2()` | 编码器这时才从帧上下文里读尺寸、格式 |
| 每帧 | `av_hwframe_get_buffer(frames, hw_frame, 0)` → `av_hwframe_transfer_data(hw_frame, sw_frame, 0)` → `avcodec_send_frame(hw_frame)` | 从池里取一个空表面，把内存帧上传进去，送去编码 |

`initial_pool_size` 是池子里表面的个数。编码器内部会“握住”几个还没编完的表面（`async_depth` 选项，默认 2，再加参考帧），
所以池子太小会在编了几帧以后取不到表面，报 `ENOMEM`。`hw_encode_av` 默认 20，用 `pool=N` 可以调；多小会出问题是【待补 3】要量的。
表面用完要放手：`av_frame_unref()` 把表面还给池子，不放的话池子很快空了。

**全 GPU 转码：编码器的帧上下文就是解码器的**

B 里不用自己建帧上下文：解码器输出的每个帧都带 `hw_frames_ctx`，直接 `avcodec_alloc_context3()` 之后
`encoder_ctx->hw_frames_ctx = av_buffer_ref(decoder_ctx->hw_frames_ctx)` 就行。问题是：解码器的帧上下文要等第一帧解出来才有
（尺寸、格式是解码器看了码流才知道的），所以**编码器也要等第一帧解出来才能建**——`vaapi_transcode.c` 里的 `dec_enc()` 就是这样，
第一次拿到帧时才打开编码器和输出文件头。这也带来一个限制：视频中途改分辨率，帧上下文就变了，示例不处理。

**上传不是免费的，硬件编码也不一定“更好”**

- 上传要把整帧从内存拷到显存。1080p 的 NV12 一帧约 3.1 MB，25 fps 就是每秒近 78 MB，对现代总线不算什么，但不是零。
  能少上传就少上传：全 GPU 转码（B）一次都没有。
- 硬件编码器的主要收益是**CPU 空出来**和**省电**；速度上，显卡编码通常比软件快，但小视频要启动设备、建池子，不一定划算。
- 质量不是一回事：同样的码率，硬件编码器通常不如软件的 x264（`preset=slow` 之类）。要不要用硬件编码，看你要的是速度和 CPU，还是码率效率。
  这两项在这台机器上没法量，是【待补 3】、【待补 4】。

**两个来自官方示例的问题（已做成 patch）**

1. **行距**（patch 0099）。官方 `vaapi_encode.c` 用 `fread()` 把文件直接读进 `frame->data[0]`，假设每一行恰好 `width` 字节。
   但 `av_frame_get_buffer()` 为了让 CPU 的向量指令高效，会把每一行补齐到对齐值（这台机器 64 字节）的整数倍，宽 352 的行距是 384。
   宽不是对齐值的倍数时，每行末尾的 32 个字节是填充，直接读就把文件里下一行的头 32 个字节塞进了填充，画面被切歪。第 2 步演示。
2. **时间基**（patch 0101）。官方 `vaapi_transcode.c` 解码出来的帧时间戳在**输入流**的时间基里（MP4 常见的 1/12800），
   却告诉编码器“时间基是 1/帧率”：编码器看到的帧间隔是几百“帧”，码率控制按错的目标工作，输出的平均帧率、时长都不对。
   修复前后对照在 `upstream_patches/README.md` 的 0101 一节。（L16 的 `transcode_av` 就是正确做法，L12 讲过时间基换算。）

## 准备

```bash
export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig PATH=$HOME/ffmpeg_build/bin:$PATH LD_LIBRARY_PATH=$HOME/ffmpeg_build/lib
cd L21_hw_encode
make                                  # 不需要显卡就能编译
S=../samples
O=../out/l21 && mkdir -p $O
# 本讲用的输入：NV12 的裸像素文件（一帧 640x360x1.5 = 345600 字节，125 帧）
ffmpeg -v error -y -i $S/small_360p25.mp4 -pix_fmt nv12 -f rawvideo $O/in_360p.nv12
```

素材是 `scripts/gen_samples.sh`（见 L02）生成的。本机环境：`/dev/dri` 不存在，FFmpeg 带了 `--enable-vaapi`，没有 CUDA、QSV 等；
编码器里有 `*_vaapi` 一族、`*_v4l2m2m` 一族和软件的 `libx264`。

## 第 1 步：hw_enc_info，这份 FFmpeg 能做哪些硬件编码

```console
$ build/hw_enc_info codec=h264_vaapi
1. 这份 FFmpeg 编译进来的硬件设备类型（git-2026-11-11-a337e8d）：
   vaapi drm

2. 硬件编码器（名字带后缀的，以及编码器自己说有硬件配置的）：
   h264_vaapi         H.264/AVC (VAAPI)
      输入格式： vaapi
      硬件配置：vaapi    帧格式 vaapi    方法 帧上下文
      标记：硬件编码器 

3. 机器上的设备：
   /dev/dri 不存在：没有显卡，或者这是没有直通显卡的虚拟机、容器。VAAPI 和 DRM 肯定用不了。
```

不写 `codec=` 会列出全部：本机是 `av1_vaapi`、`h264_vaapi`、`hevc_vaapi`、`mjpeg_vaapi`、`mpeg2_vaapi`、`vp8_vaapi`、`vp9_vaapi`，
加上 ARM 板子上用的 `h263_v4l2m2m`、`h264_v4l2m2m`、`hevc_v4l2m2m`、`mpeg4_v4l2m2m`、`vp8_v4l2m2m`。读法：

- **输入格式 `vaapi`**：编码器要的不是 `yuv420p`，是显存里的帧。这是和 L13 的 `libx264`（输入格式一长串内存格式）的根本区别：
  对比 `build/hw_enc_info codec=libx264`，输入格式是 `yuv420p nv12 …`，硬件配置是“没有”。
- **方法 `帧上下文`**：只有这一种。解码器（L20）是 `设备上下文+帧上下文+需额外设置`，编码器没有“给设备就行”这一条，因为它不知道画面多大。
- **`*_v4l2m2m`** 是另一类：专用编码器，自己管设备，没有 `avcodec_get_hw_config` 的配置，也不走这一讲的帧上下文（它们读写的是 `/dev/video*`，用在 ARM 板子上）。
  没有设备时同样打不开。

**【待补 1】** 有显卡时 `build/hw_enc_info try=1` 的第 4 层输出：

```console
$ build/hw_enc_info try=1
……
4. 逐个创建设备（device=默认）：
   vaapi    创建成功
      硬件帧格式： vaapi
      可以上传的内存格式（建帧上下文的 sw_format 要从这里选）： 【待补 1】
      尺寸范围：【待补 1】
   drm      【待补 1】
```

本机这一层的结果是：`vaapi 创建失败：Generic error in an external library（-542398533）`，`drm 创建失败：Bad address（-14）`。
创建设备失败时加 `verbose=1` 能看到 VAAPI 后端的原因（例如 `No VA display found for device /dev/dri/renderD128`，这是没有设备的情况；
有设备但没有权限时是 `Permission denied`，要把用户加进 `render` 组）。

## 第 2 步：行距，官方 vaapi_encode 的第一个 bug

先看 `av_frame_get_buffer()` 给出的行距：

```console
$ build/nv12_linesize table
这台机器上 av_cpu_max_align() = 64
   宽  linesize[0]  linesize[1] 每行填充
   176          192          192       16
   320          320          320        0    （等于宽：直接读碰巧不出错）
   352          384          384       32
   640          640          640        0    （等于宽：直接读碰巧不出错）
   720          768          768       48
   854          896          896       42
  1000         1024         1024       24
  1280         1280         1280        0    （等于宽：直接读碰巧不出错）
  1366         1408         1408       42
  1920         1920         1920        0    （等于宽：直接读碰巧不出错）
```

对齐值取决于 CPU：这台机器 `av_cpu_max_align()` 是 64；只支持 AVX2 的机器是 32，这时 352 的行距是 352（刚好是 32 的倍数），看不出问题，
但 176 到 192 或者 1000 到 1024 的情况不变。**所以同一个程序在你的机器上“好的”，在别人的机器上可能是歪的。**

再用 `naive`（官方原版的读法）和 `fixed`（patch 0099 的读法）各读一遍同一个文件，再原样写出来：

```console
$ ffmpeg -v error -y -i $S/small_360p25.mp4 -vf scale=352:288 -pix_fmt nv12 -frames:v 10 -f rawvideo $O/in_352x288.nv12
$ build/nv12_linesize naive 352 288 $O/in_352x288.nv12 $O/naive.nv12
naive：10 帧，352x288，linesize[0] = 384（宽 352）
$ build/nv12_linesize fixed 352 288 $O/in_352x288.nv12 $O/fixed.nv12
fixed：10 帧，352x288，linesize[0] = 384（宽 352）
$ cmp $O/in_352x288.nv12 $O/fixed.nv12 && echo 相同
相同
$ cmp $O/in_352x288.nv12 $O/naive.nv12
../out/l21/in_352x288.nv12 ../out/l21/naive.nv12 differ: char 353, line 1
```

`char 353`：第一行（352 字节）读对了，**第二行的第一个字节就错了**，和上面的分析一致。量一下差多少：

```console
$ ffmpeg -v info -f rawvideo -pix_fmt nv12 -s 352x288 -i $O/in_352x288.nv12 -f rawvideo -pix_fmt nv12 -s 352x288 -i $O/naive.nv12 -lavfi psnr -f null - 2>&1 | grep psnr:
[Parsed_psnr_0 @ ...] PSNR y:9.511989 u:6.926588 v:6.650782 average:8.402671 min:8.395827 max:8.412730
```

8.4 dB：基本上是另一幅图（正常的编码损失是 35 dB 以上）。动手看一眼：

```bash
ffmpeg -v error -y -f rawvideo -pix_fmt nv12 -s 352x288 -i $O/naive.nv12 -frames:v 1 $O/naive.png
ffmpeg -v error -y -f rawvideo -pix_fmt nv12 -s 352x288 -i $O/in_352x288.nv12 -frames:v 1 $O/orig.png
```

打开 `naive.png`：每一行都比上一行再往右错 32 个像素，画面被剪成斜条。对 640 宽（`linesize` 等于宽）的文件，`naive` 和 `fixed` 的输出逐字节相同：
这就是为什么官方示例用 1920x1080 这种“整”尺寸测试时从没出过问题。

注意 `nv12_linesize.c` 里还有一行官方原版没有的 `memset`，把帧缓冲清零：`av_frame_get_buffer()` 分配的内存**没有清零**，
`naive` 读不满的帧尾是没初始化的内存，每次运行都可能不一样（valgrind 会报“用了没初始化的数据”）。清零是为了让这个演示的结果可重复，不是修复的一部分。

**正确的做法**（`fixed`，也是 `hw_encode_av` 的 `read_frame()`）：读进一块紧密排列的缓冲区，再用 `av_image_copy2()` 按两边各自的行距拷进帧。
任何“把一个文件（或者网络数据、或者别的库的缓冲区）填进 `AVFrame`”的代码都应该这样做，永远不要假设 `linesize == width`。

## 第 3 步：官方 vaapi_encode.c，没有显卡时

`vaapi_encode.c` 是课程拷贝（代码是打了 patch 0099、0100 的版本，加了中文注释）。读代码时按三段看：

1. `set_hwframe_ctx()`：建帧上下文，就是前面表格的第 3 到 5 步。这里的 `sw_format = AV_PIX_FMT_NV12`、
   `initial_pool_size = 20` 是写死的。
2. `main()`：建设备 → 开编码器 → 循环：读一帧到内存帧 → `av_hwframe_get_buffer()` → `av_hwframe_transfer_data()` 上传 → 编码 → 最后冲刷。
3. `encode_write()`：和 L13 的软件编码完全一样（送帧、取包、写文件）。**编码这一步对软件、硬件编码器没有区别**，区别全在“帧怎么准备”。

没有显卡时：

```console
$ build/vaapi_encode 640 360 $O/in_360p.nv12 $O/v.h264; echo rc=$?
Failed to create a VAAPI device. Error code: Generic error in an external library
rc=1
$ build/vaapi_encode; echo rc=$?
Usage: build/vaapi_encode <width> <height> <input file> <output file>
rc=1
$ build/vaapi_encode 640 360 /nonexistent $O/v.h264; echo rc=$?
Fail to open input file : No such file or directory
rc=1
```

三种失败都是 1。官方原版（a337e8d）：创建设备失败是 **187**（`AVERROR_EXTERNAL` 的低 8 位），用法错误和找不到输入都是 **255**（`return -1`）。
为什么错误码当退出码不对，第 7 步细说。（patch 0100，修复前后在没有显卡的机器上都检查过。）

**【待补 2】** 有显卡时跑官方 `vaapi_encode`：

| 输入 | 退出码 | 输出码流大小（字节） | 解码出的帧数 / 输入帧数 | PSNR（dB，和输入比） |
| --- | --- | --- | --- | --- |
| 640x360，125 帧 | 【待补 2】 | 【待补 2】 | 【待补 2】 | 【待补 2】 |
| 352x288，10 帧（行有填充） | 【待补 2】 | 【待补 2】 | 【待补 2】 | 【待补 2】 |

352x288 这一行是 patch 0099 的硬件验证：用 patch 之前的读法 PSNR 只有 8 dB 左右，现在应当是 30 dB 以上。

## 第 4 步：hw_encode_av，没有显卡也能用的硬件编码程序

`hw_encode_av` 把上面的步骤写成一个完整的小工具：

```console
$ build/hw_encode_av 640 360 $O/in_360p.nv12 $O/sw.h264 stats=1 md5=1
没有可用的硬件编码器（h264_vaapi：创建 vaapi 设备失败：Generic error in an external library），改用软件编码器 libx264
md5=1168117bc5ca46b72119dc351526360b
编码器：libx264（软件（CPU））
设备：无
输入：640x360 nv12，125 帧
输出：125 个包，445697 字节
耗时（墙钟）：0.847 秒，147.7 帧/秒
CPU 时间（用户+系统，所有线程）：0.844 秒
上传耗时（内存拷到显存）：0.000 秒
转换耗时（libswscale）：0.000 秒
```

（耗时每次运行不同，这台机器上 125 帧在 0.65 到 0.85 秒之间波动；字节数和 MD5 是确定的。）

`enc=auto`（默认）先试 `h264_vaapi`，创建不了设备就退回 `libx264`，并且**把原因说出来**。程序里有三件事值得读：

- `try_encoder()`：用 `avcodec_get_hw_config()` 判断这个编码器要不要设备；要，就试着建，失败的原因收集进一个字符串，最后一起报告。
- `setup_frames_ctx()`：就是前面表格的第 2 到 4 步，包括先问设备约束。
- `read_frame()`：第 2 步的 `fixed` 读法。

**和命令行逐字节一致。** 软件路径的输出可以和 `ffmpeg` 命令行比：

```console
$ ffmpeg -v error -f rawvideo -pix_fmt nv12 -s 640x360 -r 25 -i $O/in_360p.nv12 -c:v libx264 -threads 1 -f h264 - | md5sum
1168117bc5ca46b72119dc351526360b  -
```

和上面 `md5=` 的值相同。`test_nodevice.sh` 里把这个比较做了 9 组：640x360 的 nv12 和 yuv420p 输入、352x288、854x480、1000x562（这三个行都有填充）、1080p、`fps=30`、
`frames=10`、`opts=preset=veryfast:crf=28`、从标准输入读。两个要点：

- **`threads=1` 是默认**。libx264 多线程时输出的字节会随线程数和调度变化（质量差别很小，但字节不同），要“可重复”必须固定线程数。和 L13 一样。
- **`sample_aspect_ratio` 不要设**。设成 1:1 会让 x264 在码流里多写一段 VUI（多 1 个字节），就和命令行不一样了，命令行对裸像素输入是不写的。

选项（完整的看文件开头的注释）：

| 选项 | 用法 |
| --- | --- |
| `enc=h264_vaapi` | 指定编码器，用不了就失败；加 `fallback=1` 才退回 libx264 |
| `device_path=/dev/dri/renderD129` | 多块显卡时选一块 |
| `pix_fmt=yuv420p` | 输入不是 nv12 时，先用 libswscale 转成 nv12 再上传（硬件编码器的表面是 nv12） |
| `pool=N` | 帧池大小，默认 20 |
| `opts=qp=25` | 原样传给编码器（VAAPI 的选项用 `ffmpeg -h encoder=h264_vaapi` 看：`qp`、`rc_mode`、`async_depth`、`low_power`……） |
| `frames=N`、`fps=N` | 只编前 N 帧、帧率 |

出错的路径（全部退出码 1，原因写在 stderr，ASan 下没有泄漏）：

```console
$ build/hw_encode_av 640 360 $O/in_360p.nv12 $O/x.h264 enc=h264_vaapi; echo rc=$?
用不了编码器：h264_vaapi：创建 vaapi 设备失败：Generic error in an external library
（加 fallback=1 可以退回软件编码器，加 loglevel=verbose 看硬件后端报告的原因）
rc=1
$ build/hw_encode_av 640 360 $O/in_360p.nv12 $O/x.h264 enc=h264_vaapi device_path=/dev/dri/renderD128; echo rc=$?
[VAAPI @ 0x…] No VA display found for device /dev/dri/renderD128.
用不了编码器：h264_vaapi：创建 vaapi 设备失败：Invalid argument
……
rc=1
$ head -c 100000 $O/in_360p.nv12 > $O/part.nv12
$ build/hw_encode_av 640 360 $O/part.nv12 - enc=libx264; echo rc=$?
输入文件的最后一帧不完整：只有 100000 字节，一帧要 345600 字节
rc=1
```

注意第二条：指定了设备路径时，错误码变成了 `Invalid argument`，第一条是 `Generic error in an external library`——**同一个原因（没有设备），
不同的路径给出不同的错误码**。所以别靠错误码判断“有没有设备”，看 stderr 里 VAAPI 后端自己打的那一行。

**【待补 3】** 有显卡时的硬件编码（`verify_hw.sh` 的第 3 节会跑完这些）：

```console
$ build/hw_encode_av 640 360 $O/in_360p.nv12 $O/hw.h264 enc=h264_vaapi stats=1
编码器：【待补 3】（显卡）
设备：【待补 3】
输入：640x360 nv12，125 帧
输出：【待补 3】
耗时（墙钟）：【待补 3】
CPU 时间（用户+系统，所有线程）：【待补 3】
上传耗时（内存拷到显存）：【待补 3】
```

要量的内容：同一输入，硬件编码和软件编码（上面的 0.85 秒、0.84 秒 CPU 时间）的耗时和 CPU 时间；码流大小和 PSNR 对比；
行有填充的尺寸（352x288、854x480、1000x562）编码是否正确；`pool=` 多小会出错（`pool=1`、`2`、`4`）。

## 第 5 步：vaapi_transcode.c，全 GPU 转码

`vaapi_transcode.c` 的结构（课程拷贝里每一段都有注释）：

| 函数 | 做什么 |
| --- | --- |
| `open_input_file()` | 打开输入，找视频流，建解码器，把设备交给它（`hw_device_ctx`），`get_format` 里选 `vaapi`（和 L20 完全一样） |
| `dec_enc()` | 送包、取帧。**第一帧解出来时才建编码器**：`encoder_ctx->hw_frames_ctx = av_buffer_ref(decoder_ctx->hw_frames_ctx)`，`pix_fmt = AV_PIX_FMT_VAAPI`，然后开编码器、写输出文件头 |
| `encode_write()` | 送帧、取包、写文件 |
| `main()` | 建设备、读包、冲刷解码器、冲刷编码器、写文件尾 |

和 L16 的软件转码相比少了什么：**没有滤镜、没有格式转换、没有 `av_hwframe_transfer_data()`**——帧从头到尾是显存里的表面，CPU 只搬运句柄。
这也是它的限制：不能加滤镜（要加就得用 `scale_vaapi` 这样的硬件滤镜，或者下载回内存用软件滤镜），不能中途改分辨率。

没有显卡时：

```console
$ build/vaapi_transcode $S/small_360p25.mp4 h264_vaapi $O/v.mp4; echo rc=$?
Failed to create a VAAPI device. Error code: Generic error in an external library
rc=1
$ build/vaapi_transcode; echo rc=$?
Usage: build/vaapi_transcode <input file> <encode codec> <output file>
The output format is guessed according to the file extension.

rc=1
```

patch 0101 和 0102 都改这个文件。0101（时间基）和 0102（错误处理）的细节，包括“有视频流但一帧也解不出来时 `av_write_trailer()` 崩溃”和各项修复前后的退出码，
都在 `upstream_patches/README.md` 里；这两个 patch 的时间戳和错误处理逻辑，是用“把 VAAPI 换成 libx264”的软件替身验证过的。

**命令行里的对应写法**（四种，各有用处）：

```bash
# 1. 内存里的帧：用 hwupload 滤镜上传（对应 vaapi_encode / hw_encode_av）
ffmpeg -vaapi_device /dev/dri/renderD128 -f rawvideo -pix_fmt nv12 -s 640x360 -r 25 -i in.nv12 \
       -vf 'format=nv12,hwupload' -c:v h264_vaapi out.h264

# 2. 全 GPU：解码器输出留在显存里，编码器直接用（对应 vaapi_transcode）
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -c:v h264_vaapi out.mp4

# 3. 全 GPU，中间加硬件滤镜（缩放在显卡上做）
ffmpeg -hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -vf scale_vaapi=w=320:h=180 -c:v h264_vaapi out.mp4

# 4. 显式地建设备，再告诉滤镜图用哪个（有多个设备、或者滤镜图里要多次用同一个设备时）
ffmpeg -init_hw_device vaapi=va:/dev/dri/renderD128 -filter_hw_device va \
       -f rawvideo -pix_fmt nv12 -s 640x360 -r 25 -i in.nv12 -vf 'format=nv12,hwupload' -c:v h264_vaapi out.h264
```

第 1 条里的 `format=nv12,hwupload`：`format=nv12` 把内存里的帧先转成显卡接受的内存格式（输入已经是 nv12 时什么也不做），
`hwupload` 上传。`hwupload` 要找到设备：`-vaapi_device` 就是给它设备的简写，第 4 条是完整写法。

**【待补 4】** 有显卡时，同一个 1080p 输入（`$S/demo_1080p30.mp4`，300 帧）、同一台机器上，下面四种做法的耗时和 CPU 时间：

| 做法 | 墙钟（秒） | CPU 时间（秒） | 备注 |
| --- | --- | --- | --- |
| 全 GPU：`-hwaccel vaapi -hwaccel_output_format vaapi … -c:v h264_vaapi` | 【待补 4】 | 【待补 4】 | 帧不离开显存 |
| 硬件解码，下载，再上传，硬件编码 | 【待补 4】 | 【待补 4】 | 多了两趟总线 |
| 只硬件解码（L20 的做法），软件编码（libx264） | 【待补 4】 | 【待补 4】 | |
| 纯软件（`libx264`） | 【待补 4】 | 【待补 4】 | 对照 |
| 官方 `vaapi_transcode` | 【待补 4】 | 【待补 4】 | 应当和第一行接近；输出的平均帧率是 30/1、时长 10 秒（patch 0101） |

## 第 6 步：QSV 的写法，选读

Intel 还有一套自己的接口 QSV（Quick Sync Video）。官方示例里有 `qsv_decode.c` 和 `qsv_transcode.c`，**课程没有拷贝，也没有在这台机器上运行**，
这里只讲读法，和 VAAPI 的区别：

- **专用解码器和编码器**：不是“`h264` 加 hwaccel”，而是 `avcodec_find_decoder_by_name("h264_qsv")`、`"h264_qsv"` 编码器，名字带后缀（L20 的“两条路”里的第二条）。
- **设备类型**：`AV_HWDEVICE_TYPE_QSV`。`get_format` 里选的像素格式是 `AV_PIX_FMT_QSV`。
- **转码的结构和 `vaapi_transcode.c` 一样**：编码器等第一帧解出来，再把解码器的 `hw_frames_ctx` 引用给它（`qsv_transcode.c` 的 `dec_enc()`，同样的“第一帧才建编码器”）。
- **多出来的功能**：`qsv_transcode` 演示“编码到一半动态改编码器选项”（`av_opt_set_dict()`，例如 100 帧以后把 GOP 从 60 改成 120）。命令行形式：
  `qsv_transcode input.mp4 h264_qsv out.mp4 "g 60" 100 "g 120"`。
- **编译**：`qsv_decode.c` 要 Intel libvpl 的头文件（`mfxvideo.h`），这台机器没有，没法编译，也没有检查。`qsv_transcode.c` 能编译，
  没有 QSV 设备时同样在“创建设备”那一步失败。它的错误处理和 `vaapi_transcode.c` 有同一类问题，做成了 patch 0103（修复前创建设备失败的退出码是 244，修复后是 1）。

## 第 7 步：命令行里的硬件错误，和退出码

没有设备（或者写错了滤镜图）时，`ffmpeg` 命令行的行为，这台机器上实际跑的结果：

| 命令 | 退出码 | 说了什么 |
| --- | --- | --- |
| `-vaapi_device /dev/dri/renderD128 … -vf format=nv12,hwupload -c:v h264_vaapi` | **234** | `No VA display found for device /dev/dri/renderD128.` `Device creation failed: -22.` `Failed to set value … for option 'vaapi_device': Invalid argument` |
| 同上但没有给 `-vaapi_device`（`hwupload` 找不到设备） | **234** | `A hardware device reference is required to upload frames to.` `Error initializing filters` |
| `-hwaccel vaapi -hwaccel_output_format vaapi -i in.mp4 -c:v h264_vaapi` | **187** | `Device creation failed: -542398533.` `No device available for decoder: device type vaapi needed for codec h264.` |
| 没有 `hwupload`，nv12 内存帧直接送给 `-c:v h264_vaapi` | **218** | `Impossible to convert between the formats supported by the filter 'Parsed_null_0' and the filter 'auto_scale_0'`（`dst: vaapi`）…… `Task finished with error: Function not implemented` |
| `-c:v bogus` | **8** | `Unknown encoder 'bogus'` |
| 输入文件不存在 | **254** | `Error opening input: No such file or directory` |

读这张表的三个结论：

- **第 4 行是新手最常见的错误**：硬件编码器的输入是 `vaapi` 格式的帧，内存里的 nv12 帧必须先 `hwupload`。看到 `dst: vaapi` 和 `Impossible to convert`，就是这个问题。
  退出码 218 是 `AVERROR(ENOSYS)`，即 `-38` 的低 8 位。
- **同样是“没有设备”，退出码不一样**：234 是 `-22`（`EINVAL`），187 是 `AVERROR_EXTERNAL` 的低 8 位，取决于走到哪一条路径。
  这和第 4 步里看到的 `hw_encode_av` 完全一样，所以**不要靠退出码判断原因**。
- **这些退出码都是“负的 AVERROR 取低 8 位”**：进程退出码只有 8 位，`return -22` 变成 234，`return AVERROR_EXTERNAL` 变成 187，`return -1` 变成 255。
  `ffmpeg` 命令行自己就是这样返回的（上表的 234、187、254）；我们自己写的程序不应该这样：成功 0，失败 1，错误原因写在 stderr。
  这是官方示例里 patch 0095、0096、0100、0102、0103 修的同一类问题。（L22 会列一张完整的错误码表。）

## 第 8 步：有显卡的机器上，一次跑完全部【待补】

```bash
cd L21_hw_encode && make
./verify_hw.sh                             # 默认 VAAPI，/dev/dri/renderD128
DEVICE_PATH=/dev/dri/renderD129 ./verify_hw.sh    # 多块显卡时指定
cat ../out/l21/results.md                  # 五节：待补 1 到待补 5，把结果填回上面的表格
```

它做五件事：

| 待补 | 内容 |
| --- | --- |
| 1 | `hw_enc_info try=1`：设备能接受的内存格式、尺寸范围 |
| 2 | 官方 `vaapi_encode`：退出码、码流大小、帧数、PSNR；352x288 验证 patch 0099 |
| 3 | `hw_encode_av` 的硬件编码：多种尺寸和输入格式的帧数、PSNR、码流大小、`pool=` 多小出错、和命令行硬件编码比 |
| 4 | 全 GPU 转码：官方 `vaapi_transcode` 和命令行四种做法的耗时、CPU 时间，输出的平均帧率和时长（patch 0101） |
| 5 | 设备支持哪些编码器：h264、hevc、vp9、av1、mpeg2、mjpeg 各编 10 帧，成功还是失败 |

没有 VAAPI 设备时，脚本打印“跳过”，退出码 0。**`STANDIN=1 ./verify_hw.sh`** 是替身模式：不用显卡，“硬件”这一侧也用 libx264，
只用来检查脚本本身（解析、对比、写 results.md）有没有问题；`results.md` 开头会写明“替身模式”，结果不能当硬件结果用。
这个脚本的替身模式在本机跑过，全部五节都能生成。

## 第 9 步：没有显卡也能做的检查

```bash
./test_nodevice.sh                         # 52 项，全部应当通过
```

它检查：软件路径和命令行逐字节一致（9 组）；`nv12_linesize` 的 `fixed` 总是对、`naive` 在行有填充时是错的；
所有出错路径（缺参数、宽高不对、输入不存在、输出写不了、不完整的帧、编码器名字不对、没有设备时）退出码是 1 且没有 sanitizer 报告；
官方拷贝 `vaapi_encode`、`vaapi_transcode` 没有设备时退出码 1；`hw_enc_info` 的各种选项。
用 `BUILD=build-asan ./test_nodevice.sh` 可以在 ASan 下跑（先 `make SANITIZE=address,undefined BUILD=build-asan`）。
全课程的检查脚本 `L22_engineering/check_all.sh` 已经把本讲的 5 个程序都加进去了：`L22_engineering/check_all.sh asan L21`（本机 ASan、valgrind、普通三种模式都通过）。

## 常见错误

| 现象 | 原因 | 怎么办 |
| --- | --- | --- |
| `Failed to create a VAAPI device`、`No VA display found for device` | 没有 `/dev/dri`（没有显卡、虚拟机、容器） | 用 `hw_encode_av`（自动退回 libx264），或者换有显卡的机器 |
| `Permission denied`（打开 `/dev/dri/renderD128`） | 用户没有权限 | 加入 `render` 组，重新登录 |
| `Impossible to convert between the formats …（dst: vaapi）` | 内存帧直接送给硬件编码器 | 命令行加 `-vf format=nv12,hwupload`；自己写代码要先上传 |
| `A hardware device reference is required to upload frames to` | `hwupload` 没有设备 | 命令行加 `-vaapi_device`，或者 `-init_hw_device` 加 `-filter_hw_device` |
| 编码了几帧后 `Cannot allocate memory`（`ENOMEM`） | 帧池太小，或者取出来的表面没有放手 | 增大 `initial_pool_size`；每次用完 `av_frame_unref()` |
| `avcodec_open2()` 返回 `EINVAL`，别的什么也没说 | `hw_frames_ctx` 没设，或者设在 `avcodec_open2()` 之后，或者尺寸、`sw_format` 设备不接受 | 先问 `av_hwdevice_get_hwframe_constraints()`，`hw_encode_av` 的 `setup_frames_ctx()` |
| 编出来的画面是歪的斜条 | 读输入时没有考虑 `linesize` | `av_image_copy2()`（第 2 步） |
| 输出的平均帧率、时长不对 | 编码器的时间基没设对 | 帧的 pts 换算到编码器的时间基（L12、L16，patch 0101） |
| 两次编码的码流字节不一样 | libx264 多线程 | `threads=1`（`hw_encode_av` 默认） |
| 视频中途换分辨率，转码出错 | 编码器的帧上下文是第一帧时建的，不会跟着变 | `vaapi_transcode` 不支持；要支持得在分辨率变化时重建编码器（或者用 `scale_vaapi` 固定输出尺寸） |

## 练习

1. **行距**。用 `nv12_linesize table 100 200 300 400 500` 看这几个宽的行距。哪几个宽读起来是对的？再用 `naive` 读一个 200x100 的 NV12 文件，
   `cmp` 找出第一个不一样的字节在哪里。
2. **对齐值**。`nv12_linesize table` 开头打印的 `av_cpu_max_align()` 是 64。在没有 AVX-512 的机器上它是 32。352 的行距会变成多少？
   （提示：352 是 32 的倍数。）
3. **用命令行验证软件路径**。对 `$S/small_360p25.mp4` 用 `ffmpeg … -pix_fmt yuv420p -f rawvideo` 得到 YUV420P 文件，用 `hw_encode_av … pix_fmt=yuv420p md5=1 enc=libx264`
   算 MD5，和命令行 `-c:v libx264 -threads 1 -f h264 - | md5sum` 对比。如果改成 `threads=4`，MD5 还一样吗？为什么？
4. **改 hw_encode_av**。加一个 `scale=WxH` 选项：输入是 640x360，输出 320x180。软件路径用 libswscale（L9），硬件路径应当在哪里缩放？
   （提示：上传之前缩小，上传的数据量更少。）
5. **（有显卡）池子多大才够**。`pool=1`、`2`、`4`、`20` 各编一遍，哪些成功？`async_depth` 选项（`opts=async_depth=4`）会让需要的池子变大还是变小？
6. **（有显卡）测上传的代价**。`stats=1` 里有“上传耗时”。用 1080p、`frames=300`，上传占总耗时的百分之几？和第 5 步【待补 4】里全 GPU 转码的 CPU 时间一起想想，哪个差别来自上传。
7. **读代码**。`vaapi_transcode.c` 的 `dec_enc()` 里，编码器是什么时候建的？如果输入文件的第一帧就解码失败，编码器建了吗？`main()` 最后冲刷编码器时怎么知道“该不该冲刷”？（patch 0102。）

### 练习的参考答案

1. 宽 100、200、300、400、500 的 `linesize` 在 `av_cpu_max_align()` 为 64 的机器上分别是 128、256、320、448、512，只有 `linesize == 宽` 的才对（本机上没有一个）；
   200x100 的 `cmp` 第一个不同的字节是第 201 个（`char 201`）：第二行的第一个字节。
2. 宽 352 在对齐值 32 时行距是 352，等于宽，`naive` 读起来是对的；换成 64 对齐就是 384。这就是“在我机器上好的”的来历。
3. `threads=4` 时 MD5 通常不一样：libx264 多线程用了 slice 或 lookahead 并行，每次的决策和线程数、调度有关，字节会变；
   质量几乎一样，但“可重复”要靠固定线程数。
4. 硬件路径在**上传之前**缩小（用 libswscale）：上传的数据量变成 1/4，上传的耗时和总线占用都下降；
   或者先上传、再用 `scale_vaapi`（命令行第 3 种写法）在显卡上缩放，CPU 不参与，但上传的是原始大小。
5. 取决于显卡和驱动，是【待补 3】要量的结论。`async_depth` 越大，编码器同时握住的表面越多，需要的池子越大。
6. 取决于机器，是【待补 4】、【待补 3】的数据。上传是内存到显存的一次拷贝，全 GPU 转码里它不存在。
7. 第一帧解出来才建编码器（`if (!initialized)`）。第一帧就解码失败时编码器没建、输出文件头也没写，所以冲刷编码器、写文件尾必须跳过——
   patch 0102 加的检查就是这个（原版会对着没有写过文件头的复用器调用 `av_write_trailer()`，崩溃）。

## 对前面和后面的交代

- **L20 说过“后面再讲”的**：上传、编码器的帧上下文自己建、`hwupload` 滤镜、帧留在显存里不搬回来——这一讲全部讲完（要点一节、第 4、5 步）。
- **L12 说过的**：滤镜图和时间基——这一讲 patch 0101 是时间基换算的又一个实例，`hwupload`、`scale_vaapi` 是滤镜图里的硬件滤镜。
- **L17 和 L20 说过“后面讲”的退出码和错误码表**：第 7 步看了命令行的 234、218、187、8、254 几种，完整的表放在 L22。
- **留给 L22**：`check_all.sh`（本讲已经接进去）、回归脚本、错误码表。
