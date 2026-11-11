# 官方示例问题清单与上游 patch

制作课程时，每一讲都要把 FFmpeg `doc/examples` 里对应的官方示例编译、运行、读一遍。
过程中发现的问题记录在这个目录里，每个问题对应一个 patch，后续用来向 FFmpeg 社区提交。

收录范围是示例中的四类问题：

- **错误**：编译不出来、运行失败、输出或提示与实际行为不符；
- **已废弃**：使用了已经被删除或废弃的接口、选项；
- **不符合当前开发规范**：能运行，但做法与当前文档的建议不一致，或者每次运行都触发警告；
- **不满足现有技术需求**：示例覆盖不到现在常见的用法。

## 基线

| 项目 | 值 |
| --- | --- |
| 制作和测试所用的提交 | `a337e8ddaa`（课程固定使用的版本，2026-11-10） |
| 复核时的上游 master | `72f8c33`（2026-11-11），`doc/examples` 和 `configure` 与 `a337e8ddaa` 完全相同 |
| 测试环境 | Ubuntu 24.04，gcc 13.3，编译选项见 [L02](../L02_environment/README.md) |

全部 patch 在 `a337e8ddaa` 上编译并运行验证过，在 `72f8c33` 上用 `git am` 按顺序应用无冲突。
提交前请先在最新的 master 上重新应用一遍，确认问题没有被别人先修掉。

## 问题清单

| 编号 | 涉及文件 | 类别 | 问题 | 发现于 |
| --- | --- | --- | --- | --- |
| [0001](0001-doc-examples-transcode-fix-buffersink-option-names.patch) | transcode.c | 已废弃、错误 | 仍在设置 buffersink 已删除的选项，一运行就失败 | L02 |
| [0002](0002-doc-examples-build-avio_http_serve_files-with-make-e.patch) | Makefile、configure | 错误 | `make examples` 从不编译 avio_http_serve_files；三个示例的依赖声明写错名字 | L02 |
| [0003](0003-doc-examples-Makefile.example-add-filter_audio.patch) | Makefile.example | 错误 | 独立 Makefile 漏了 filter_audio | L02 |
| [0004](0004-doc-examples-Makefile.example-fix-the-spelling-of-.P.patch) | Makefile.example | 错误 | `.PHONY` 写成了 `.phony`，不起作用 | L02 |
| [0005](0005-doc-examples-Makefile.example-remove-the-rule-for-th.patch) | Makefile.example | 不符合规范 | 残留着已不存在的 avcodec 示例的规则 | L02 |
| [0006](0006-doc-examples-decode_audio-fix-the-hints-printed-afte.patch) | decode_audio.c | 错误、已废弃 | 提示“只输出第一个声道”与实际不符；打印的 ffplay 命令用了已失效的 `-ac` | L02 |
| [0007](0007-doc-examples-pass-the-color-properties-to-the-buffer.patch) | decode_filter_video.c、transcode.c | 不符合规范 | 没有把色彩属性传给 buffer 源，每次运行都有警告 | L02 |
| [0008](0008-doc-examples-set-pkt_timebase-when-decoding-audio-fr.patch) | demux_decode.c、decode_filter_audio.c | 不符合规范 | 没有设置 `pkt_timebase`，解码 AAC 时有警告 | L02 |
| [0009](0009-doc-examples-show_metadata-report-errors-and-clean-u.patch) | show_metadata.c | 不符合规范 | 打不开文件时不报任何信息，错误路径上不释放资源 | L03 |
| [0010](0010-doc-examples-terminate-error-messages-with-a-newline.patch) | remux.c、encode_audio.c | 错误 | 错误信息末尾缺换行，两条信息粘在一行 | L03 |
| [0011](0011-doc-examples-transcode-pass-frame-durations-on-to-th.patch) | transcode.c | 错误 | 视频包没有时长，输出的 MP4 播放时丢掉最后一帧 | L03 |
| [0012](0012-doc-examples-remux-log-input-packets-before-remappin.patch) | remux.c | 错误 | 有流被跳过时，输入包的日志用错了流的序号和时间基 | L04 |
| [0013](0013-doc-examples-remux-copy-metadata-and-dispositions.patch) | remux.c | 不满足现有需求 | 不拷贝元数据和 disposition，输出文件丢掉标题、语言和默认轨标志 | L04 |
| [0014](0014-doc-examples-do-not-replace-error-codes-with-AVERROR.patch) | remux.c、transcode.c | 不符合规范 | 丢弃真实的错误码，一律报 `Unknown error occurred` | L04 |
| [0015](0015-doc-examples-remux-set-the-DTS-of-packets-that-come-.patch) | remux.c | 已废弃 | 把没有 DTS 的包直接交给复用器，依赖一个即将删除的兼容行为 | L04 |
| [0016](0016-doc-examples-return-a-failure-status-after-a-usage-e.patch) | decode_audio.c、decode_video.c、encode_audio.c、encode_video.c | 不符合规范 | 参数不够时打印用法，退出码却是 0 | L05 |
| [0017](0017-doc-examples-decode_video-do-not-ignore-I-O-errors.patch) | decode_video.c | 错误 | 输出文件建不起来、读输入出错时照常结束，退出码是 0 | L05 |
| [0018](0018-doc-examples-decode_video-describe-the-input-and-the.patch) | decode_video.c | 错误 | 用法说明与实际不符：第二个参数是前缀不是文件名，输入必须是裸流 | L05 |

## 怎么使用

应用到 FFmpeg 源码树。各个问题在逻辑上互相独立，但有几个 patch 改的是同一个文件里相邻的行
（0003 到 0005 都改 Makefile.example，0001、0007、0011、0014 都改 transcode.c，
0010、0012 到 0015 都改 remux.c，0016 到 0018 都改 decode_video.c），所以要按编号顺序应用：

```bash
cd ~/ffmpeg_src
git am /path/to/FFmpeg_Tutorial/upstream_patches/00*.patch
make -j"$(nproc)" examples
```

向社区提交之前需要做的事：

1. **换成提交者本人的身份并签名**。patch 里的作者是生成它的 Claude，FFmpeg 要求提交者用
   `Signed-off-by` 签名。应用之后对每个提交执行 `git commit --amend --reset-author -s`，
   或者一次处理整个系列（`<base>` 是应用 patch 之前所在的提交）：
   `git rebase <base> --exec 'git commit --amend --no-edit --reset-author -s'`。
2. **决定是否保留提交信息末尾的两行**（`Co-Authored-By` 和 `Claude-Session`）。
   它们说明 patch 是 AI 协助生成的。请先确认社区当前对 AI 协助贡献的要求再决定去留。
3. **选择提交渠道**。按 `doc/developer.texi` 的说明，可以在
   <https://code.ffmpeg.org/FFmpeg/FFmpeg/pulls> 提 pull request，
   也可以用 `git send-email` 发到 ffmpeg-devel 邮件列表。
4. 可以按主题拆成几组提交，例如构建系统（0002 到 0005）一组、transcode（0001、0007、0011）一组、
   remux（0012 到 0015）一组、decode_video（0016 到 0018）一组。

这些 patch 对应的提交同时保存在 fork 仓库 `yinwenjie/FFmpeg` 的 `claude/project-thread-h6un3a` 分支上，
一个提交对应一个 patch。以后发现新问题时，在这个分支上追加提交，再用
`git format-patch -N --start-number <下一个编号>` 导出到这个目录，并在上面的清单里加一行。

## 每个问题的复现与验证

下面的命令都在 FFmpeg 源码根目录执行，`$S` 是课程仓库的 `samples/` 目录。

### 0001 transcode 运行失败

```console
$ doc/examples/transcode $S/small_360p25.mp4 /tmp/out.mp4
...
Cannot set output pixel format
Error occurred: Option not found
```

buffersink 的 `pix_fmts`、`sample_fmts`、`ch_layouts`、`sample_rates` 选项在 libavfilter 10.6.100
（2024-09-30）被数组类型的 `pixel_formats`、`sample_formats`、`channel_layouts`、`samplerates`
取代，旧选项现在已经删除。同目录的 decode_filter_video.c 和 decode_filter_audio.c 已经更新，transcode.c 被漏掉了。

修复后分别转码到 MP4、MKV、TS，都正常结束，输出里有 125 个视频包和 217 个音频包，
`ffmpeg -v error -i 输出 -f null -` 解码无报错。MP4 输出还有一个丢帧问题，见 0011。

### 0002 avio_http_serve_files 从不编译

`doc/examples/Makefile` 判断的是 `CONFIG_AVIO_HTTP_SERVE_FILES`，而 configure 生成的变量是
`CONFIG_AVIO_HTTP_SERVE_FILES_EXAMPLE`，条件永远不成立。

只改 Makefile 还不够。configure 里这个示例的依赖写成了 `avio_http_serve_files_deps`，
少了 `_example`，所以依赖（包括 `fork`）实际上没有生效。旁边的 avio_list_dir 和 avio_read_callback 有同样的笔误。
用 `--disable-avformat` 配置可以看出来：

| | 修复前 | 修复后 |
| --- | --- | --- |
| `CONFIG_AVIO_HTTP_SERVE_FILES_EXAMPLE` | yes | 关闭 |
| `CONFIG_AVIO_LIST_DIR_EXAMPLE` | yes | 关闭 |
| `CONFIG_AVIO_READ_CALLBACK_EXAMPLE` | yes | 关闭 |
| `CONFIG_SHOW_METADATA_EXAMPLE`（对照） | 关闭 | 关闭 |

修复后 `make examples` 会生成 avio_http_serve_files。运行它并用 curl 取回文件，内容与原文件一致。

### 0003 独立 Makefile 漏了 filter_audio

安装到 `share/ffmpeg/examples/Makefile` 的是 `Makefile.example`，它的示例列表里没有 filter_audio。
加进去之后还要加上 `-lm`，否则用动态库链接时报 `undefined reference to symbol 'sin'`。

### 0004 `.phony` 不起作用

make 的特殊目标区分大小写。目录里有名为 `clean` 和 `clean-test` 的文件时：

```console
$ touch clean clean-test && make clean
make: 'clean' is up to date.
```

### 0005 残留的 avcodec 规则

`avcodec: LDLIBS += -lm` 对应的示例早已不存在。

### 0006 decode_audio 的提示信息

```console
$ doc/examples/decode_audio $S/audio_44k_stereo.mp2 /tmp/out.pcm
Warning: the sample format the decoder produced is planar (s16p). This example will output the first channel only.
Play the output audio file with the command:
ffplay -f s16le -ac 2 -ar 44100 /tmp/out.pcm
```

两处都不对。`decode()` 实际上把所有声道交错写出：5 秒立体声得到 884,736 字节，
与 `ffmpeg -i audio_44k_stereo.mp2 -f s16le` 的输出逐字节相同。打印出来的命令 ffplay 也不接受：

```console
$ ffplay -f s16le -ac 2 -ar 44100 /tmp/out.pcm
Failed to set value '2' for option 'ac': Option not found
```

修复后去掉错误的警告，命令改成与 demux_decode 一致的
`ffplay -f s16le -ch_layout stereo -sample_rate 44100`，ffplay 能识别为 44100 Hz 立体声。解码输出的文件不变。

### 0007 没有把色彩属性传给 buffer 源

```console
$ doc/examples/decode_filter_video $S/small_360p25.mp4
[in @ 0x...] Changing video frame properties on the fly is not supported by all filters.
[in @ 0x...] filter context - w: 640 h: 360 fmt: 0 csp: unknown range: unknown alpha: unspecified chroma_loc: unspecified, incoming frame - w: 640 h: 360 fmt: 0 csp: unknown range: unknown alpha: unspecified chroma_loc: left pts_time: 0
```

buffer 源创建时只给了尺寸、像素格式、时间基和宽高比。送进来的帧还带有色彩空间、色彩范围、alpha 模式
和色度采样位置，只要其中一项与创建时的值（未指定）不同就会告警。H.264 解码器总是把色度采样位置设为 `left`，
所以任何 H.264 输入都会触发；带 BT.709 标记的文件还会多出色彩空间和范围的差异。
transcode 修好 0001 之后有同样的警告。

修复后把这四项从解码器上下文传给 buffer 源，两个示例对普通文件和 BT.709 文件都不再告警。

### 0008 没有设置 pkt_timebase

```console
$ doc/examples/demux_decode $S/small_360p25.mp4 /tmp/v.yuv /tmp/a.pcm
...
[aac @ 0x...] Could not update timestamps for discarded samples.
```

解码器不知道包时间戳的时间基，遇到需要丢弃采样的流（例如 MP4 里的 AAC）就无法修正时间戳。
transcode.c 和 transcode_aac.c 已经设置了 `AVCodecContext.pkt_timebase`，这两个示例没有。
修复后警告消失，两个示例输出的文件与修复前逐字节相同。

### 0009 show_metadata 的错误处理

```console
$ doc/examples/show_metadata /nonexistent.mp4; echo $?
254
```

没有任何提示，退出码是负的错误码被截断后的值。`avformat_find_stream_info()` 失败时也没有关闭输入。
修复后：

```console
$ doc/examples/show_metadata /nonexistent.mp4; echo $?
Cannot open '/nonexistent.mp4': No such file or directory
1
```

### 0010 错误信息缺换行

```console
$ doc/examples/remux /nonexistent.mp4 /tmp/out.mp4
Could not open input file '/nonexistent.mp4'Error occurred: No such file or directory
```

remux.c 有三处，encode_audio.c 有一处。

### 0011 transcode 输出的 MP4 丢掉最后一帧

修好 0001 之后，用 5 秒、125 帧的 `small_360p25.mp4` 转码成 MP4，再统计实际能读出的帧数：

```console
$ ffprobe -v error -count_frames -select_streams v \
    -show_entries stream=duration,nb_read_frames -of compact /tmp/out.mp4
stream|duration=4.960000|nb_read_frames=124
```

文件里有 125 个视频包，但最后一个的时长是 0，落在 edit list 之外，播放时被丢弃。
原因是示例只把滤镜输出帧的时间戳换算到编码器时间基，没有换算帧的时长，也没有设置
`AV_CODEC_FLAG_FRAME_DURATION`，所以视频编码器输出的包全部没有时长。
MKV 和 TS 不依赖最后一个包的时长，所以看不出问题；ffmpeg 命令行工具设置了这个标志，也没有这个问题。

修复后视频流时长为 5.000000 秒，能读出 125 帧；用 10 秒 30 fps 的 `demo_1080p30.mp4` 复验，读出 300 帧。

### 0012 remux 的输入日志用错了流

remux 不拷贝数据流和附件流。只要输入里有这样的流排在音视频流前面，它后面各路流的
`in:` 日志就会用错流的序号和时间基。用一个带 timecode 轨的 MOV 复现，流的顺序是视频、数据、音频：

```console
$ ffmpeg -v error -i $S/small_360p25.mp4 -c copy -timecode 01:00:00:00 /tmp/tc.mov
$ ffmpeg -v error -i /tmp/tc.mov -map 0:v -map 0:d -map 0:a -c copy /tmp/tc_mid.mov
$ doc/examples/remux /tmp/tc_mid.mov /tmp/out.mp4 2>/dev/null | grep -m2 "stream_index:1"
in: pts:-1024 pts_time:-40.96 dts:-1024 dts_time:-40.96 duration:1024 duration_time:40.96 stream_index:1
out: pts:-1024 pts_time:-0.02322 dts:-1024 dts_time:-0.02322 duration:1024 duration_time:0.02322 stream_index:1
```

音频包在输入里是第 2 路流，时间基是 1/44100。`in:` 一行却显示成第 1 路流，
并且用数据流的时间基 1/25 换算，得到 -40.96 秒。原因是打印之前 `pkt->stream_index` 已经被改成输出流的序号。
修复后 `in:` 一行是 `pts_time:-0.02322 ... stream_index:2`。输出文件不受影响，修复前后逐字节相同。

### 0013 remux 丢掉元数据和 disposition

```console
$ ffmpeg -v error -i $S/small_360p25.mp4 -c copy -metadata title="L04 test" \
    -metadata:s:a:0 language=chi /tmp/tagged.mkv
$ doc/examples/remux /tmp/tagged.mkv /tmp/out.mkv >/dev/null 2>&1
$ for f in /tmp/tagged.mkv /tmp/out.mkv; do ffprobe -v error -of compact -show_entries \
    format_tags=title:stream=index:stream_tags=language:stream_disposition=default $f; done
stream|index=0|disposition:default=1
stream|index=1|disposition:default=1|tag:language=chi
format|tag:title=L04 test
stream|index=0|disposition:default=0
stream|index=1|disposition:default=0
format
```

| | 输入 | 修复前的输出 | 修复后的输出 |
| --- | --- | --- | --- |
| 容器的 `title` | L04 test | 没有 | L04 test |
| 音频流的 `language` | chi | 没有 | chi |
| 两路流的 `disposition:default` | 1 | 0 | 1 |

示例只拷贝了 `codecpar`。修复后输出的标签与 `ffmpeg -c copy` 的结果一致，
MKV、MP4、MOV、TS、FLV 五种输出都能正常写出。

### 0014 真实的错误码被换成 AVERROR_UNKNOWN

```console
$ doc/examples/remux $S/small_360p25.mp4 /tmp/out.xyz 2>&1 | tail -2
Could not create output context
Error occurred: Unknown error occurred
```

`avformat_alloc_output_context2()` 返回的是 `AVERROR(EINVAL)`，示例没有接收返回值。
修复后最后一行是 `Error occurred: Invalid argument`。transcode.c 有同样的写法，一并修改；
qsv_transcode.c 和 vaapi_transcode.c 本来就是用返回值的。

### 0015 remux 把没有 DTS 的包交给复用器

MKV 每个包只存一个时间戳。视频有 B 帧时，解封装器对开头的几个包给不出 DTS：

```console
$ doc/examples/remux $S/small_360p25.mkv /tmp/out.mp4 2>&1 | grep -m3 -e "^in:" -e "Timestamps"
[mp4 @ 0x...] Timestamps are unset in a packet for stream 0. This is deprecated and will stop working in the future. Fix your code to set the timestamps properly
in: pts:0 pts_time:0 dts:NOPTS dts_time:NOPTS duration:40 duration_time:0.04 stream_index:0
in: pts:80 pts_time:0.08 dts:NOPTS dts_time:NOPTS duration:40 duration_time:0.04 stream_index:0
```

现在还能得到正确的文件，是因为 `libavformat/mux.c` 里保留着一段替调用者猜时间戳的旧代码
（`FF_API_COMPUTE_PKT_FIELDS2`，定义为 `LIBAVFORMAT_VERSION_MAJOR < 64`，当前主版本是 63）。
这段代码删除之后，同一位置会直接返回 `AVERROR(EINVAL)`，示例对这类输入就不能用了。

修复的做法和 ffmpeg 命令行工具拷贝流时一样：第一个包的 DTS 取 PTS 减去重排延迟（`video_delay` 个包的时长），
之后按上一个包的 DTS 加时长往下推，直到解封装器给出 DTS 为止。
修复后没有警告。MKV 到 MP4、MKV 到 TS，以及 MP4、TS、FLV 作输入的另外六种组合，输出文件与修复前逐字节相同
（输出为 MKV 时比较的是每个包的时间戳和大小，因为 MKV 文件里有随机生成的 UID）。

### 0016 用法错误时退出码是 0

```console
$ doc/examples/decode_video; echo $?
Usage: doc/examples/decode_video <input file> <output file>
And check your input file is encoded by mpeg1video please.
0
```

decode_audio、encode_audio、encode_video 也是这样，其中 encode_audio 是 `return 0`，另外三个是 `exit(0)`。
脚本或 Makefile 调用它们时判断不出程序什么也没做。其余示例在同样的情况下返回的都是非 0。
修复后这四个示例都返回 1。

### 0017 decode_video 忽略读写错误

输出文件建不起来（这里是目录不存在）：

```console
$ doc/examples/decode_video $S/video_360p25.m1v /nonexistent/frame > /tmp/log 2>&1; echo $?
0
$ grep -c "saving frame" /tmp/log; grep -c "Could not open" /tmp/log
125
125
```

`pgm_save()` 打不开文件时打印一条信息就返回，程序把 125 帧全部解完，一个文件也没写出来，退出码是 0。

读输入出错（这里把一个目录当作输入文件，`fread()` 失败）：

```console
$ doc/examples/decode_video $S /tmp/frame; echo $?
0
```

读循环遇到 `ferror()` 只是 `break`，没有任何提示。

修复后两种情况都报错并以 1 退出：前者在第一帧就停下，只打印一次 `Could not open /nonexistent/frame-1`；
后者打印 `Error while reading <文件名>`。输入正常时，125 个 PGM 文件与修复前逐字节相同。

### 0018 decode_video 的用法说明

用法说明是这两行：

```text
Usage: doc/examples/decode_video <input file> <output file>
And check your input file is encoded by mpeg1video please.
```

两处都会把人带偏。第二个参数其实是前缀，`decode_video in.m1v out.pgm` 得到的是 `out.pgm-1`、`out.pgm-2` 这样的文件。
输入要求写的是“用 mpeg1video 编码”，照着做最自然的办法是转一个 `.mpg`，而 `.mpg` 是 MPEG-PS 容器：

```console
$ ffmpeg -v error -y -i $S/small_360p25.mp4 -an -c:v mpeg1video -b:v 1M /tmp/in.mpg
$ doc/examples/decode_video /tmp/in.mpg /tmp/frame > /tmp/log 2>&1; echo $?
0
$ grep -c "saving frame" /tmp/log; grep -c "concealing" /tmp/log
124
124
```

示例把文件内容原样交给 MPEG-1 的 parser，容器的包头混进了帧数据，每一帧都触发错误隐藏，存下来的图片几乎是一片灰。
换成 AVI 则直接失败，报 `Invalid frame dimensions 0x0`。

修复后的用法说明写明输入必须是裸流，给出生成裸流的命令（`ffmpeg -i INPUT -c:v mpeg1video -f mpeg1video OUTPUT`），
并说明每一帧存成 `<output prefix>-<frame number>`。文件开头的说明注释同步修改。只改了文字，不影响行为。

## 已记录、暂未制作 patch 的观察项

这些问题需要结合对应课次仔细研究之后再决定怎么改，到那一讲时处理。

| 涉及文件 | 观察 | 计划处理的课次 |
| --- | --- | --- |
| scale_video.c、mux.c | 使用 `sws_getContext()` 加 `sws_scale()` 的旧式写法。libswscale 已提供 `sws_alloc_context()` 和按帧工作的 `sws_scale_frame()`，旧接口尚未标记废弃 | L09 |
| decode_audio.c | `decode()` 用 `frame->data[ch]` 取各声道，只对 planar 格式成立。示例写死了 MP2 解码器所以没出问题，换成输出 packed 格式的解码器会崩溃 | L06 |
| transcode.c | 编码器没有继承输入的色彩属性，BT.709 的输入转码后 `color_space` 和 `color_range` 变成 unknown | L16 |
| avio_read_callback.c | 只提供读回调、没有 seek 回调。`moov` 在文件末尾的 MP4 读不到完整的流参数（报 `partial file` 和 `unspecified pixel format`） | L18 |
| avio_http_serve_files.c、filter_audio.c | 用 `uint8_t` 数组当字符串，脱离源码树用 `-Wall` 编译时有 `-Wpointer-sign` 警告 | L11、L19 |

用 `-Wdeprecated-declarations` 编译全部能独立编译的示例，没有发现使用已标记废弃的函数。
qsv_decode.c 和 qsv_transcode.c 需要 Intel 的库才能编译，还没有检查。

示例之外顺带看到的：`libavfilter/buffersink.c` 里 `alphamodes` 和 `samplerates` 两个选项的说明文字是从别的选项复制来的
（分别写成了 color ranges 和 sample formats）。不在本目录的范围内，没有制作 patch。
