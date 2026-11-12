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
| 复核时的上游 master | `23c4540`（2026-11-12），`doc/examples` 和 `configure` 与 `a337e8ddaa` 完全相同 |
| 测试环境 | Ubuntu 24.04，gcc 13.3，编译选项见 [L02](../L02_environment/README.md) |

全部 patch 在 `a337e8ddaa` 上编译并运行验证过，在 `23c4540` 上用 `git am` 按顺序应用无冲突。
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
| [0019](0019-doc-examples-decode_audio-fix-a-crash-when-no-data-c.patch) | decode_audio.c | 错误 | 输入文件是空的或者读不出数据时段错误 | L06 |
| [0020](0020-doc-examples-decode_audio-do-not-ignore-I-O-errors.patch) | decode_audio.c | 错误 | 输出写不进去、读输入出错时照常提示播放命令，退出码是 0 | L06 |
| [0021](0021-doc-examples-decode_audio-support-packed-sample-form.patch) | decode_audio.c | 不满足现有需求 | 只能处理 planar 采样格式，换一个输出 packed 格式的解码器就崩溃 | L06 |
| [0022](0022-doc-examples-decode_audio-flush-the-parser-at-the-en.patch) | decode_audio.c | 不符合规范 | 读到文件末尾没有冲刷 parser，结尾的数据被丢掉 | L06 |
| [0023](0023-doc-examples-decode_audio-describe-the-input-in-the-.patch) | decode_audio.c | 不符合规范 | 用法说明没有说输入必须是 MP2 裸流 | L06 |
| [0024](0024-doc-examples-decode_audio-zero-the-padding-of-the-in.patch) | decode_audio.c | 不符合规范 | 输入缓冲区末尾的填充字节没有清零 | L06 |
| [0025](0025-doc-examples-demux_decode-return-a-failure-exit-stat.patch) | demux_decode.c | 错误 | 输出文件打不开、输入里没有音视频流时报了错，退出码却是 0 | L07 |
| [0026](0026-doc-examples-demux_decode-stop-with-an-error-when-de.patch) | demux_decode.c | 错误 | 解码中途出错后仍然打印 `Demuxing succeeded.`，退出码是 0 | L07 |
| [0027](0027-doc-examples-demux_decode-print-audio-timestamps-in-.patch) | demux_decode.c | 错误 | 打印音频帧时间用错了时间基，输入是 MPEG-TS 时数值不对 | L07 |
| [0028](0028-doc-examples-demux_decode-write-all-the-audio-channe.patch) | demux_decode.c | 错误、不满足现有需求 | planar 音频只写第一个声道；packed 多声道音频每帧只写前一部分 | L07 |
| [0029](0029-doc-examples-demux_decode-do-not-ignore-write-errors.patch) | demux_decode.c | 错误 | 输出写不进去时照常报告成功，退出码是 0 | L07 |
| [0030](0030-doc-examples-demux_decode-fix-a-typo-in-the-file-des.patch) | demux_decode.c | 错误 | 文件开头的说明把输出写成了 `input files` | L07 |
| [0031](0031-doc-examples-extract_mvs-return-a-failure-exit-statu.patch) | extract_mvs.c | 错误 | 输入里没有视频流时报了错，退出码却是 0 | L08 |
| [0032](0032-doc-examples-extract_mvs-do-not-leak-the-codec-conte.patch) | extract_mvs.c | 不符合规范 | 解码器打不开时不释放解码器上下文 | L08 |
| [0033](0033-doc-examples-extract_mvs-check-for-errors-when-writi.patch) | extract_mvs.c | 错误 | 结果写不出去时照常结束，退出码是 0 | L08 |
| [0034](0034-doc-examples-scale_video-do-not-free-uninitialized-p.patch) | scale_video.c | 错误 | 建立缩放上下文失败时，对没有初始化的指针调用 `av_freep()` | L09 |
| [0035](0035-doc-examples-scale_video-fix-the-end-of-the-usage-me.patch) | scale_video.c | 错误 | 用法说明最后一句的句号印到了下一行开头 | L09 |
| [0036](0036-doc-examples-scale_video-check-for-scaling-and-write.patch) | scale_video.c | 错误 | 不检查 `sws_scale()` 和 `fwrite()` 的返回值，写不出去时照常报告成功 | L09 |
| [0037](0037-doc-examples-scale_video-use-sws_scale_frame.patch) | scale_video.c | 不符合规范 | 用的是 swscale.h 标为 legacy 的 `sws_getContext()` 加 `sws_scale()`，以及已不推荐的 `SWS_BILINEAR` 标志 | L09 |
| [0038](0038-doc-examples-resample_audio-fix-the-suggested-ffplay.patch) | resample_audio.c | 已废弃 | 结束时提示的 ffplay 命令用了已经删除的 `-channel_layout`、`-channels` 选项，照着执行会报错 | L10 |
| [0039](0039-doc-examples-resample_audio-do-not-report-success-wh.patch) | resample_audio.c | 错误 | 输出缓冲区扩容失败时跳出循环，随后照常报告成功，退出码是 0 | L10 |
| [0040](0040-doc-examples-resample_audio-check-for-write-errors.patch) | resample_audio.c | 错误 | 不检查 `fwrite()` 的返回值，写不出去时照常报告成功 | L10 |
| [0041](0041-doc-examples-resample_audio-flush-the-resampler-at-t.patch) | resample_audio.c | 错误 | 结束时不冲刷重采样器，输出少了最后 17 个采样 | L10 |
| [0042](0042-doc-examples-filter_audio-use-char-buffers-for-strin.patch) | filter_audio.c | 不符合规范 | 字符串缓冲区声明成 `uint8_t` 数组，用 `-Wall` 编译有 4 个 `-Wpointer-sign` 警告 | L11 |
| [0043](0043-doc-examples-filter_audio-generate-a-continuous-sine.patch) | filter_audio.c | 错误 | 生成的正弦波在每个帧的边界跳过一个采样，波形不连续 | L11 |
| [0044](0044-doc-examples-filter_audio-actually-use-the-MD5-conte.patch) | filter_audio.c | 不符合规范 | 分配了 MD5 上下文却没有用它（`av_md5_sum()` 不使用调用者的上下文） | L11 |
| [0045](0045-doc-examples-filter_audio-print-the-error-when-the-g.patch) | filter_audio.c | 错误 | 滤镜图建立失败时只打印半句话，没有错误原因和换行 | L11 |
| [0046](0046-doc-examples-filter_audio-do-not-leak-the-graph-when.patch) | filter_audio.c | 错误 | 滤镜图建立失败时不释放已经分配的图和滤镜 | L11 |
| [0047](0047-doc-examples-filter_audio-flush-the-filtergraph-at-t.patch) | filter_audio.c | 错误 | 结束时不冲刷滤镜图，输出少了最后 16 个采样 | L11 |

## 怎么使用

应用到 FFmpeg 源码树。各个问题在逻辑上互相独立，但有几个 patch 改的是同一个文件里相邻的行
（0003 到 0005 都改 Makefile.example，0001、0007、0011、0014 都改 transcode.c，
0010、0012 到 0015 都改 remux.c，0016 到 0018 都改 decode_video.c，
0006、0016、0019 到 0024 都改 decode_audio.c，
0008、0025 到 0030 都改 demux_decode.c，
0031 到 0033 都改 extract_mvs.c，
0034 到 0037 都改 scale_video.c，
0038 到 0041 都改 resample_audio.c，
0042 到 0047 都改 filter_audio.c），所以要按编号顺序应用：

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
   remux（0012 到 0015）一组、decode_video（0016 到 0018）一组、decode_audio（0006、0019 到 0024）一组、
   demux_decode（0025 到 0030）一组、extract_mvs（0031 到 0033）一组、
   scale_video（0034 到 0037）一组、resample_audio（0038 到 0041）一组、
   filter_audio（0042 到 0047）一组。

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

### 0019 decode_audio 在空输入上崩溃

```console
$ : > /tmp/empty.mp2
$ doc/examples/decode_audio /tmp/empty.mp2 /tmp/out.pcm; echo $?
Segmentation fault (core dumped)
139
```

把一个目录当作输入文件也是同样的结果。`AVFrame` 是在读循环里面第一次用到时才分配的，
第一次 `fread()` 就读不到数据时循环体一次也不执行，随后冲刷解码器的那次调用把空指针交给了 `avcodec_receive_frame()`。

修复后帧在循环之前分配。这种情况下一帧也没有解出来，结尾那条 ffplay 提示里的参数无从得知
（只修崩溃的话会打印 `-ch_layout 0 channels -sample_rate 0`），所以改为报
`No audio frame could be decoded from <文件名>` 并以 1 退出。

### 0020 decode_audio 忽略读写错误

输出写不进去（用 `/dev/full` 模拟磁盘已满）：

```console
$ doc/examples/decode_audio $S/audio_44k_stereo.mp2 /dev/full; echo $?
Play the output audio file with the command:
ffplay -f s16le -ch_layout stereo -sample_rate 44100 /dev/full
0
```

`fwrite()` 和 `fclose()` 的结果都没有检查，一个字节也没写成功，程序照样提示去播放输出文件。
读输入中途出错时，读循环像读到文件末尾一样正常结束，输出只有一部分，同样报告成功。

修复后检查两个文件的错误标志，以及输出文件 `fclose()` 的返回值（缓冲区里剩下的数据是在这一步写出去的），
出错时打印 `Error while writing <文件名>` 或 `Error while reading <文件名>` 并以 1 退出。
输出文件改为在打印提示之前关闭，只有完整写完才会出现播放提示。输入正常时输出与修复前逐字节相同。

### 0021 decode_audio 只能处理 planar 格式

示例写死了 MP2 解码器，它输出 s16p，所以原样运行没有问题。问题出在把它当模板用的时候。
只改一处，把 `AV_CODEC_ID_MP2` 换成 `AV_CODEC_ID_FLAC`（FLAC 解码器输出 packed 的 s16），拿一个双声道文件来解：

```console
$ ffmpeg -v error -i $S/audio_44k_stereo.mp2 -c:a flac /tmp/stereo.flac
$ ./decode_audio_flac /tmp/stereo.flac /tmp/out.pcm; echo $?
Segmentation fault (core dumped)
139
```

`decode()` 对每个声道都去读 `frame->data[ch]`。packed 格式只有一个平面，`data[1]` 是空指针。
另外 `data[]` 只有 8 个位置，planar 格式超过 8 个声道时必须用 `extended_data`。
而文件里的 `get_format_from_sample_fmt()` 列的全是 packed 格式，看上去像是什么格式都能处理。

修复后 packed 的帧整块写出，planar 的帧通过 `extended_data` 逐个采样交错写出，
采样格式和声道数改用帧自己的（`frame->format`、`frame->ch_layout`）。MP2 的输出与修复前逐字节相同，
改成 FLAC 的版本不再崩溃。

### 0022 decode_audio 没有冲刷 parser

`av_parser_parse2()` 的文档要求在输入结束时用长度 0 再调用一次，parser 才会把手里剩下的数据交出来。
decode_video.c 是这样做的，decode_audio.c 没有。

对完整的 MP2 文件没有影响，因为 MPEG 音频的 parser 从帧头就知道一帧多长，收齐就交出来。
文件在一帧中间被截断时有区别，最后那个不完整的帧 ffmpeg 会解，示例把它丢了：

```console
$ head -c 3000 $S/audio_44k_stereo.mp2 > /tmp/short.mp2
$ doc/examples/decode_audio /tmp/short.mp2 /tmp/out.pcm > /dev/null; wc -c < /tmp/out.pcm
32256
$ ffmpeg -v error -i /tmp/short.mp2 -f s16le - | wc -c
36864
```

要看到下一帧的开头才能确定上一帧结尾的 parser 扣下的数据更多。接着 0021 把解码器换成 FLAC，
5 秒的文件少了结尾的 14 帧：

```console
$ ./decode_audio_flac /tmp/stereo.flac /tmp/out.pcm > /dev/null; wc -c < /tmp/out.pcm
820224
$ ffmpeg -v error -i /tmp/stereo.flac -f s16le - | wc -c
884736
```

修复后在读循环之后用长度 0 反复调用 parser，直到它不再交出数据。上面两个例子的输出都与 ffmpeg 的结果逐字节相同
（分别是 36864 和 884736 字节）。

### 0023 decode_audio 的用法说明

```console
$ doc/examples/decode_audio
Usage: doc/examples/decode_audio <input file> <output file>
$ doc/examples/decode_audio $S/small_360p25.mp4 /tmp/out.pcm
[mp2 @ 0x564036e81640] Header missing
Error submitting the packet to the decoder
```

用法说明只说要一个输入文件。示例把文件内容原样交给 MPEG 音频的 parser，再用 MP2 解码器解，所以输入必须是 MP2 裸流。
给它 MP4、AAC，或者装在 MPEG-TS、Matroska、WAV 里的 MP2，得到的都是上面这两行，看不出错在哪里。

修复后的用法说明写明输入必须是 MP2 裸流，给出生成它的命令（`ffmpeg -i INPUT -c:a mp2 OUTPUT.mp2`），
文件开头的说明注释同步修改。只改了文字，不影响行为。

### 0024 decode_audio 没有把填充字节清零

`AV_INPUT_BUFFER_PADDING_SIZE` 的文档要求输入数据后面多留 64 个字节，并且提醒这些字节不为 0 时，
损坏的 MPEG 码流可能让解码器读过头。decode_video.c 在读文件之前有一句
`memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE)`，decode_audio.c 留了位置但从来没有清零：

```console
$ grep -n "memset\|PADDING" doc/examples/decode_audio.c doc/examples/decode_video.c
doc/examples/decode_audio.c:126:    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
doc/examples/decode_video.c:96:    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
doc/examples/decode_video.c:120:    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
```

文件最后一帧后面的内容是缓冲区里原来的东西：前几次读进来的旧数据，文件比缓冲区小时则是没有初始化的栈内存。
用课程素材和 valgrind 没有观察到实际的越界读，这个 patch 是按文档的要求补齐。

修复后每次填充缓冲区之后把数据末尾的 64 个字节清零。输出与修复前逐字节相同。

### 0025 demux_decode 出错时退出码是 0

```console
$ doc/examples/demux_decode $S/small_360p25.mp4 /nonexistent/v.yuv /tmp/a.pcm; echo "exit=$?"
Could not open destination file /nonexistent/v.yuv
exit=0
```

`main()` 最后一句是 `return ret < 0`，而三处出错的地方把 `ret` 设成了 1：两个输出文件打不开，以及输入里既没有视频也没有音频。
1 不小于 0，所以退出码是 0。

修复后这三处改用负的错误码（`AVERROR(errno)` 和 `AVERROR_STREAM_NOT_FOUND`），退出码是 1。

### 0026 demux_decode 解码出错后仍然报告成功

把素材中间的一段数据清零，做一个损坏的文件：

```console
$ cp $S/small_360p25.mp4 /tmp/bad.mp4
$ dd if=/dev/zero of=/tmp/bad.mp4 bs=1 seek=200000 count=50000 conv=notrunc
$ doc/examples/demux_decode /tmp/bad.mp4 /tmp/v.yuv /tmp/a.pcm; echo "exit=$?"
...
Error submitting a packet for decoding (Invalid data found when processing input)
Demuxing succeeded.
Play the output video file with the command:
...
exit=0
$ ls -l /tmp/v.yuv
... 16243200 ... /tmp/v.yuv
```

`decode_packet()` 失败后只是 `break` 出读循环，接着冲刷解码器、打印成功信息和播放命令。
`ret` 在后面被别的函数的返回值覆盖，退出码也是 0。视频只写了 47 帧（完整的是 125 帧，43,200,000 字节），调用它的脚本无从知道。
另外 `av_read_frame()` 返回 `AVERROR_EOF` 以外的错误、冲刷时出错，原来都没有检查。

修复后这几种情况都直接跳到清理代码，退出码是 1，不再打印成功信息。
冲刷改为只对实际用到的流做：解码器上下文在 `avcodec_open2()` 之前分配，打开失败时指针不是空的，原来会对一个没打开的解码器调用冲刷。

### 0027 demux_decode 打印的音频时间用错了时间基

```console
$ ffprobe -v error -select_streams a -show_entries stream=time_base,start_time -of compact $S/small_360p25.ts
stream|time_base=1/90000|start_time=1.456778
$ doc/examples/demux_decode $S/small_360p25.ts /tmp/v.yuv /tmp/a.pcm 2>/dev/null | grep -m2 audio_frame
audio_frame n:0 nb_samples:1024 pts:2.973016
audio_frame n:1 nb_samples:1024 pts:3.020408
```

解码出来的帧的 pts 用的是包的时间基，也就是 `AVStream.time_base`。示例换算成秒时用了 `AVCodecContext.time_base`，
这个字段解码时不由用户设置，音频解码器里它恰好是采样率的倒数。MP4 的音频流时间基也是采样率的倒数，所以用 MP4 测试看不出问题；
MPEG-TS 的时间基是 1/90000，第一帧的 pts 是 131110，被算成 131110 ÷ 44100 = 2.973 秒，正确的是 131110 ÷ 90000 = 1.457 秒。

修复后改用 `audio_stream->time_base`，上面两行变成 `pts:1.456778` 和 `pts:1.48`。

### 0028 demux_decode 没有写全音频数据

```console
$ doc/examples/demux_decode $S/small_360p25.mp4 /tmp/v.yuv /tmp/a.pcm 2>/dev/null | tail -3
Warning: the sample format the decoder produced is planar (fltp). This example will output the first channel only.
Play the output audio file with the command:
ffplay -f f32le -ch_layout mono -sample_rate 44100 /tmp/a.pcm
```

planar 格式只写第一个声道，这一点示例自己说明了，但现在常见的解码器输出的几乎都是 planar，双声道的输入得到的总是单声道。
更严重的是 packed 格式。注释里说 packed“works fine”，实际写出去的字节数是 `nb_samples × 每采样字节数`，没有乘声道数：

```console
$ ffmpeg -v error -i $S/small_360p25.mp4 -c:v copy -c:a flac /tmp/flac.mkv
$ doc/examples/demux_decode /tmp/flac.mkv /tmp/v.yuv /tmp/a.pcm 2>/dev/null | tail -1
ffplay -f s32le -ch_layout stereo -sample_rate 44100 /tmp/a.pcm
$ ls -l /tmp/a.pcm
... 882000 ... /tmp/a.pcm
```

5 秒双声道 s32 应该是 1,764,000 字节，实际只有一半：每一帧只写了前半帧的左右声道数据，后半帧丢掉了。
按它打印的命令播放，时长是 2.5 秒，内容是断开的片段。

修复后 packed 格式写出整个平面，planar 格式逐个采样交错写出（与 0021 之后的 decode_audio 相同），
打印的播放命令使用真实的声道布局。MP4 和 FLAC 两种输入的音频输出都与 `ffmpeg -vn -f f32le`、`ffmpeg -vn -f s32le` 的结果 md5 相同。

### 0029 demux_decode 忽略写入错误

```console
$ doc/examples/demux_decode $S/small_360p25.mp4 /dev/full /dev/full 2>/dev/null | tail -3; echo "exit=${PIPESTATUS[0]}"
Warning: the sample format the decoder produced is planar (fltp). This example will output the first channel only.
Play the output audio file with the command:
ffplay -f f32le -ch_layout mono -sample_rate 44100 /dev/full
exit=0
```

`/dev/full` 模拟磁盘已满。`fwrite()` 和 `fclose()` 的结果都没有检查，一个字节也没写成功，示例照样报告成功。

修复后每写一帧检查一次文件的错误标志，并且在报告成功之前关闭输出文件（缓冲区里剩下的数据这时才真正写出，也可能失败）。
同样的命令打印 `Error writing to /dev/full`，退出码是 1。

### 0030 demux_decode 文件说明里的笔误

文件开头的说明写的是 `Write the output as raw audio and input files to be played by ffplay`，`input` 应为 `video`。只改注释。

### 0031 extract_mvs 没有视频流时退出码是 0

```console
$ doc/examples/extract_mvs $S/audio_44k_stereo.mp2 > /dev/null; echo "exit=$?"
...
Could not find video stream in the input, aborting
exit=0
```

和 0025 是同一种写法：`main()` 最后是 `return ret < 0`，这里却把 `ret` 设成了 1。修复后改用 `AVERROR_STREAM_NOT_FOUND`，退出码是 1。

### 0032 extract_mvs 不释放解码器上下文

`open_codec_context()` 里，`avcodec_parameters_to_context()` 失败时释放了 `dec_ctx`，紧接着的 `avcodec_open2()` 失败时却直接返回。
`dec_ctx` 只有在成功之后才存进全局变量 `video_dec_ctx`，所以 `main()` 结尾的 `avcodec_free_context(&video_dec_ctx)` 也释放不到它。
这是读代码发现的，用课程素材触发不了 `avcodec_open2()` 失败。

修复后在这条错误路径上补了 `avcodec_free_context(&dec_ctx)`。顺带把分配失败时返回的 `AVERROR(EINVAL)` 改成 `AVERROR(ENOMEM)`。

### 0033 extract_mvs 不检查输出是否写成功

```console
$ doc/examples/extract_mvs $S/small_360p25.mp4 2>/dev/null > /dev/full; echo "exit=$?"
exit=0
```

运动矢量打印在标准输出上，通常重定向到文件，它就是这个程序的全部结果。磁盘满了写不进去，程序不知道。

修复后在结束前 `fflush(stdout)` 并检查错误标志，失败时打印 `Error writing to the standard output`，退出码是 1。
正常情况下的输出与修复前逐字节相同（174,394 行）。

### 0034 scale_video 释放没有初始化的指针

```console
$ valgrind -q doc/examples/scale_video /tmp/out.rgb 100000x100000
Impossible to create scale context for the conversion fmt:yuv420p s:320x240 -> fmt:rgb24 s:100000x100000
==8392== Conditional jump or move depends on uninitialised value(s)
==8392==    at 0x4849845: free (in /usr/libexec/valgrind/vgpreload_memcheck-amd64-linux.so)
==8392==    by 0x109477: main (scale_video.c:137)
==8392==
==8392== Conditional jump or move depends on uninitialised value(s)
==8392==    at 0x4849845: free (in /usr/libexec/valgrind/vgpreload_memcheck-amd64-linux.so)
==8392==    by 0x109481: main (scale_video.c:138)
```

`src_data` 和 `dst_data` 两个数组定义时没有初始化。`sws_getContext()` 失败后直接跳到清理代码，那时图像缓冲区还没有分配，
`av_freep(&src_data[0])` 释放的是栈上的随机值。不用 valgrind 时这次运行碰巧没有崩溃，但这是未定义行为。
源图像分配失败时 `dst_data[0]` 也有同样的问题。

修复后两个数组初始化为 `NULL`，valgrind 不再报告。

### 0035 scale_video 用法说明末尾的句号

```console
$ doc/examples/scale_video 2>&1 | cat -A | tail -3
This program generates a series of pictures, rescales them to the given output_size and saves them to an output file named output_file$
.$
$
```

字符串里写的是 `output_file\n.`，换行和句号的顺序反了。修复后句号在行尾。

### 0036 scale_video 不检查缩放和写文件是否成功

```console
$ doc/examples/scale_video /dev/full 320x240; echo "exit=$?"
Scaling succeeded. Play the output file with the command:
ffplay -f rawvideo -pix_fmt rgb24 -video_size 320x240 /dev/full
exit=0
```

`sws_scale()` 和 `fwrite()` 的返回值都没有检查。修复后两处都检查，报告成功之前再 `fflush()` 一次，
上面的命令打印 `Error writing to /dev/full`，退出码是 1。正常情况下的输出与修复前逐字节相同。

### 0037 scale_video 改用 sws_scale_frame()

`libswscale/swscale.h` 把 `sws_getContext()`、`sws_scale()` 归在“Legacy (stateful) API”一节，
`sws_init_context()` 的说明写着推荐的用法是把帧的属性设置好、直接调用 `sws_scale_frame()`；
`SWS_BILINEAR` 这类选择缩放算法的标志也注明“Deprecated in favor of `SwsContext.scaler`”。
旧接口没有加废弃标记，仍然可以用（`doc/APIchanges` 里写过 `sws_getContext` 不会被移除），所以这个 patch 的类型是“不符合规范”而不是错误。
示例是新用户照着写代码的地方，应该演示推荐的写法。

修改后：图像放在 `AVFrame` 里（`av_frame_get_buffer()` 分配），上下文用 `sws_alloc_context()` 分配，
缩放算法用 `sws_ctx->scaler = SWS_SCALE_BILINEAR` 选择，转换用 `sws_scale_frame()`，释放用 `sws_free_context()`。
`AVFrame` 的每一行可能有填充，所以输出改成逐行写。

```console
$ for s in 100x75 320x240 640x480; do old/scale_video a.rgb $s; new/scale_video b.rgb $s; cmp a.rgb b.rgb && echo "$s same"; done 2>/dev/null
100x75 same
320x240 same
640x480 same
```

输出尺寸为 1x1 这种极端情况下两种接口的结果不同（新接口走的是另一条初始化路径），常规尺寸逐字节相同。
这个 patch 改动的行数多，上游是否接受要看维护者的意见，提交时建议和 0034 到 0036 分开。

### 0038 resample_audio 提示的 ffplay 命令不能用

```console
$ doc/examples/resample_audio /tmp/out.pcm > /dev/null
Resampling succeeded. Play the output file with the command:
ffplay -f s16le -channel_layout 3.0 -channels 3 -ar 44100 /tmp/out.pcm
$ ffplay -f s16le -channel_layout 3.0 -channels 3 -ar 44100 /tmp/out.pcm
Failed to set value '3.0' for option 'channel_layout': Option not found
```

`-channel_layout` 和 `-channels` 是旧的声道布局接口时期的选项，已经删除。修复后提示的是 `-ch_layout 3.0 -ar 44100`，
声道数由布局决定，不用再写。用 `SDL_AUDIODRIVER=dummy ffplay -nodisp -autoexit` 验证新命令可以正常播放到结束。

### 0039 resample_audio 扩容失败后报告成功

这是读代码发现的，正常运行触发不了。循环里输出缓冲区不够大时会重新分配：

```c
ret = av_samples_alloc(dst_data, &dst_linesize, dst_nb_channels,
                       dst_nb_samples, dst_sample_fmt, 1);
if (ret < 0)
    break;
```

`break` 之后执行的是循环后面的代码：`ret` 被 `get_format_from_sample_fmt()` 的返回值覆盖，接着打印 `Resampling succeeded`，退出码是 0，而输出文件只写了一部分。
修复后和其他出错路径一样打印错误信息并 `goto end`。

### 0040 resample_audio 不检查写文件是否成功

```console
$ doc/examples/resample_audio /dev/full > /dev/null; echo "exit=$?"
Resampling succeeded. Play the output file with the command:
ffplay -f s16le -channel_layout 3.0 -channels 3 -ar 44100 /dev/full
exit=0
```

修复后检查 `fwrite()` 的返回值，报告成功之前再 `fflush()` 一次，上面的命令打印 `Error writing to /dev/full`，退出码是 1。

### 0041 resample_audio 结束时不冲刷重采样器

```console
$ doc/examples/resample_audio /tmp/out.pcm | awk '{split($2,a,":"); split($3,b,":"); i+=a[2]; o+=b[2]} END {print i, o}'
480256 441219
```

输入 480,256 个采样（48000 Hz），按比例应该输出 480256 × 44100 / 48000 = 441,235.2 个，实际只有 441,219 个。
重采样器内部留着一些采样等下一次调用时再用，输入结束后要再调用 `swr_convert()`（输入传 `NULL`）把它们取出来，示例漏了这一步。

修复后在主循环之后冲刷，多输出 17 个采样，总数 441,236；前 441,219 个采样与修复前逐字节相同。

说明：fork 分支上这个修复是两个提交（`b908217` 和紧跟的 `aac704f`，后者修正了前者在冲刷结束时误报失败的问题），
这里的 0041 是两者合并后的结果，提交到上游时以 patch 文件为准。

### 0042 filter_audio 的字符串缓冲区用了 uint8_t

在 FFmpeg 源码树之外用 `-Wall` 编译（课程仓库的 Makefile 就是这样）：

```console
$ gcc -Wall -c filter_audio.c $(pkg-config --cflags libavfilter libavutil) 2>&1 | grep -c warning
4
```

4 个警告都是 `-Wpointer-sign`：`options_str` 和 `ch_layout` 声明成 `uint8_t` 数组，却传给参数是 `char *` 的函数。
修复后改成 `char` 数组，警告为 0。

### 0043 filter_audio 生成的正弦波不连续

`get_input()` 用 `frame_num + j` 算相位。一个周期正好是 `FRAME_SIZE` 个采样，
第 n 帧最后一个采样的相位是 n + 1023，下一个采样应该是 n + 1024（等同于 n），
但第 n + 1 帧是从 n + 1 开始的，每个帧的边界上跳过了一个采样。

修复后相位只由帧内的位置 `j` 决定（每帧都是整数个周期）。输出的校验值会变，这是预期的。

### 0044 filter_audio 没有使用分配的 MD5 上下文

`process_output()` 先 `av_md5_init(md5)`，然后调用 `av_md5_sum()`。后者不接受上下文参数，它自己另建一个，
所以 `main()` 里分配并一路传下来的上下文实际上没有用。

修复后改用 `av_md5_update()` 加 `av_md5_final()`。修复前后 `filter_audio 1 | md5sum` 的结果相同
（`6c19462e54888af465a35a0d9541ba77`，在 0043 之后测）。

### 0045 filter_audio 滤镜图建立失败时错误信息不完整

把示例里的 `channel_layouts=stereo` 临时改成 `channel_layouts=nosuch` 来触发失败：

```console
$ ./filter_audio 1
Could not initialize the aformat filter.
Unable to init filter graph:$
```

最后一行没有错误原因，也没有换行。其他失败路径都跳到 `fail` 标签，由那里补上 `av_err2str(err)` 和换行，只有这一处直接返回了。
修复后输出 `Unable to init filter graph:Invalid argument`。

### 0046 filter_audio 滤镜图建立失败时泄漏

同样用上面的办法触发失败，valgrind 报告：

```text
2,024 (192 direct, 1,832 indirect) bytes in 1 blocks are definitely lost
```

`init_filter_graph()` 里十几处出错返回都没有释放已经分配的图。修复后图一分配就交给调用者，
由 `main()` 的失败路径统一释放，valgrind 不再报告。

### 0047 filter_audio 结束时不冲刷滤镜图

示例的图里 aformat 要求 44100 Hz，输入是 48000 Hz，libavfilter 会自动插入重采样。
时长给 1 秒时送进去 46 帧共 47,104 个采样，按比例应该输出 47104 × 44100 / 48000 = 43,276.8 个；
在 `process_output()` 里临时加一行打印 `frame->nb_samples` 再求和，实际只有 43,261 个，少了 16 个。
原因和 0041 相同：重采样器里留着的采样没有取出来。示例从来没有告诉 buffersrc 输入结束了。

修复后在主循环之后调用 `av_buffersrc_add_frame(src, NULL)`，再从 buffersink 读到 `AVERROR_EOF` 为止。
输出多了最后一帧 16 个采样，总数 43,277；valgrind 通过。

## 已记录、暂未制作 patch 的观察项

这些问题需要结合对应课次仔细研究之后再决定怎么改，到那一讲时处理。

| 涉及文件 | 观察 | 计划处理的课次 |
| --- | --- | --- |
| mux.c | 和 scale_video.c 一样使用 `sws_getContext()` 加 `sws_scale()` 的旧式写法（scale_video.c 已在 0037 改掉）。mux.c 的改法要和它的编码流程一起考虑 | L15 |
| transcode.c | 编码器没有继承输入的色彩属性，BT.709 的输入转码后 `color_space` 和 `color_range` 变成 unknown | L16 |
| avio_read_callback.c | 只提供读回调、没有 seek 回调。`moov` 在文件末尾的 MP4 读不到完整的流参数（报 `partial file` 和 `unspecified pixel format`） | L18 |
| avio_http_serve_files.c | 用 `uint8_t` 数组当字符串，脱离源码树用 `-Wall` 编译时有 `-Wpointer-sign` 警告（filter_audio.c 的同类问题已在 0042 改掉） | L19 |

用 `-Wdeprecated-declarations` 编译全部能独立编译的示例，没有发现使用已标记废弃的函数。
qsv_decode.c 和 qsv_transcode.c 需要 Intel 的库才能编译，还没有检查。

示例之外顺带看到的：`libavfilter/buffersink.c` 里 `alphamodes` 和 `samplerates` 两个选项的说明文字是从别的选项复制来的
（分别写成了 color ranges 和 sample formats）。不在本目录的范围内，没有制作 patch。
