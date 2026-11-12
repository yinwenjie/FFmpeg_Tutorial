/*
 * L11 滤镜图基础：解码一个文件里的音频，送进手工搭建的滤镜图调整音量，结果写成 16 位 WAV 文件。
 *
 * 用法：volume_file <输入文件> <输出.wav> <音量> [采样率]
 *
 * 例子：
 *   volume_file samples/small_360p25.mp4 out/half.wav 0.5           音量减半
 *   volume_file samples/small_360p25.mp4 out/quiet.wav -6dB         降低 6 分贝
 *   volume_file samples/small_360p25.mp4 out/v48.wav 0.5 48000      音量减半，同时转成 48000 Hz
 *
 * 滤镜图是：
 *   abuffer（入口）-> volume -> aformat（要求 s16，可选地要求采样率）-> abuffersink（出口）
 *
 * 和官方示例 filter_audio.c 的区别：
 *   1. 输入来自解码器，入口滤镜的参数从第一帧上取，所以图是在拿到第一帧之后才建的；
 *   2. 建滤镜用 avfilter_graph_create_filter()，一个函数完成“建实例、给参数、初始化”；
 *   3. 输入结束后冲刷滤镜图，一个采样也不少；
 *   4. 出错时释放所有资源。
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

#define WAV_HEADER_SIZE 44

typedef struct Chain {
    AVFilterGraph   *graph;
    AVFilterContext *src;           /* 入口 abuffer */
    AVFilterContext *sink;          /* 出口 abuffersink */
    AVFrame         *out;           /* 从出口取帧用，反复使用 */
    FILE            *file;

    const char      *volume;        /* 命令行给的音量，原样交给 volume 滤镜 */
    int              want_rate;     /* 命令行给的采样率，0 表示不变 */

    /* 出口的参数，图配置好之后才知道 */
    int              out_rate;
    int              out_channels;

    int64_t          nb_in;         /* 送进图的采样数 */
    int64_t          nb_out;        /* 从图里取出的采样数 */
    int64_t          data_bytes;
} Chain;

/* WAV 文件头，和 L06 的 decode_wav.c 相同。 */
static void put_le16(uint8_t *p, unsigned v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8 & 0xff;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    put_le16(p, v & 0xffff);
    put_le16(p + 2, v >> 16);
}

static int write_wav_header(FILE *f, int sample_rate, int channels, uint32_t data_bytes)
{
    uint8_t h[WAV_HEADER_SIZE];
    const int block_align = channels * 2;

    memcpy(h, "RIFF", 4);
    put_le32(h + 4, WAV_HEADER_SIZE - 8 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, channels);
    put_le32(h + 24, sample_rate);
    put_le32(h + 28, sample_rate * block_align);
    put_le16(h + 32, block_align);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
    return fwrite(h, 1, sizeof(h), f) == sizeof(h) ? 0 : AVERROR(EIO);
}

/*
 * 建一个滤镜实例并初始化。avfilter_graph_create_filter() 相当于官方示例里的
 * avfilter_graph_alloc_filter() 加 avfilter_init_str()。
 */
static int create_filter(AVFilterContext **ctx, AVFilterGraph *graph, const char *filter_name,
                         const char *instance_name, const char *args)
{
    const AVFilter *filter = avfilter_get_by_name(filter_name);
    int ret;

    if (!filter) {
        fprintf(stderr, "这个 FFmpeg 里没有 %s 滤镜\n", filter_name);
        return AVERROR_FILTER_NOT_FOUND;
    }
    ret = avfilter_graph_create_filter(ctx, filter, instance_name, args, NULL, graph);
    if (ret < 0)
        fprintf(stderr, "建立滤镜 %s（参数 %s）失败：%s\n", filter_name, args ? args : "无",
                av_err2str(ret));
    return ret;
}

/* 按第一帧的参数搭建滤镜图。time_base 是帧上时间戳的单位。 */
static int init_graph(Chain *c, const AVFrame *frame, AVRational time_base)
{
    AVFilterContext *volume, *aformat;
    char layout[64], args[256];
    int ret;

    c->graph = avfilter_graph_alloc();
    if (!c->graph)
        return AVERROR(ENOMEM);

    /*
     * 入口。它不会去看帧，必须明确告诉它将要送进来的帧是什么样的。
     * 参数写成 "键=值:键=值" 的字符串，和 ffmpeg 命令行里滤镜参数的写法相同。
     */
    av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));
    snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
             time_base.num, time_base.den, frame->sample_rate,
             av_get_sample_fmt_name(frame->format), layout);
    ret = create_filter(&c->src, c->graph, "abuffer", "src", args);
    if (ret < 0)
        return ret;
    printf("入口 abuffer 的参数：%s\n", args);

    /* 音量。命令行上写的 0.5、2、-6dB 原样交给它解析。 */
    snprintf(args, sizeof(args), "volume=%s", c->volume);
    ret = create_filter(&volume, c->graph, "volume", "vol", args);
    if (ret < 0)
        return ret;

    /* 限定出口的格式：一定是 s16；指定了采样率的话再加上采样率。没有写的项不限制。 */
    if (c->want_rate)
        snprintf(args, sizeof(args), "sample_fmts=s16:sample_rates=%d", c->want_rate);
    else
        snprintf(args, sizeof(args), "sample_fmts=s16");
    ret = create_filter(&aformat, c->graph, "aformat", "fmt", args);
    if (ret < 0)
        return ret;

    /* 出口，没有参数。 */
    ret = create_filter(&c->sink, c->graph, "abuffersink", "sink", NULL);
    if (ret < 0)
        return ret;

    /* 连接：入口 -> volume -> aformat -> 出口。每个滤镜都只有一个输入、一个输出，序号都是 0。 */
    ret = avfilter_link(c->src, 0, volume, 0);
    if (ret >= 0)
        ret = avfilter_link(volume, 0, aformat, 0);
    if (ret >= 0)
        ret = avfilter_link(aformat, 0, c->sink, 0);
    if (ret < 0) {
        fprintf(stderr, "连接滤镜失败：%s\n", av_err2str(ret));
        return ret;
    }

    /* 配置：检查连接，协商每条连接上的格式，必要时自动插入转换滤镜。 */
    ret = avfilter_graph_config(c->graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "配置滤镜图失败：%s\n", av_err2str(ret));
        return ret;
    }

    /*
     * 配置之后从入口开始顺着连接走一遍，看每条连接上协商出的格式。
     * 自动插入的转换滤镜也在里面，实例名以 auto_ 开头。
     */
    printf("图里的连接（上游 -> 下游：这条连接上的格式）：\n");
    for (const AVFilterContext *f = c->src; f->nb_outputs; f = f->outputs[0]->dst) {
        const AVFilterLink *link = f->outputs[0];

        av_channel_layout_describe(&link->ch_layout, layout, sizeof(layout));
        printf("  %s -> %s：%s，%d Hz，%s\n", f->name, link->dst->name,
               av_get_sample_fmt_name(link->format), link->sample_rate, layout);
    }

    /* 出口实际给什么格式，要在配置之后问它。 */
    c->out_rate     = av_buffersink_get_sample_rate(c->sink);
    c->out_channels = av_buffersink_get_channels(c->sink);
    printf("出口 abuffersink 的格式：%s，%d Hz，%d 声道\n",
           av_get_sample_fmt_name(av_buffersink_get_format(c->sink)), c->out_rate, c->out_channels);
    return 0;
}

/*
 * 把出口现在能取的帧都取出来，写进文件。
 * 返回 0 表示暂时取完了（要再送输入），返回 1 表示图已经结束，负数是错误。
 */
static int drain(Chain *c)
{
    AVFrame *out = c->out;

    for (;;) {
        size_t bytes;
        int ret = av_buffersink_get_frame(c->sink, out);

        if (ret == AVERROR(EAGAIN))
            return 0;
        if (ret == AVERROR_EOF)
            return 1;
        if (ret < 0) {
            fprintf(stderr, "从滤镜图取帧出错：%s\n", av_err2str(ret));
            return ret;
        }
        /* 出口限定了 s16（packed），数据都在 data[0]，各声道交错。 */
        bytes = (size_t)out->nb_samples * out->ch_layout.nb_channels * 2;
        if (fwrite(out->data[0], 1, bytes, c->file) != bytes) {
            fprintf(stderr, "写输出文件出错\n");
            av_frame_unref(out);
            return AVERROR(EIO);
        }
        c->nb_out     += out->nb_samples;
        c->data_bytes += bytes;
        av_frame_unref(out);
    }
}

/* 处理解码器给的一帧：第一帧时建图，然后送进入口，再把出口能取的都取走。 */
static int process_frame(Chain *c, AVFrame *frame, AVRational time_base)
{
    int ret;

    /* 文件没写声道布局时补上默认的（L10）。入口和后面的格式协商都需要知道布局。 */
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        const int nb_channels = frame->ch_layout.nb_channels;

        av_channel_layout_uninit(&frame->ch_layout);
        av_channel_layout_default(&frame->ch_layout, nb_channels);
    }

    if (!c->graph) {
        ret = init_graph(c, frame, time_base);
        if (ret < 0)
            return ret;
    }

    c->nb_in += frame->nb_samples;
    /*
     * 送进入口。这个函数会把帧里的数据拿走，返回之后 frame 是空的。
     * 帧的参数如果和建图时说的不一样，这里会报错：图配置好之后格式就固定了。
     */
    ret = av_buffersrc_add_frame(c->src, frame);
    if (ret < 0) {
        fprintf(stderr, "把帧送进滤镜图出错：%s\n", av_err2str(ret));
        return ret;
    }
    ret = drain(c);
    return ret < 0 ? ret : 0;
}

/* 送一个包（NULL 表示冲刷解码器），把解出的帧逐个处理。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Chain *c)
{
    int ret = avcodec_send_packet(dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包出错：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
            return ret;
        }
        ret = process_frame(c, frame, dec->pkt_timebase);
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVStream *stream;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    Chain c = { 0 };
    int64_t before_flush;
    int ret, index;

    if (argc < 4 || argc > 5) {
        fprintf(stderr, "用法：%s <输入文件> <输出.wav> <音量> [采样率]\n"
                "音量可以写倍数（0.5、2）或分贝（-6dB）\n", argv[0]);
        return 1;
    }
    c.volume = argv[3];
    if (argc == 5) {
        char *end_ptr;
        const long rate = strtol(argv[4], &end_ptr, 10);

        if (end_ptr == argv[4] || *end_ptr || rate <= 0 || rate > 768000) {
            fprintf(stderr, "采样率要写成正整数，例如 48000\n");
            return 1;
        }
        c.want_rate = rate;
    }

    ret = avformat_open_input(&fmt, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不出流信息：%s\n", av_err2str(ret));
        goto end;
    }

    /* 建立音频解码器，步骤和 L07 相同。 */
    index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0) {
        ret = index;
        fprintf(stderr, "%s 里没有能解码的音频流\n", argv[1]);
        goto end;
    }
    stream = fmt->streams[index];
    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avcodec_parameters_to_context(dec, stream->codecpar);
    if (ret < 0)
        goto end;
    dec->pkt_timebase = stream->time_base;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器 %s：%s\n", codec->name, av_err2str(ret));
        goto end;
    }

    c.out = av_frame_alloc();
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!c.out || !pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    c.file = fopen(argv[2], "wb");
    if (!c.file) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开输出文件 %s\n", argv[2]);
        goto end;
    }
    /* 先占住文件头的位置，数据写完再回来填（L06）。 */
    ret = write_wav_header(c.file, 0, 0, 0);
    if (ret < 0)
        goto end;

    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == index)
            ret = decode(dec, pkt, frame, &c);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    /* 从上游到下游依次冲刷（L10）：先解码器。 */
    ret = decode(dec, NULL, frame, &c);
    if (ret < 0)
        goto end;
    if (!c.graph) {
        fprintf(stderr, "没有解出任何音频\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /*
     * 再冲刷滤镜图：往入口送 NULL，告诉它输入结束了。图里的滤镜依次把留着的数据处理完，
     * 出口最后返回 AVERROR_EOF（drain() 返回 1）。
     */
    before_flush = c.nb_out;
    ret = av_buffersrc_add_frame(c.src, NULL);
    if (ret < 0) {
        fprintf(stderr, "冲刷滤镜图出错：%s\n", av_err2str(ret));
        goto end;
    }
    ret = drain(&c);
    if (ret < 0)
        goto end;
    if (ret != 1) {
        fprintf(stderr, "滤镜图没有正常结束\n");
        ret = AVERROR_BUG;
        goto end;
    }

    if (c.data_bytes > UINT32_MAX - WAV_HEADER_SIZE) {
        fprintf(stderr, "数据超过 4 GB，WAV 文件头放不下这个长度\n");
        ret = AVERROR(EINVAL);
        goto end;
    }
    if (fseek(c.file, 0, SEEK_SET) < 0) {
        ret = AVERROR(errno);
        goto end;
    }
    ret = write_wav_header(c.file, c.out_rate, c.out_channels, (uint32_t)c.data_bytes);
    if (ret < 0)
        goto end;

    printf("送进 %" PRId64 " 个采样，取出 %" PRId64 " 个采样，其中冲刷时取出 %" PRId64 " 个\n",
           c.nb_in, c.nb_out, c.nb_out - before_flush);
    printf("写出 %s：%d 字节文件头 + %" PRId64 " 字节数据，时长 %.3f 秒\n", argv[2], WAV_HEADER_SIZE,
           c.data_bytes, (double)c.nb_out / c.out_rate);
    ret = 0;

end:
    if (c.file && fclose(c.file) && ret >= 0) {
        fprintf(stderr, "写 %s 出错\n", argv[2]);
        ret = AVERROR(EIO);
    }
    /* 释放图的同时释放图里所有的滤镜实例，c.src 和 c.sink 不用单独释放。 */
    avfilter_graph_free(&c.graph);
    av_frame_free(&c.out);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
