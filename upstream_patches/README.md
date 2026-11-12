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
| 复核时的上游 master | `539da4e3d7`（2026-11-12），`doc/examples`、`libavformat/aviobuf.c` 与 `a337e8ddaa` 完全相同，`configure` 只有一行（AMF 版本号）不同，不影响 patch |
| 测试环境 | Ubuntu 24.04，gcc 13.3，编译选项见 [L02](../L02_environment/README.md) |

全部 patch 在 `a337e8ddaa` 上编译并运行验证过，在 `539da4e3d7` 上用 `git am` 按顺序应用无冲突（0001 到 0084）。
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
| [0048](0048-doc-examples-decode_filter_-audio-video-check-avcode.patch) | decode_filter_audio.c、decode_filter_video.c | 不符合规范 | 不检查 `avcodec_parameters_to_context()` 的返回值 | L12 |
| [0049](0049-doc-examples-decode_filter_-audio-video-stop-when-fe.patch) | decode_filter_audio.c、decode_filter_video.c | 错误 | 把帧送进滤镜图失败后继续运行，每一帧都报同样的错，最后退出码为 0 | L12 |
| [0050](0050-doc-examples-decode_filter_-audio-video-flush-the-de.patch) | decode_filter_audio.c、decode_filter_video.c | 错误 | 文件读完后不冲刷解码器，125 帧的 H.264 文件只处理了 123 帧 | L12 |
| [0051](0051-doc-examples-decode_filter_audio-fix-the-ffplay-comm.patch) | decode_filter_audio.c | 已废弃 | 用法里提示的 ffplay 命令用了已经删除的 `-ac` 选项 | L12 |
| [0052](0052-doc-examples-decode_filter_audio-check-for-write-err.patch) | decode_filter_audio.c | 错误 | 不检查写标准输出是否成功，写不出去时照常报告成功 | L12 |
| [0053](0053-doc-examples-encode_video-check-for-write-errors.patch) | encode_video.c | 错误 | 不检查 fwrite 和 fclose 的结果，写不出去时照常报告成功 | L13 |
| [0054](0054-doc-examples-encode_video-report-a-failure-to-set-th.patch) | encode_video.c | 不符合规范 | 给所有 H.264 编码器设 `preset` 私有选项却不看结果，编码器没有这个选项时没有任何提示 | L13 |
| [0055](0055-doc-examples-encode_video-fix-a-stale-comment.patch) | encode_video.c | 不符合规范 | 注释还写着“查找 mpeg1video 编码器”，实际按命令行给的名字查找 | L13 |
| [0056](0056-doc-examples-encode_audio-do-not-use-an-uninitialize.patch) | encode_audio.c | 错误 | 编码器给出空的声道布局列表时使用未初始化的指针，`gcc -O2` 有 `-Wmaybe-uninitialized` 警告 | L14 |
| [0057](0057-doc-examples-encode_audio-keep-the-phase-of-the-tone.patch) | encode_audio.c | 错误 | 正弦波的相位用 float 累加，后半段精度不够，440 Hz 的音漂到 438.7 Hz | L14 |
| [0058](0058-doc-examples-encode_audio-use-int16_t-for-signed-16-.patch) | encode_audio.c | 不符合规范 | 有符号的 16 位采样用 `uint16_t` 指针写入 | L14 |
| [0059](0059-doc-examples-encode_audio-fix-the-comment-of-select_.patch) | encode_audio.c | 不符合规范 | `select_sample_rate()` 的注释说选最高的采样率，实际选最接近 44100 的 | L14 |
| [0060](0060-doc-examples-encode_audio-check-for-write-errors.patch) | encode_audio.c | 错误 | 不检查 fwrite 和 fclose 的结果，写不出去时照常报告成功 | L14 |
| [0061](0061-doc-examples-encode_audio-set-the-timestamps-of-the-.patch) | encode_audio.c | 不符合规范 | 送给编码器的帧没有设 pts，编出的包也就没有时间戳 | L14 |
| [0062](0062-doc-examples-mux-check-the-results-of-av_write_trail.patch) | mux.c | 错误 | 不检查 `av_write_trailer()` 和 `avio_closep()` 的结果，MP4 的索引写不进去时照常返回 0，留下一个打不开的文件 | L15 |
| [0063](0063-doc-examples-mux-free-the-options-dictionary.patch) | mux.c | 错误 | 用了 `-flags`、`-fflags` 参数时选项字典没有释放，valgrind 报内存泄漏 | L15 |
| [0064](0064-doc-examples-mux-do-not-generate-one-video-frame-too.patch) | mux.c | 错误 | 结束条件用了 `>`，视频多生成一帧：251 帧、10.04 秒，而不是 250 帧、10 秒 | L15 |
| [0065](0065-doc-examples-mux-pass-the-frame-duration-on-to-the-v.patch) | mux.c | 错误 | 视频帧没有时长，编出的包时长为 0，MP4 的最后一帧落在编辑列表之外，播放时被丢掉 | L15 |
| [0066](0066-doc-examples-mux-use-a-pixel-format-supported-by-the.patch) | mux.c | 错误 | 像素格式写死 yuv420p，转换像素格式的代码永远走不到，gif、apng 这些输出格式打不开编码器 | L15 |
| [0067](0067-doc-examples-mux-make-image-sequence-output-work.patch) | mux.c | 错误 | 用法说明里说文件名带 `%d` 可以输出图片，实际打不开 MJPEG 编码器 | L15 |
| [0068](0068-doc-examples-mux-use-sws_scale_frame.patch) | mux.c | 不符合规范 | 用 swscale.h 标为 legacy 的 `sws_getContext()` 加 `sws_scale()`，而且不检查转换结果 | L15 |
| [0069](0069-doc-examples-mux-compute-the-audio-signal-in-double-.patch) | mux.c | 错误 | 音频信号的相位用 float 累加，最后一秒的音高是 1158.7 Hz，应为 1155 Hz | L15 |
| [0070](0070-doc-examples-mux-document-the-flags-and-fflags-argum.patch) | mux.c | 不符合规范 | 程序接受 `-flags` 和 `-fflags` 两个参数，用法说明里没有提 | L15 |
| [0071](0071-doc-examples-transcode-do-not-crash-when-the-input-c.patch) | transcode.c | 错误 | 输入文件打不开时，收尾代码访问还没有创建的 `ifmt_ctx` 和 `stream_ctx`，段错误 | L16 |
| [0072](0072-doc-examples-transcode-check-the-result-of-av_write_.patch) | transcode.c | 错误 | 不检查 `av_write_trailer()` 和 `avio_closep()` 的结果，MP4 的索引写不进去时照常返回 0 | L16 |
| [0073](0073-doc-examples-transcode-stop-with-an-error-when-decod.patch) | transcode.c | 错误 | 解码失败只打印 `Decoding failed`，接着照常收尾、返回 0，输出被悄悄截断 | L16 |
| [0074](0074-doc-examples-transcode-pass-the-color-properties-on-.patch) | transcode.c | 错误 | 编码器没有继承输入的色彩属性，BT.709 的输入转码后范围、原色、传输特性、矩阵全变成 unknown | L16 |
| [0075](0075-doc-examples-transcode-copy-the-metadata-and-the-dis.patch) | transcode.c | 不满足现有需求 | 不拷贝容器和流的元数据（标题、语言），也不拷贝 default 等标志，播放器选不对默认音轨 | L16 |
| [0076](0076-doc-examples-transcode-terminate-the-Could-not-open-.patch) | transcode.c | 错误 | `Could not open output file` 后面没有换行，下一行日志接在同一行里 | L16 |
| [0077](0077-doc-examples-transcode-make-the-audio-timestamps-con.patch) | transcode.c | 错误 | 输入的时间基很粗（FLV、MKV 是毫秒）时，音频帧的时间戳被取整，间隔是 1014、1015、1058 个采样而不是 1024，输出的音频包时间戳也跟着不均匀 | L16 |
| [0078](0078-doc-examples-transcode_aac-convert-the-packet-timest.patch) | transcode_aac.c | 错误 | 写包前不把时间戳换算到输出流的时间基，只有 MP4 碰巧对：5 秒的输入，MKV、FLV 变成 222 秒，TS 变成 2.5 秒，AVI 变成 5159 秒 | L17 |
| [0079](0079-doc-examples-transcode_aac-do-not-drop-the-frames-th.patch) | transcode_aac.c | 错误 | 读到文件尾就宣布解码结束，带 `AV_CODEC_CAP_DELAY` 的解码器（WMA、APE……）留在里面的最后几帧丢了，输出少一整帧 | L17 |
| [0080](0080-doc-examples-transcode_aac-accept-inputs-that-have-m.patch) | transcode_aac.c | 不满足现有需求 | 输入不止一路流就拒绝（`Expected one audio input stream`），视频文件里的音轨、带封面图的音乐文件都用不了 | L17 |
| [0081](0081-doc-examples-transcode_aac-copy-the-metadata-of-the-.patch) | transcode_aac.c | 不满足现有需求 | 不拷贝容器和流的元数据，转出来的文件没有标题、艺术家 | L17 |
| [0082](0082-doc-examples-avio_read_callback-avoid-comparing-int-.patch) | avio_read_callback.c | 不符合规范 | `FFMIN` 拿 `int` 和 `size_t` 比较，`-Wall -Wextra` 下有 `-Wsign-compare` 警告 | L18 |
| [0083](0083-doc-examples-avio_read_callback-add-a-seek-callback.patch) | avio_read_callback.c | 不满足现有需求 | 只提供读回调、没有 seek 回调：`moov` 在文件尾的 MP4 读不出包，像素格式未知，却不报错；TS、MP2 的时长未知 | L18 |
| [0084](0084-avformat-aviobuf-pad-the-dynamic-buffer-after-the-la.patch) | **libavformat/aviobuf.c**（不是示例） | 错误 | `avio_close_dyn_buf()` 把补的 64 字节零写在当前位置，不一定是数据末尾：flv、aiff、caf、asf 写出来的结果被盖掉 64 字节，长度也少 64 字节 | L18 |

## 怎么使用

应用到 FFmpeg 源码树。各个问题在逻辑上互相独立，但有几个 patch 改的是同一个文件里相邻的行
（0003 到 0005 都改 Makefile.example，0001、0007、0011、0014、0071 到 0077 都改 transcode.c，
0010、0012 到 0015 都改 remux.c，0016 到 0018 都改 decode_video.c，
0006、0016、0019 到 0024 都改 decode_audio.c，
0008、0025 到 0030 都改 demux_decode.c，
0031 到 0033 都改 extract_mvs.c，
0034 到 0037 都改 scale_video.c，
0038 到 0041 都改 resample_audio.c，
0042 到 0047 都改 filter_audio.c，
0007、0008、0048 到 0052 都改 decode_filter_audio.c 或 decode_filter_video.c，
0016、0053 到 0055 都改 encode_video.c，
0010、0016、0056 到 0061 都改 encode_audio.c，
0062 到 0070 都改 mux.c，0078 到 0081 都改 transcode_aac.c，0082、0083 都改 avio_read_callback.c），所以要按编号顺序应用：

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
4. 可以按主题拆成几组提交，例如构建系统（0002 到 0005）一组、transcode（0001、0007、0011、0071 到 0077）一组、
   remux（0012 到 0015）一组、decode_video（0016 到 0018）一组、decode_audio（0006、0019 到 0024）一组、
   demux_decode（0025 到 0030）一组、extract_mvs（0031 到 0033）一组、
   scale_video（0034 到 0037）一组、resample_audio（0038 到 0041）一组、
   filter_audio（0042 到 0047）一组、
   decode_filter_audio 和 decode_filter_video（0048 到 0052）一组，encode_video（0053 到 0055）一组，
   encode_audio（0056 到 0061）一组，mux（0062 到 0070）一组，transcode_aac（0078 到 0081）一组，
   avio_read_callback（0082、0083）一组。**0084 改的是 libavformat，不是示例**，要单独提交，邮件主题和评审的人都不一样（`avformat/aviobuf: ...`），不要和示例的 patch 放在一组。

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

### 0048 decode_filter 两个示例不检查 avcodec_parameters_to_context()

这个函数要为 extradata 和 side data 分配内存，可能失败。两个示例都丢掉了返回值，失败时会带着不完整的参数去打开解码器。
同目录的其他示例（demux_decode.c、transcode.c 等）都检查了。修复后失败时打印错误并返回。

### 0049 decode_filter 两个示例送帧失败后继续运行

用一个采样率中途变化的 ADTS 文件（两段参数不同的 AAC 首尾相接）：

```console
$ doc/examples/decode_filter_audio change.aac > /dev/null; echo "rc=$?"
[in @ ...] Changing audio frame properties on the fly is not supported.
Error while feeding the audio filtergraph
（以上两行对后面的每一帧重复一次，共 23 次）
rc=0
```

`av_buffersrc_add_frame_flags()` 失败后用的是 `break`，只跳出了内层循环，这时 `ret` 还是 `avcodec_receive_frame()` 留下的 0，
主循环接着处理下一个包。修复后保留错误码并跳到清理代码：错误只报一次，然后是 `Error occurred: Invalid argument`，退出码 1。
通知滤镜图输入结束的那一处调用同样丢了错误码，一并改了。

### 0050 decode_filter 两个示例不冲刷解码器

```console
$ doc/examples/decode_filter_video small_360p25.mp4 | grep -c $'\033c'
123
$ ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_read_frames -of csv=p=0 small_360p25.mp4
125
```

（示例每显示一帧先输出一个 `ESC c` 清屏，所以数它的个数就是帧数。）文件读完后示例直接去结束滤镜图，
没有给解码器送空包，解码器里还留着的帧（B 帧重排、帧级多线程造成的延迟）就丢了。300 帧的 1080p 素材同样少 2 帧，MPEG-1 素材少 1 帧。

修复时还发现接收帧的循环写成 `while (ret >= 0)`，而 `ret` 会被里面从 buffersink 取帧的循环改成 `AVERROR(EAGAIN)`，
所以它每个包实际上只转一圈。平时一个包出一帧看不出问题，冲刷时一个空包要出好几帧，只改冲刷仍然会少 1 帧。
修复后循环一直取到 `avcodec_receive_frame()` 自己返回 `AVERROR(EAGAIN)` 或 `AVERROR_EOF`。三个素材的帧数都对了（125、300、125），valgrind 通过。

decode_filter_audio.c 是同样的结构，一并修改。课程的音频素材（AAC、MP2）解码器没有延迟，修复前后输出相同。

### 0051 decode_filter_audio 提示的 ffplay 命令不能用

```console
$ doc/examples/decode_filter_audio small_360p25.mp4 | ffplay -f s16le -ar 8000 -ac 1 -
Failed to set value '1' for option 'ac': Option not found
```

ffplay 已经没有 `-ac` 选项。修复后提示 `ffplay -f s16le -ar 8000 -ch_layout mono -`，实测可以播放。和 0038 是同一类问题。

### 0052 decode_filter_audio 不检查写输出是否成功

```console
$ doc/examples/decode_filter_audio small_360p25.mp4 > /dev/full; echo "rc=$?"
rc=0
```

`print_frame()` 用 `fputc()` 逐字节写，从不看结果。修复后每帧写完检查 `ferror(stdout)`，
出错时打印 `Error while writing the filtered audio` 并以退出码 1 结束。

### 0053 encode_video 不检查写文件是否成功

```console
$ doc/examples/encode_video /dev/full mpeg1video >/dev/null 2>&1; echo $?
0
```

修复后 `fwrite()` 和 `fclose()` 的结果都检查，同一条命令的退出码是 1。正常输出不变：修复前后编出的 `mpeg1video` 文件逐字节相同。

### 0054 encode_video 设置 preset 失败时没有提示

示例对所有 H.264 编码器执行 `av_opt_set(c->priv_data, "preset", "slow", 0)`，不看返回值。
`preset` 是 libx264 等编码器的私有选项，h264_vaapi、h264_v4l2m2m 没有这个选项，这时设置被悄悄丢掉。

修复后：

```console
$ doc/examples/encode_video out.264 h264_vaapi 2>&1 | grep preset
Could not set the preset: Option not found
```

只打印，不退出，因为编码器用默认设置仍然可以工作（这条命令后面仍然会因为 h264_vaapi 不接受 yuv420p 而失败，那是另一回事）。libx264 下没有这行输出。

### 0055 encode_video 的过时注释

`/* find the mpeg1video encoder */` 下面的代码是 `avcodec_find_encoder_by_name(codec_name)`，编码器由命令行第二个参数决定。只改注释。

### 0056 encode_audio 可能使用未初始化的指针

```console
$ gcc -O2 -Wall -I. -c doc/examples/encode_audio.c -o /dev/null
doc/examples/encode_audio.c:113:12: warning: 'best_ch_layout' may be used uninitialized [-Wmaybe-uninitialized]
```

`select_channel_layout()` 只在遍历列表的循环里给 `best_ch_layout` 赋值。编码器返回空列表时循环一次也不执行，
未初始化的指针被交给 `av_channel_layout_copy()`。修复后空列表和“没有列表”一样回退到立体声，警告消失，输出不变。

### 0057 encode_audio 生成的音高会漂移

示例生成 440 Hz 的正弦波，相位 `t` 是 `float`，每个采样加一次 `tincr`（0.0627），一共加 230,400 次。
后半段 `t` 超过 14000，float 在这个量级的精度约为 0.001，加上去的增量不再准确。
把输出解码后按秒数过零点（`ffmpeg -ss N -t 1 -i out.mp2 -ac 1 -f s16le -` 接一个数过零点的小程序）：

| 位置 | 修复前 | 修复后 |
| --- | --- | --- |
| 0.2 秒起的 1 秒 | 440.30 Hz | 440.00 Hz |
| 2 秒起的 1 秒 | 438.67 Hz | 440.00 Hz |
| 4 秒起的 1 秒 | 439.03 Hz | 439.82 Hz |

修复是把 `t` 和 `tincr` 改成 `double`。（修复后最后一行的 439.82 是测量窗口落在文件末尾造成的误差。）
和 filter_audio 的 0043 是同一类问题：示例生成的测试信号本身不准。

### 0058 encode_audio 用 uint16_t 存有符号采样

帧的格式是 `AV_SAMPLE_FMT_S16`，生成的值一半是负数，却通过 `uint16_t *` 写入。靠隐式转换结果碰巧正确。
改成 `int16_t` 后输出逐字节相同。

### 0059 encode_audio 的过时注释

`select_sample_rate()` 上面的注释是 `just pick the highest supported samplerate`，代码选的是最接近 44100 Hz 的采样率。只改注释。

### 0060 encode_audio 不检查写文件是否成功

```console
$ doc/examples/encode_audio /dev/full; echo $?
0
```

修复后输出 `Error writing the encoded data`，退出码 1。和 0053 是同一类问题。

### 0061 encode_audio 没有给帧设时间戳

帧的 `pts` 一直是 `AV_NOPTS_VALUE`，编码器返回的包也没有时间戳。示例把包直接写成裸流，看不出问题；
照着这个示例写的程序如果把包交给复用器，就会写出没有时间戳的流。
修复后 `frame->pts` 设为已经送进去的采样数（音频编码器的 `time_base` 默认是 `1 / sample_rate`）。输出文件不变。

### 0062 mux 不检查写文件尾和关闭文件的结果

`av_write_trailer()` 和 `avio_closep()` 的返回值都被丢掉了。MP4 的索引（`moov`）是在写文件尾时才写的，
这一步失败，文件就打不开。复现（把输出文件的大小限制在 398 KB，数据写得进去，索引写不进去）：

```bash
( trap '' XFSZ; ulimit -f 398; ./mux lim.mp4 >/dev/null 2>&1; echo "退出码 $?" )
ffprobe -v error lim.mp4
```

修复前退出码是 0，ffprobe 报 `Invalid data found when processing input`。
修复后打印 `Error writing the trailer: File too large`，退出码 1。

### 0063 mux 没有释放选项字典

`-flags`、`-fflags` 的值放进了一个 `AVDictionary`，用完没有 `av_dict_free()`。
`valgrind --leak-check=full ./mux v.mp4 -flags +bitexact -fflags +bitexact` 报 `definitely lost`。修复后没有泄漏。

### 0064 mux 多生成一帧视频

判断“是否到了 10 秒”用的是 `av_compare_ts(next_pts, …, STREAM_DURATION, …) > 0`，
时间戳正好等于 10 秒的那一帧也生成了。修复前视频是 251 帧、10.04 秒，改成 `>= 0` 之后是 250 帧、10 秒。
音频每帧 1024 个采样，10 秒不是整数帧，前后都是 431 帧。

### 0065 mux 的视频包没有时长

和 0011（transcode）是同一个问题：帧没有设 `duration`，也没有设 `AV_CODEC_FLAG_FRAME_DURATION`，
视频编码器输出的包时长是 0。MP4 复用器需要最后一个包的时长才能算出流的总时长，拿到 0 之后，
最后一帧落在编辑列表之外：

```bash
./mux a.mp4 >/dev/null 2>&1
ffprobe -v error -count_frames -select_streams v -show_entries stream=nb_frames,nb_read_frames,duration -of default=nw=1 a.mp4
```

（在 0064 之后）修复前 `nb_frames=250`、`nb_read_frames=249`、`duration=9.960000`：文件里有 250 帧，能读出来的只有 249 帧。
修复后三个数是 250、250、10.000000。

### 0066 mux 的像素格式写死为 yuv420p

`add_stream()` 里 `c->pix_fmt = STREAM_PIX_FMT`，后面 `get_video_frame()` 里“像素格式不是 yuv420p 就先转换”的分支永远走不到。
默认编码器不接受 yuv420p 的输出格式直接失败：`./mux a.gif` 报
`Specified pixel format yuv420p is not supported by the gif encoder` 和 `Could not open video codec`。
修复后先查询编码器支持的像素格式，不支持 yuv420p 时用它的第一种格式（和音频采样率的选法一致），
gif、apng 都能输出 250 帧。原来能用的格式输出不变。

### 0067 mux 输出图片序列失败

用法说明写着 `Raw images can also be output by using '%d' in the filename`，
实际 `./mux 'img/x%03d.jpg'` 报 `Non full-range YUV is non-standard` 和 `Could not open video codec`：
image2 复用器的默认编码器是 MJPEG，它只接受全范围的 YUV。
修复后给 MJPEG 设 `color_range = AVCOL_RANGE_JPEG`（示例生成的画面本来就用满了 0 到 255），能写出 250 张 JPEG。

### 0068 mux 使用旧式的 libswscale 接口

和 0037（scale_video）一样，把 `sws_getContext()` 加 `sws_scale()` 换成 `sws_alloc_context()` 加 `sws_scale_frame()`，
同时检查转换的返回值。这段代码在 0066 之后才真正被执行；修改前后 gif、apng 的输出逐字节相同。

### 0069 mux 的音频信号用 float 累加相位

和 0057（encode_audio）同类。相位 `t` 和它的增量 `tincr` 是 float，累加 44 万次之后误差明显：
按注释，音高每秒升高 110 Hz，最后一秒（第 9 到 10 秒）的平均频率应为 1155 Hz，实测 1158.7 Hz。
改成 double 之后是 1154.9 Hz。测法：输出 WAV，取第 9 到 10 秒数过零点。

### 0070 mux 的用法说明没有提 -flags 和 -fflags

程序会把这两个参数的值交给编码器和复用器（例如 `-flags +bitexact -fflags +bitexact` 让输出不带版本信息、可以逐字节比较），
但用法说明里只写了 `output_file`。修复后用法说明里加上了这两个参数。

### 0071 transcode 输入打不开时崩溃

`avformat_open_input()` 失败时 `ifmt_ctx` 是 NULL，`stream_ctx` 也还没有分配，但 `main()` 末尾的清理代码
无条件地按 `ifmt_ctx->nb_streams` 循环、访问 `stream_ctx[i]`。输入文件不存在或打不开就直接段错误：

```console
$ doc/examples/transcode /nonexistent.mp4 /tmp/out.mp4
Cannot open input file
Segmentation fault
```

`avformat_find_stream_info()` 失败（例如输入是个空文件）时同样会走到这里。
修复后循环前先判断两个指针，打印 `Error occurred: No such file or directory`，返回 1。

### 0072 transcode 不检查写文件尾的结果

和 0062（mux）是同一个问题：`av_write_trailer()` 写 MP4 的 moov 索引，`avio_closep()` 把缓冲区里剩下的数据写出去，
两个返回值都被丢掉。把输出文件的大小限制在刚好放得下数据、放不下索引的位置：

```console
$ ( trap '' XFSZ; ulimit -f 520; doc/examples/transcode $S/small_360p25.mp4 /tmp/lim.mp4 >/dev/null 2>&1; echo "exit $?" )
exit 0
$ ffprobe -v error /tmp/lim.mp4
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] moov atom not found
```

修复后最后一行日志是 `Error occurred: File too large`，返回 1。（bash 的 `ulimit -f` 以 1 KB 为单位，
具体多大能卡出这个效果取决于编码器的输出大小，上面的数在课程素材上可以重现。）

### 0073 transcode 解码失败后仍然返回成功

读循环里 `avcodec_send_packet()` 失败时只打印 `Decoding failed` 然后 `break`，后面的冲刷、写文件尾、返回 0 照常执行。
把一个 ADTS 文件的两处各改写 300 个字节再转码：

```console
$ ffmpeg -v error -i $S/small_360p25.mp4 -vn -c:a copy /tmp/a.aac
$ python3 - <<'EOF'
import random
random.seed(5)
b = bytearray(open('/tmp/a.aac', 'rb').read())
for off in (20000, 40000):
    for i in range(300):
        b[off + i] = random.randrange(256)
open('/tmp/bad.aac', 'wb').write(b)
EOF
$ doc/examples/transcode /tmp/bad.aac /tmp/bad.m4a >/dev/null 2>&1; echo "exit $?"
exit 0
```

输出文件被截断，调用方却以为成功。修复后改成 `goto end`，和读循环里其他出错的路径一致，
最后打印 `Error occurred: Invalid data found when processing input`，返回 1。

### 0074 transcode 没有把色彩属性交给编码器

编码器只拿到了解码器的宽高和像素宽高比，没有拿到色彩范围、原色、传输特性和矩阵系数。
用 BT.709、limited range 的输入转码，输出里这四项全部是 unknown：

```console
$ ffmpeg -v error -f lavfi -i testsrc2=size=320x180:rate=25 -t 1 -c:v libx264 -pix_fmt yuv420p \
    -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range tv /tmp/bt709.mp4
$ doc/examples/transcode /tmp/bt709.mp4 /tmp/out.mp4 >/dev/null 2>&1
$ ffprobe -v error -select_streams v -show_entries stream=color_range,color_space,color_transfer,color_primaries \
    -of default=nw=1 /tmp/out.mp4
color_range=unknown
color_space=unknown
color_transfer=unknown
color_primaries=unknown
```

这些值已经通过 0007 到了滤镜图的入口，只是没有传给编码器，libx264 写进码流的值来自编码器上下文。
没有这几项，播放器要靠猜：高清视频当成 BT.601 或者 limited range 当成 full range，颜色都会变。
修复后输出是 `tv`、`bt709`、`bt709`、`bt709`，和输入一致。同时复制了 `chroma_sample_location`。

### 0075 transcode 丢掉元数据和流标志

输出里没有输入的标题、各流的语言和 handler 名，也没有 default 等标志，播放器无法选择默认的音轨或字幕：

```console
$ ffmpeg -v error -i $S/small_360p25.mp4 -metadata title="My Title" -metadata:s:a:0 language=eng \
    -metadata:s:v:0 language=chi -disposition:a:0 default -c copy /tmp/meta.mkv
$ doc/examples/transcode /tmp/meta.mkv /tmp/out.mkv >/dev/null 2>&1
$ ffprobe -v error -show_entries format_tags=title:stream=index,codec_type:stream_tags=language:stream_disposition=default \
    -of compact /tmp/out.mkv
stream|index=0|codec_type=video|disposition:default=0
stream|index=1|codec_type=audio|disposition:default=0
format
```

输入文件的同一条命令输出是 `video|default=1|language=chi`、`audio|default=1|language=eng`、`format|title=My Title`。
修复后用 `av_dict_copy()` 拷贝容器和每路流的元数据，再复制 `disposition`，做法和 remux.c 一致，
输出与输入完全相同。

### 0076 transcode 的错误信息没有换行

`Could not open output file '%s'` 的格式串末尾缺 `\n`，后面的日志接在同一行：

```console
$ doc/examples/transcode $S/small_360p25.mp4 /nonexistent/x.mp4 2>&1 | grep 'Could not open'
Could not open output file '/nonexistent/x.mp4'Qavg: nan  Tr: 0.0%  TNS(L): 0.0%  TNS(S): 0.0%  M/S: 0.0%  I/S: 0.0%  PNS: 0.0%
```

加上换行之后，编码器的日志另起一行，带上 `[aac @ ...]` 前缀。

### 0077 transcode 的音频时间戳不连续

音频帧是用输入流的时间基交给滤镜图的。FLV、MKV 的时间基是 1/1000，包的时间戳被取整到毫秒；
44100 Hz 下 1024 个采样是 23.22 毫秒，所以相邻两帧的 pts 换到 1/44100 以后相差 1014、1015、1058……而不是 1024，
输出文件里的音频包也是这样。用 L16 的 FLV 样本验证：

```console
$ ffmpeg -v error -y -i $S/small_360p25.mp4 -c copy /tmp/in.flv
$ doc/examples/transcode /tmp/in.flv /tmp/out.mp4 >/dev/null 2>&1
$ ffprobe -v error -select_streams a -show_entries packet=pts -of csv=p=0 /tmp/out.mp4 |
    awk 'NR>1{d=$1-p; c[d]++} {p=$1} END{for(k in c) printf "%s×%s ", k, c[k]}'
1014×120 1015×49 1058×27 1059×20 1024×1
```

（样本是 `samples/small_360p25.flv`，上面的输出是对它跑的结果。）
修复后，在解码出帧时用 `av_rescale_delta()` 把时间戳换成 1/采样率：它利用“每帧正好 `nb_samples` 个采样”把取整的误差接平，
真的出现断档时自动退回普通的换算；滤镜图入口也改用 1/采样率做时间基。ffmpeg 命令行是同样的做法。
修复后相邻帧都相差 1024（输入本身有断档的两处除外，命令行的结果也是这样）；
时间基本来就是 1/采样率的输入（MP4 里的音频）不受影响。

### 0078 transcode_aac 写包前不换算时间戳

示例让编码器和输出流都用 1/采样率做时间基，可是复用器在 `avformat_write_header()` 里可以改流的时间基，
包写出去之前没有用 `av_packet_rescale_ts()` 换算。MP4 的复用器保持 1/采样率，所以输出碰巧是对的；
示例开头的说明却写着“按输出文件的扩展名支持其他格式”，换成别的容器，时间戳就全错了：

```console
$ for e in m4a mkv flv ts avi; do
    doc/examples/transcode_aac $S/audio_44k_stereo.mp2 /tmp/out.$e >/dev/null 2>&1
    echo "$e $(ffprobe -v error -show_entries format=duration -of csv=p=0 /tmp/out.$e)"
  done
m4a 5.015510
mkv 222.208000
flv 222.208000
ts 2.480811
avi 5159.659683
```

输入是 5 秒的音频。复用器选的时间基：MKV、FLV 是 1/1000，TS 是 1/90000，AVI 是 256/11025；AVI 的文件有 5.4 MB，里面的音频只有 62 KB。
修复后显式设置编码器的时间基，写包前换算，并设置包的 `stream_index`。MP4、MKV、FLV、AVI 的输出的时间戳与
`ffmpeg -i in -c:a aac -b:a 96k -ac 2 out.<ext>` 的完全相同（用 `cmp` 比较整个文件，MP4、FLV、AVI 逐字节相同，
MKV 只差命令行多写的流级别 `ENCODER` 标签；TS 的音频包相同，但文件比命令行的大三成，因为命令行默认把 `AVFormatContext.max_delay`
设成 0.7 秒，让 MPEG-TS 复用器把靠得近的几个音频帧装进同一个 PES 包，示例没有设，把它设成 0.7 秒以后也逐字节相同）。

### 0079 transcode_aac 丢掉解码器里剩下的帧

`decode_audio_frame()` 一读到文件尾（`av_read_frame()` 返回 `AVERROR_EOF`）就把 `finished` 置 1，
`read_decode_convert_and_store()` 看到 `finished` 直接返回，既没有使用冲刷刚刚吐出来的那一帧，之后也没有人再去取解码器里的帧。
有的解码器（带 `AV_CODEC_CAP_DELAY` 标志的，例如 wmav2、ape，或者开了帧线程的）收到冲刷信号才把缓存的帧吐出来，输出的结尾就丢了：

```console
$ ffmpeg -v error -y -i $S/audio_44k_stereo.mp2 -c:a wmav2 /tmp/in.wma
$ ffmpeg -v error -i /tmp/in.wma -f s16le -ac 2 - | wc -c
876544
$ doc/examples/transcode_aac /tmp/in.wma /tmp/out.m4a >/dev/null 2>&1
$ ffmpeg -v error -i /tmp/out.m4a -f s16le -ac 2 - | wc -c
868352
```

少了 8192 个字节，正好是 2048 个采样的一帧。修复后只在解码器报告 `AVERROR_EOF` 时才置 `finished`；
文件尾以后每次调用都送一次空包，已经冲刷过的解码器对它回答 `AVERROR_EOF`，这不是错误。修复后输出是 876544 字节，和命令行的一致。
（用 MP2、FLAC、Vorbis、Opus、AC-3 的输入试过，修复前后输出没有区别：这几个解码器冲刷时没有帧留在里面。）

### 0080 transcode_aac 不接受有多路流的输入

示例要求输入里只有一路流：

```console
$ doc/examples/transcode_aac $S/small_360p25.mp4 /tmp/out.m4a
Expected one audio input stream, but found 2
```

这拒绝了两种最常见的输入：要取音轨的视频文件，和带封面图的音乐文件（MP3、FLAC、M4A 里的封面是第二路流，一路“视频”）。
修复后用 `av_find_best_stream()` 选出最好的一路音频，别的流的包读到就丢掉；输入里没有音频时明确报告。
这两种输入的输出都和 `ffmpeg -i in -vn -c:a aac -b:a 96k -ac 2 out.m4a` 逐字节相同。

### 0081 transcode_aac 丢掉元数据

输出里没有输入的标题、艺术家，也没有流的元数据和标志（和 0013 的 remux、0075 的 transcode 是同一类问题）：

```console
$ ffmpeg -v error -y -i $S/audio_44k_stereo.mp2 -metadata title="My Song" -metadata artist="Me" /tmp/tag.flac
$ ffprobe -v error -show_entries format_tags=title,artist -of compact /tmp/tag.flac
format|tag:artist=Me|tag:title=My Song
$ doc/examples/transcode_aac /tmp/tag.flac /tmp/out.m4a >/dev/null 2>&1
$ ffprobe -v error -show_entries format_tags=title,artist -of compact /tmp/out.m4a
format
```

标题和艺术家是音乐库里一个文件的身份，转码以后全丢了。修复后在写文件头之前用 `av_dict_copy()` 拷贝容器和这一路流的元数据，
再复制流的 `disposition`；输出与 `ffmpeg -i in -c:a aac -b:a 96k -ac 2 out.m4a` 逐字节相同（FLAC 和 Matroska 的输入试过）。

### 0082 avio_read_callback 的符号比较警告

```console
$ gcc -std=c11 -Wall -Wextra -c doc/examples/avio_read_callback.c -o /dev/null $(pkg-config --cflags libavformat libavcodec libavutil)
warning: comparison of integer expressions of different signedness: 'int' and 'size_t' {aka 'long unsigned int'} [-Wsign-compare]
```

`read_packet()` 里的 `FFMIN(buf_size, bd->size)`。回调的 `buf_size` 不会是负数，转成 `size_t` 不改变行为。

### 0083 avio_read_callback 没有 seek 回调

`avio_alloc_context()` 的最后一个参数是 `NULL`，得到的 `AVIOContext` 不能 seek，而示例不报错：

```console
$ doc/examples/avio_read_callback $S/small_360p25.mp4 2>&1 >/dev/null | head -3
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] stream 0, offset 0x30: partial file
[mov,mp4,m4a,3gp,3g2,mj2 @ 0x...] Could not find codec parameters for stream 0 (Video: h264 (avc1 / 0x31637661), none, 640x360, 770 kb/s): unspecified pixel format
Consider increasing the value for the 'analyzeduration' (0) and 'probesize' (5000000) options
$ doc/examples/avio_read_callback $S/small_360p25.mp4 2>/dev/null | wc -l
138
$ doc/examples/avio_read_callback $S/small_360p25.mp4 >/dev/null 2>&1; echo $?
0
```

`ffmpeg` 默认写出的 MP4 把 `moov` 放在文件尾，读它要么 seek 到末尾，要么把整个 `mdat` 读过去。
没有 seek 回调时只能后者：138 次读回调读完整个 563 kB 的文件，而 `mdat` 读过去以后回不来，视频流的像素格式是 `none`，视频包一个也读不出来；
程序却照常返回 0，只有 `av_dump_format()` 的输出里看得出来。
修复后加一个处理 `SEEK_SET`、`SEEK_CUR`、`SEEK_END` 和 `AVSEEK_SIZE` 的 seek 回调：6 次读回调，`yuv420p`，码率 901 kb/s。
MPEG-TS（时长 N/A，修复后 5.039 秒）和 MP2（时长 N/A，修复后 5.016 秒）也一样。

提交信息里有一句不准确：“MPEG-TS and MP2 files lose their duration and bitrate”。实测 TS 丢时长和码率，**MP2 只丢时长，码率还在**（128 kb/s，来自帧头）。
提交之前请改成 “MPEG-TS files lose their duration and bitrate, and MP2 files their duration”。

### 0084 avio_close_dyn_buf 把补的零写在了数据中间

这是 **libavformat 的问题**，不是示例的问题：L18 用 `avio_open_dyn_buf()` 把输出写进内存，对比直接写文件时发现的。
`avio_close_dyn_buf()` 在数据后面补 `AV_INPUT_BUFFER_PADDING_SIZE`（64）字节的零，补在**当前写位置**上，再返回“总长度减 64”。
写位置在数据末尾时没有问题。但有的复用器在 `write_trailer()` 里回头改文件头（时长、文件大小），改完就停在那里，不 seek 回末尾，
这时 64 个零盖在了数据中间，返回的长度又少了 64，文件尾被截掉。

课程里的 `L18_avio_callbacks/avio_mem_out` 用同一个输入分别写文件（`mode=file`）和写 `avio_open_dyn_buf()`（`mode=dyn`），逐字节比较（`bitexact=1`）：

```console
$ L18_avio_callbacks/build/avio_mem_out $S/small_360p25.mp4 flv /tmp/file.flv mode=file bitexact=1
$ L18_avio_callbacks/build/avio_mem_out $S/small_360p25.mp4 flv /tmp/dyn.flv  mode=dyn  bitexact=1
$ stat -c %s /tmp/file.flv /tmp/dyn.flv
564393
564329
$ cmp -l /tmp/file.flv /tmp/dyn.flv 2>/dev/null | wc -l
37
$ ffprobe -v error -count_packets -show_entries stream=nb_read_packets -of csv=p=0 /tmp/dyn.flv 2>/dev/null | tr '\n' ' '
N/A N/A
```

受影响的格式（用 1 秒的音频或本讲的视频素材逐个试过）：

| 格式 | 写文件 | `avio_open_dyn_buf()` |
| --- | --- | --- |
| flv | 119467 字节（1 秒的视频素材） | 119403 字节 |
| aiff | 176454 字节 | 176390 字节 |
| caf | 176492 字节 | 176428 字节 |
| asf | 19644 字节 | 19580 字节 |
| mp4、matroska、mpegts、wav、flac | 相同 | 相同，没有问题 |

在 `539da4e3d7`（上游 master）上打 0084 重新编译，flv、aiff、caf、asf 的两种输出都逐字节相同，mp4、matroska、wav 的输出不变；`valgrind` 没有报错。
修复的办法是：先 `avio_flush()`，再把 `DynBuffer` 的写位置设到数据末尾，然后才补零。
课程用的是没有打这个补丁的 libavformat，`avio_mem_out` 的 `dynend=1` 在关闭之前先把位置移到末尾（`avio_get_dyn_buf()` 得到长度，再 `avio_seek(pb, size, SEEK_SET)`），可以绕开。
没有为它写 FATE 测试：`libavformat/tests` 里没有动态缓冲区的测试，提交时评审可能会要求加一个。

## 已记录、暂未制作 patch 的观察项

这些问题需要结合对应课次仔细研究之后再决定怎么改，到那一讲时处理。

| 涉及文件 | 观察 | 计划处理的课次 |
| --- | --- | --- |
| avio_http_serve_files.c | 用 `uint8_t` 数组当字符串，脱离源码树用 `-Wall` 编译时有 `-Wpointer-sign` 警告（filter_audio.c 的同类问题已在 0042 改掉） | L19 |

transcode_aac.c 在 L17 研究过，除了 0078 到 0081，下面几处也看到了，但各自要改的地方比较大或者属于风格问题，暂时没有制作 patch：

- 输出采样率固定等于输入，`init_resampler()` 里用 `av_assert0()` 写明“暂不支持采样率转换”。输入的采样率 AAC 不支持（192 kHz、4 kHz……）时，
  `avcodec_open2()` 失败。要支持的话，要换成 `swr_convert_frame()` 或者估算输出采样数、冲刷转换器，是一个功能的增加，不是修一个错误。
- 先 `avio_open()` 创建输出文件，再配编码器；后面任何一步失败，输出文件已经被创建、清空，留下 0 字节的垃圾。可以把创建文件挪到写文件头之前。
- `add_samples_to_fifo()` 里先 `av_audio_fifo_realloc()` 再 `av_audio_fifo_write()`：`av_audio_fifo_write()` 空间不够时自己会扩，前一句是多余的。
- `avctx->pkt_timebase` 设在 `avcodec_open2()` 之后。目前的解码器是每解一帧才读它，碰巧没有问题，但接口没有这样保证。

用 `-Wdeprecated-declarations` 编译全部能独立编译的示例，没有发现使用已标记废弃的函数。
qsv_decode.c 和 qsv_transcode.c 需要 Intel 的库才能编译，还没有检查。

示例之外顺带看到的：`libavfilter/buffersink.c` 里 `alphamodes` 和 `samplerates` 两个选项的说明文字是从别的选项复制来的
（分别写成了 color ranges 和 sample formats）。不在本目录的范围内，没有制作 patch。
