/*
 * 解码器和滤镜图的公共代码：thumb、audio、transcode 都要用。
 * 写法来自 L16 的 transcode_av.c（入口按解码器的参数、出口按编码器支持的格式、中间是一段描述）。
 */
#include <math.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

#include "mtool.h"

int mt_open_decoder(const AVStream *st, int keyframes_only, AVCodecContext **out)
{
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *dec;
    int ret;

    if (!codec) {
        mt_averr(AVERROR_DECODER_NOT_FOUND, "没有 %s 的解码器", avcodec_get_name(st->codecpar->codec_id));
        return AVERROR_DECODER_NOT_FOUND;
    }
    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        mt_err("内存不够");
        return AVERROR(ENOMEM);
    }
    ret = avcodec_parameters_to_context(dec, st->codecpar);
    if (ret < 0) {
        mt_averr(ret, "读取 %s 流的参数失败", codec->name);
        goto fail;
    }
    dec->pkt_timebase = st->time_base;     /* 解码出来的帧的时间戳以它为单位（L5） */
    if (keyframes_only)
        dec->skip_frame = AVDISCARD_NONKEY;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        mt_averr(ret, "打不开解码器 %s", codec->name);
        goto fail;
    }
    *out = dec;
    return 0;
fail:
    avcodec_free_context(&dec);
    return ret;
}

/* 出口只接受编码器支持的格式。configs 为 NULL 表示编码器对这一项没有限制。 */
static int restrict_sink(AVFilterContext *sink, const AVCodec *enc, int audio)
{
    const void *configs;
    int n, ret = 0;

    if (!enc)
        return 0;
    if (!audio) {
        ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &n);
        if (ret >= 0 && configs)
            ret = av_opt_set_array(sink, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, n, AV_OPT_TYPE_PIXEL_FMT, configs);
        return ret;
    }
    ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &n);
    if (ret >= 0 && configs)
        ret = av_opt_set_array(sink, "sample_formats", AV_OPT_SEARCH_CHILDREN, 0, n, AV_OPT_TYPE_SAMPLE_FMT, configs);
    if (ret >= 0)
        ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &n);
    if (ret >= 0 && configs)
        ret = av_opt_set_array(sink, "samplerates", AV_OPT_SEARCH_CHILDREN, 0, n, AV_OPT_TYPE_INT, configs);
    if (ret >= 0)
        ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &configs, &n);
    if (ret >= 0 && configs)
        ret = av_opt_set_array(sink, "channel_layouts", AV_OPT_SEARCH_CHILDREN, 0, n, AV_OPT_TYPE_CHLAYOUT, configs);
    return ret;
}

int mt_graph_init(MtGraph *g, AVCodecContext *dec, AVRational frame_rate, const char *desc, const AVCodec *enc)
{
    const int audio = dec->codec_type == AVMEDIA_TYPE_AUDIO;
    const AVFilter *bsrc  = avfilter_get_by_name(audio ? "abuffer" : "buffer");
    const AVFilter *bsink = avfilter_get_by_name(audio ? "abuffersink" : "buffersink");
    AVFilterInOut *outputs = avfilter_inout_alloc(), *inputs = avfilter_inout_alloc();
    char args[512], layout[64];
    int ret;

    memset(g, 0, sizeof(*g));
    g->graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !g->graph || !bsrc || !bsink) {
        ret = AVERROR(ENOMEM);
        mt_err("建滤镜图失败（内存不够，或者这份 FFmpeg 没有 buffer 滤镜）");
        goto end;
    }
    if (audio) {
        if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)    /* 有的文件只知道声道数：给一个默认布局 */
            av_channel_layout_default(&dec->ch_layout, dec->ch_layout.nb_channels);
        av_channel_layout_describe(&dec->ch_layout, layout, sizeof(layout));
        snprintf(args, sizeof(args), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                 dec->sample_rate, dec->sample_rate, av_get_sample_fmt_name(dec->sample_fmt), layout);
    } else {
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d:"
                 "colorspace=%d:range=%d:alpha_mode=%d:chroma_location=%d",
                 dec->width, dec->height, dec->pix_fmt, dec->pkt_timebase.num, dec->pkt_timebase.den,
                 dec->sample_aspect_ratio.num, FFMAX(dec->sample_aspect_ratio.den, 1),
                 frame_rate.num, FFMAX(frame_rate.den, 1),
                 dec->colorspace, dec->color_range, dec->alpha_mode, dec->chroma_sample_location);
    }
    ret = avfilter_graph_create_filter(&g->src, bsrc, "in", args, NULL, g->graph);
    if (ret < 0) {
        mt_averr(ret, "创建滤镜图入口失败（%s）", args);
        goto end;
    }
    g->sink = avfilter_graph_alloc_filter(g->graph, bsink, "out");
    if (!g->sink) {
        ret = AVERROR(ENOMEM);
        mt_err("内存不够");
        goto end;
    }
    if ((ret = restrict_sink(g->sink, enc, audio)) < 0 || (ret = avfilter_init_dict(g->sink, NULL)) < 0) {
        mt_averr(ret, "设置滤镜图出口失败");
        goto end;
    }
    outputs->name = av_strdup("in");
    outputs->filter_ctx = g->src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = g->sink;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        mt_err("内存不够");
        goto end;
    }
    if ((ret = avfilter_graph_parse_ptr(g->graph, desc && *desc ? desc : (audio ? "anull" : "null"),
                                        &inputs, &outputs, NULL)) < 0) {
        mt_averr(ret, "滤镜图“%s”有语法错误", desc);
        goto end;
    }
    if ((ret = avfilter_graph_config(g->graph, NULL)) < 0)
        mt_averr(ret, "配置滤镜图“%s”失败（格式对不上、或者滤镜参数不对）", desc);
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret < 0)
        mt_graph_free(g);
    return ret;
}

void mt_graph_free(MtGraph *g)
{
    avfilter_graph_free(&g->graph);
    g->src = g->sink = NULL;
}

/*
 * 和 ffmpeg 命令行自动旋转（fftools/ffmpeg_filter.c）同样的做法：
 * theta = -取整(显示矩阵的旋转角)，归到 [0, 360)；
 * 90 度用 transpose=clock，270 度用 transpose=cclock（显示矩阵带镜像时换成 cclock_flip、clock_flip），
 * 180 度按矩阵符号用 hflip 加 vflip，只有镜像没有旋转时用 vflip。
 * 不是 90 的倍数的旋转这里不处理，只给一条警告。
 */
void mt_rotation_filter(const AVStream *st, char *buf, size_t size)
{
    const AVPacketSideData *sd = av_packet_side_data_get(st->codecpar->coded_side_data, st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    const int32_t *m;
    double theta;

    buf[0] = '\0';
    if (!sd || sd->size < 9 * sizeof(int32_t))
        return;
    m = (const int32_t *)sd->data;
    theta = -round(av_display_rotation_get(m));
    theta -= 360 * floor(theta / 360 + 0.9 / 360);
    if (fabs(theta - 90) < 1.0)
        snprintf(buf, size, "transpose=%s", m[3] > 0 ? "cclock_flip" : "clock");
    else if (fabs(theta - 180) < 1.0)
        snprintf(buf, size, "%s%s%s", m[0] < 0 ? "hflip" : "", m[0] < 0 && m[4] < 0 ? "," : "", m[4] < 0 ? "vflip" : "");
    else if (fabs(theta - 270) < 1.0)
        snprintf(buf, size, "transpose=%s", m[3] < 0 ? "clock_flip" : "cclock");
    else if (fabs(theta) > 1.0)
        mt_warn("视频旋转了 %.0f 度，不是 90 的倍数，不处理", theta);
    else if (m[4] < 0)
        snprintf(buf, size, "vflip");
}
