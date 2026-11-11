# L01 音视频基础概念

本讲不写代码。目标是弄清楚一帧画面和一段声音在计算机里是什么样子、为什么必须压缩，
以及“编码格式”和“容器”各管什么。学完后应该能看懂 ffprobe 输出里的每一个字段。

## 本讲要点

**视频**

- 视频是一帧帧静止图像的快速切换。描述未压缩视频需要四个参数：宽、高、像素格式、帧率。
- `rgb24`：每像素 3 字节（红、绿、蓝各 1 字节）。
- `yuv420p`：把像素拆成亮度 Y 和色度 U、V，每 2×2 个像素共用一组 U、V，平均每像素 1.5 字节。
  末尾的 `p` 表示 planar，即 Y、U、V 各自连续存放。
- 帧率用分数表示（如 `30/1`、`30000/1001`），因为它不一定是整数。

**音频**

- 未压缩的音频叫 PCM。描述它需要三个参数：采样率、采样格式、声道布局。
- `s16` 是 16 位有符号整数，左右声道交替存放；`fltp` 是 32 位浮点，每个声道单独存放。
- 每秒数据量 = 采样率 × 声道数 × 每个采样的字节数。

**压缩与容器**

- 1080p30 的 yuv420p 视频每秒 93,312,000 字节（约 746 Mbit/s），必须压缩后才能存储和传输。
- 编码格式（H.264、H.265、AV1、AAC、Opus……）决定数据如何压缩。
- 容器（MP4、MKV、FLV、MPEG-TS……）把多路压缩数据按时间交错打包，并记录时间戳。
  文件扩展名对应的是容器，不是编码格式。
- 处理链路：解封装 → 解码 → 处理 → 编码 → 封装。压缩数据包在代码里叫 `AVPacket`，
  原始帧叫 `AVFrame`。

## 动手验证

这一讲还没有编译 FFmpeg，先用系统自带的版本：

```bash
sudo apt install -y ffmpeg
scripts/gen_samples.sh        # 在仓库根目录运行，生成 samples/ 下的素材
```

查看文件信息，把每个字段对应到上面的概念：

```bash
ffprobe -v error -show_entries \
  format=duration,size,bit_rate:stream=codec_type,codec_name,width,height,pix_fmt,r_frame_rate,sample_rate,channels,bit_rate \
  -of default=nw=1 samples/demo_1080p30.mp4
```

把 1 秒的内容还原成原始数据，核对文件大小：

```bash
mkdir -p out
ffmpeg -i samples/demo_1080p30.mp4 -t 1 -an -f rawvideo -pix_fmt yuv420p out/demo.yuv
ffmpeg -i samples/demo_1080p30.mp4 -t 1 -an -f rawvideo -pix_fmt rgb24   out/demo.rgb
ffmpeg -i samples/demo_1080p30.mp4 -t 1 -vn -f s16le out/demo.pcm
ls -l out/
```

| 文件 | 大小（字节） | 对应的计算 |
| --- | --- | --- |
| demo.yuv | 93,312,000 | 1920 × 1080 × 1.5 × 30 |
| demo.rgb | 186,624,000 | 1920 × 1080 × 3 × 30 |
| demo.pcm | 192,000 | 48000 × 2 × 2 |

原始数据没有文件头，播放时必须自己给出参数。故意写错宽度，看看画面会变成什么样：

```bash
ffplay -f rawvideo -pixel_format yuv420p -video_size 1920x1080 -framerate 30 out/demo.yuv
ffplay -f rawvideo -pixel_format yuv420p -video_size 1280x1080 -framerate 30 out/demo.yuv
ffplay -f s16le -ar 48000 -ch_layout stereo out/demo.pcm
```

## 练习

1. `demo_1080p30.mp4` 的视频部分被压缩了多少倍？音频呢？
2. 一段 4K（3840×2160）60 fps 的 yuv420p 视频，每秒多少字节？
3. 把 `demo_1080p30.mp4` 的扩展名直接改成 `.mkv`，它就变成 MKV 文件了吗？用 ffprobe 验证。
4. 找一个自己手机拍的视频，用 ffprobe 写出它的容器、视频编码、分辨率、帧率、像素格式、
   音频编码、采样率、声道数和总码率。

<details>
<summary>参考答案</summary>

1. 视频原始码率 93,312,000 × 8 ≈ 746.5 Mbit/s，压缩后约 6.1 Mbit/s，约 122 倍。
   音频 1536 kbit/s 对 128 kbit/s，12 倍。文件体积基本由视频决定。
   这里的画面是合成的测试图，真实拍摄内容的压缩比会随画面复杂度变化。
2. 3840 × 2160 × 1.5 × 60 = 746,496,000 字节/秒，约 6 Gbit/s，是 1080p30 的 8 倍。
3. 不会。ffprobe 根据文件内容识别容器，`format_name` 仍然是 `mov,mp4,m4a,3gp,3g2,mj2`。
   真正换容器要用 `ffmpeg -i demo_1080p30.mp4 -c copy demo.mkv`，这就是 L04 要自己实现的 remux。
4. 没有标准答案。留意两个现象：竖屏视频的宽高可能仍是 1920×1080，另带一个旋转信息；
   帧率可能不是整数。

</details>
