/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2012 Clément Bœsch
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file audio decoding and filtering usage example
 * @example decode_filter_audio.c
 *
 * Demux, decode and filter audio input file, generate a raw audio
 * file to be played with ffplay.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/decode_filter_audio.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它解码输入文件的音频，送进用一个字符串描述的滤镜图（重采样到 8000 Hz，转成 s16 单声道），
 * 把结果作为裸 PCM 写到标准输出，可以用管道交给 ffplay 播放。
 * 结构和 decode_filter_video.c 几乎一样，注释只写和它不同的地方。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0008、0048 到 0052），
 * 下面在出问题的位置都做了标注。
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

/*
 * 课程注释：滤镜图的描述。逗号分开两个滤镜；aformat 有两个参数，参数之间用冒号分开。
 * aresample=8000 明确地做重采样；即使不写它，只写 aformat 并加上 sample_rates=8000，格式协商也会自动插入一个（L11）。
 */
static const char *filter_descr = "aresample=8000,aformat=sample_fmts=s16:channel_layouts=mono";
/* 课程注释：提示用户怎么播放。ffplay 已经没有 -ac 选项，要写成 -ch_layout mono（patch 0051）。 */
static const char *player       = "ffplay -f s16le -ar 8000 -ac 1 -";

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int audio_stream_index = -1;

static int open_input_file(const char *filename)
{
    const AVCodec *dec;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the audio stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find an audio stream in the input file\n");
        return ret;
    }
    audio_stream_index = ret;

    /* create decoding context */
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);
    /* 课程注释：没有检查返回值（patch 0048），也没有设置 dec_ctx->pkt_timebase（patch 0008，L07 讲过它的作用）。 */
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[audio_stream_index]->codecpar);

    /* init the audio decoder */
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open audio decoder\n");
        return ret;
    }

    return 0;
}

static int init_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    static const int out_sample_rate = 8000;
    const AVFilterLink *outlink;
    AVRational time_base = fmt_ctx->streams[audio_stream_index]->time_base;

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer audio source: the decoded frames from the decoder will be inserted here. */
    /* 课程注释：文件没写声道布局时补上默认布局，L10 讲过。入口滤镜 abuffer 必须知道声道布局。 */
    if (dec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&dec_ctx->ch_layout, dec_ctx->ch_layout.nb_channels);
    ret = snprintf(args, sizeof(args),
            "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=",
             time_base.num, time_base.den, dec_ctx->sample_rate,
             av_get_sample_fmt_name(dec_ctx->sample_fmt));
    av_channel_layout_describe(&dec_ctx->ch_layout, args + ret, sizeof(args) - ret);
    ret = avfilter_graph_create_filter(&buffersrc_ctx, abuffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "out");
    if (!buffersink_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 课程注释：限定出口的格式：采样格式 s16、声道布局 mono、采样率 8000。
     * 前两项用字符串设置；采样率是整数列表，用 av_opt_set_array() 设置，这里列表里只有一个元素。
     * 这三项其实和 filter_descr 里的要求重复了，示例是为了演示出口滤镜的选项怎么设。
     */
    ret = av_opt_set(buffersink_ctx, "sample_formats", "s16",
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
        goto end;
    }

    ret = av_opt_set(buffersink_ctx, "channel_layouts", "mono",
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
        goto end;
    }

    ret = av_opt_set_array(buffersink_ctx, "samplerates", AV_OPT_SEARCH_CHILDREN,
                           0, 1, AV_OPT_TYPE_INT, &out_sample_rate);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
        goto end;
    }

    ret = avfilter_init_dict(buffersink_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot initialize audio buffer sink\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                        &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Print summary of the sink buffer
     * Note: args buffer is reused to store channel layout string */
    /*
     * 课程注释：配置之后看出口那条连接上实际协商出的格式。L11 的 volume_file.c 用的是
     * av_buffersink_get_sample_rate() 这一组函数，是更规范的取法。
     */
    outlink = buffersink_ctx->inputs[0];
    av_channel_layout_describe(&outlink->ch_layout, args, sizeof(args));
    av_log(NULL, AV_LOG_INFO, "Output: srate:%dHz fmt:%s chlayout:%s\n",
           (int)outlink->sample_rate,
           (char *)av_x_if_null(av_get_sample_fmt_name(outlink->format), "?"),
           args);

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

/* 课程注释：把一帧 s16 采样按小端字节序逐字节写到标准输出。没有检查写入是否成功（patch 0052）。 */
static void print_frame(const AVFrame *frame)
{
    const int n = frame->nb_samples * frame->ch_layout.nb_channels;
    const uint16_t *p     = (uint16_t*)frame->data[0];
    const uint16_t *p_end = p + n;

    while (p < p_end) {
        fputc(*p    & 0xff, stdout);
        fputc(*p>>8 & 0xff, stdout);
        p++;
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int ret;
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    AVFrame *filt_frame = av_frame_alloc();

    if (!packet || !frame || !filt_frame) {
        fprintf(stderr, "Could not allocate frame or packet\n");
        exit(1);
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: %s file | %s\n", argv[0], player);
        exit(1);
    }

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = init_filters(filter_descr)) < 0)
        goto end;

    /* read all packets */
    while (1) {
        /* 课程注释：和 decode_filter_video.c 一样，读到文件结尾后没有冲刷解码器（patch 0050）。 */
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
            break;

        if (packet->stream_index == audio_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    goto end;
                }

                if (ret >= 0) {
                    /* push the audio data from decoded frame into the filtergraph */
                    /* 课程注释：失败时只 break，错误被忽略（patch 0049）。用 L10 做的 out/change.aac 可以看到：同一个错误重复报了 23 次，退出码却是 0。 */
                    if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                        av_log(NULL, AV_LOG_ERROR, "Error while feeding the audio filtergraph\n");
                        break;
                    }

                    /* pull filtered audio from the filtergraph */
                    while (1) {
                        ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                            break;
                        if (ret < 0)
                            goto end;
                        print_frame(filt_frame);
                        av_frame_unref(filt_frame);
                    }
                    av_frame_unref(frame);
                }
            }
        }
        av_packet_unref(packet);
    }
    if (ret == AVERROR_EOF) {
        /* signal EOF to the filtergraph */
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, NULL, 0) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while closing the filtergraph\n");
            goto end;
        }

        /* pull remaining frames from the filtergraph */
        while (1) {
            ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                goto end;
            print_frame(filt_frame);
            av_frame_unref(filt_frame);
        }
    }

end:
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        exit(1);
    }

    exit(0);
}
