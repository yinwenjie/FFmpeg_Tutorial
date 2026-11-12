# FFmpeg 音视频开发新手课程 · 配套代码

这是一套面向音视频开发初学者的 FFmpeg 课程的配套代码。课程以 FFmpeg 源码树
`doc/examples` 下的官方示例为主线，沿着“解封装 → 解码 → 滤镜与格式转换 → 编码 → 封装”
这条处理链路，一讲一讲地学会使用 FFmpeg 的各个库，最后完成一个综合实战项目。

课程只讲 SDK 的用法和背后的音视频原理，不深入 FFmpeg 内部实现。

## 适合谁

- 会写 C 语言（指针、结构体、动态内存），用过 Linux 命令行；
- 没有做过音视频开发，想入门基于 FFmpeg 的工程研发。

## 环境

| 项目 | 版本 |
| --- | --- |
| 操作系统 | Ubuntu 24.04（Windows 用户可使用 WSL2） |
| FFmpeg | 从源码编译，提交 `a337e8d`（master，libavcodec 63.x） |
| 构建工具 | gcc、make、CMake ≥ 3.16、pkg-config、nasm |

课程代码统一使用当前的新接口（`avcodec_send_packet` / `avcodec_receive_frame`、
`AVChannelLayout` 等）。系统自带的 `apt install ffmpeg` 版本较旧，只在 L01 中临时使用。

## 快速开始

```bash
# 1. 安装依赖
sudo apt update && sudo apt install -y build-essential git pkg-config nasm cmake \
    libx264-dev libsdl2-dev libfreetype-dev libharfbuzz-dev libfontconfig-dev \
    libva-dev libdrm-dev

# 2. 编译 FFmpeg 并安装到 ~/ffmpeg_build（4 核机器约 4 分钟）
scripts/build_ffmpeg.sh

# 3. 设置环境变量（建议写入 ~/.bashrc）
export PATH="$HOME/ffmpeg_build/bin:$PATH"
export PKG_CONFIG_PATH="$HOME/ffmpeg_build/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# 4. 生成测试素材，编译全部课程代码，运行环境检查
scripts/gen_samples.sh
cmake -S . -B build && cmake --build build
./build/L02_environment/hello_ffmpeg
```

最后一条命令输出“环境检查通过。”就说明环境已经就绪。每一步的解释见
[L02_environment](L02_environment/README.md)。

## 课程目录

带链接的课次已经发布，其余课次会陆续加入。

| 阶段 | 课次 | 对应的官方示例 |
| --- | --- | --- |
| 0 入门准备 | [L01 音视频基础概念](L01_basics/README.md) | 无 |
| | [L02 开发环境与命令行工具](L02_environment/README.md) | 全部示例的编译 |
| 1 容器与封装层 | [L03 打开媒体文件](L03_open_media/README.md) | show_metadata.c |
| | [L04 时间基与时间戳](L04_remux_timestamps/README.md) | remux.c |
| 2 解码 | [L05 视频解码](L05_decode_video/README.md) | decode_video.c |
| | [L06 音频解码](L06_decode_audio/README.md) | decode_audio.c |
| | [L07 解封装 + 解码全流程](L07_demux_decode/README.md) | demux_decode.c |
| | L08 帧类型、GOP 与 side data | extract_mvs.c |
| 3 原始数据处理 | L09 图像缩放与像素格式转换 | scale_video.c |
| | L10 音频重采样 | resample_audio.c |
| | L11 滤镜图基础 | filter_audio.c |
| | L12 解码 + 滤镜 | decode_filter_video.c、decode_filter_audio.c |
| 4 编码 | L13 视频编码 | encode_video.c |
| | L14 音频编码 | encode_audio.c |
| 5 封装输出与转码 | L15 编码并封装 | mux.c |
| | L16 完整转码 | transcode.c |
| | L17 音频转码与帧长适配 | transcode_aac.c |
| 6 I/O 与网络 | L18 自定义 I/O | avio_read_callback.c |
| | L19 网络协议与流媒体 | avio_list_dir.c、avio_http_serve_files.c |
| 7 硬件加速与工程化 | L20 硬件解码 | hw_decode.c |
| | L21 硬件编码与全 GPU 转码 | vaapi_encode.c、vaapi_transcode.c |
| | L22 工程化专题 | 无 |
| 综合项目 | 命令行媒体工具箱 mtool | 无 |

## 仓库约定

- **每讲一个目录**，命名为 `LNN_主题`。目录里的 `README.md` 列出本讲要点、操作步骤和练习。
- **两种构建方式**。在仓库根目录用 CMake 一次编译所有课次；或者进入某一讲的目录直接 `make`，
  产物在该目录的 `build/` 下。两种方式都通过 pkg-config 查找 FFmpeg。
- **测试素材不入库**。`scripts/gen_samples.sh` 会在 `samples/` 下生成所有课次共用的素材。
  脚本固定了编码参数和线程数，在不同机器上用同一版本的 FFmpeg 生成，得到的音视频数据完全相同，便于核对结果。
- **验证结果，而不只是“能跑”**。每个练习都要求用 ffprobe、ffplay 或校验和确认输出正确。

## 向上游反馈

制作课程时在官方示例里发现的问题（运行失败、已删除的选项、与当前规范不符的写法等）
记录在 [upstream_patches](upstream_patches/README.md) 目录，每个问题附带一个可以直接应用到 FFmpeg 源码的 patch，
用于向 FFmpeg 社区提交。课程里用到这些示例时，会说明问题所在和绕过的办法。

## 旧版教程

本仓库 2016–2018 年的旧版教程（FFmpeg 3.4.2、Visual Studio 2015）保留在
[`obsolete` 分支](https://github.com/yinwenjie/FFmpeg_Tutorial/tree/obsolete)，不再维护。
旧版使用的 `avcodec_decode_video2`、`av_register_all` 等接口已被 FFmpeg 删除，
不能与当前版本的 FFmpeg 一起编译。

## 许可证

本仓库的代码以 [GPL-3.0](LICENSE) 发布。改编自 FFmpeg `doc/examples` 的文件保留其原有的版权声明。
