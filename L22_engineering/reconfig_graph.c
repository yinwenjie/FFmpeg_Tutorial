/*
 * L22 工程化：输入的参数中途变了，滤镜图要重建。
 *
 * 用法：reconfig_graph <输入文件> <输出文件> <video|audio> <滤镜图描述> [reconfig=1|0] [flush=1|0] [dump=0|1]
 *
 * 例子：
 *   reconfig_graph in.mp4 out.yuv video "hflip"
 *   reconfig_graph two_sizes.h264 out.yuv video "null"            # 中途换了分辨率
 *   reconfig_graph change.aac out.pcm audio "volume=0.5"          # 中途换了采样率和声道数（L10 第 5 步的文件）
 *   reconfig_graph two_sizes.h264 out.yuv video "null" reconfig=0 # 不重建：看看会怎样
 *   reconfig_graph two_sizes.h264 out.yuv video "tpad=stop_mode=clone:stop_duration=0.5" flush=0
 *                                                                 # 重建时不给旧图送“结束”，做法和 ffmpeg 命令行一样
 *
 * 输出是裸数据：视频是 yuv420p 的各帧首尾相接，音频是 s16 交织的 PCM，所以输出的尺寸、采样率、声道数在整个文件里必须不变。
 * 播放：
 *   ffplay -f rawvideo -pixel_format yuv420p -video_size 宽x高 out.yuv
 *   ffplay -f s16le -sample_rate 采样率 -ch_layout stereo out.pcm
 *
 * 滤镜图建好以后，入口滤镜（buffer、abuffer）的格式就固定了，之后送进去的帧必须和建图时说的一致（L11 第 6 步）。
 * 解码器吐出来的帧参数中途变了（分辨率变了、采样率变了、声道数变了），程序要自己发现，然后：
 *   1. 把旧图冲刷干净：送一个空帧，把图里剩下的帧取完（flush=0 时只取现在能取的，不送空帧，见下）；
 *   2. 释放旧图；
 *   3. 按新参数建一张新图；
 *   4. 新图的出口必须和旧图一样（尺寸、采样率、声道布局），因为输出文件只有一种格式。
 *      做法是在描述的末尾加一个固定参数的 scale（视频）或 aformat（音频）。ffmpeg 命令行用的也是这个办法。
 * 这四步就是本程序的 feed() 函数，其余部分和 L11、L12 的程序基本相同。
 *
 * 第 1 步有两种做法，结果不同：
 *   flush=1（默认）：送空帧。图里攒着的帧（重采样器的尾巴、fps 留着的最后一帧）都会输出，一个不丢；
 *                     代价是所有“收到结束就做点什么”的滤镜（tpad 补帧、apad、loop）每次重建都会触发一次。
 *   flush=0：         只取图里现在能取的帧，然后直接丢掉旧图。ffmpeg 命令行是这样做的（fftools/ffmpeg_filter.c 的 send_frame()
 *                     里调用 read_frames()，没有送空帧）；图里攒着的帧丢了，但“结束时做点什么”的滤镜只在真正的结尾触发一次。
 * 没有哪个是唯一正确的，要看你的滤镜图里放了什么。
 *
 * 判断“参数变了”看哪些值，也是照着命令行的做法：
 *   视频：像素格式、宽、高、宽高比、色彩空间、色彩范围；
 *   音频：采样格式、采样率、声道布局。
 * 时间戳、帧的类型、码率之类的变化不需要重建。
 */

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
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
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

/* 入口滤镜需要知道的、判断“是否要重建”时要比较的帧参数。 */
typedef struct Props {
    int                format;          /* 像素格式或采样格式 */
    int                width, height;   /* 视频 */
    AVRational         sar;
    enum AVColorSpace  space;
    enum AVColorRange  range;
    int                sample_rate;     /* 音频 */
    AVChannelLayout    layout;
} Props;

typedef struct Ctx {
    enum AVMediaType type;
    const char      *descr;             /* 命令行给的滤镜图描述 */
    int              reconfig;          /* 1：参数变了就重建；0：不重建，原样送进去 */
    int              flush_old;         /* 1：重建前给旧图送空帧（结束）；0：只取现在能取的帧，和命令行一样 */
    int              dump;              /* 1：每次建图以后打印图的结构 */
    AVRational       time_base;         /* 流的时间基 */
    AVRational       frame_rate;        /* 视频的帧率 */

    AVFilterGraph   *graph;
    AVFilterContext *src;               /* 入口 */
    AVFilterContext *sink;              /* 出口 */
    AVFrame         *out;               /* 从出口取帧用，反复使用 */
    FILE            *file;

    Props            cur;               /* 当前这张图建图时说的输入参数 */

    /* 第一张图配置好以后问出来的出口格式。以后每一张图的出口都要和它一样。 */
    int              pinned;
    int              pin_w, pin_h;
    int              pin_rate;
    char             pin_layout[128];

    int              nb_graphs;         /* 建了几张图 */
    int64_t          nb_in;             /* 送进图的帧数 */
    int64_t          nb_out;            /* 从图里取出的帧数 */
    int64_t          nb_flushed;        /* 其中重建时冲刷旧图取出的帧数 */
    int64_t          next_pts;          /* 帧没有时间戳时，按帧率或采样数推算出的下一个 pts（流的时间基） */
} Ctx;

static void props_free(Props *p)
{
    av_channel_layout_uninit(&p->layout);
    memset(p, 0, sizeof(*p));
}

static int props_from_frame(Props *p, const AVFrame *f, enum AVMediaType type)
{
    props_free(p);
    p->format = f->format;
    if (type == AVMEDIA_TYPE_VIDEO) {
        p->width  = f->width;
        p->height = f->height;
        p->sar    = f->sample_aspect_ratio;
        p->space  = f->colorspace;
        p->range  = f->color_range;
        return 0;
    }
    p->sample_rate = f->sample_rate;
    return av_channel_layout_copy(&p->layout, &f->ch_layout);
}

static int props_equal(const Props *a, const Props *b, enum AVMediaType type)
{
    if (a->format != b->format)
        return 0;
    if (type == AVMEDIA_TYPE_VIDEO)
        return a->width == b->width && a->height == b->height &&
               !av_cmp_q(a->sar, b->sar) && a->space == b->space && a->range == b->range;
    return a->sample_rate == b->sample_rate && !av_channel_layout_compare(&a->layout, &b->layout);
}

/* 把“哪些参数变了”写成一行字，打印给使用者看。 */
static void describe_change(char *buf, size_t size, const Props *a, const Props *b,
                            enum AVMediaType type)
{
    char x[64], y[64];
    size_t n = 0;

#define ADD(...) do { if (n < size) n += snprintf(buf + n, size - n, __VA_ARGS__); } while (0)
    buf[0] = 0;
    if (type == AVMEDIA_TYPE_VIDEO) {
        if (a->width != b->width || a->height != b->height)
            ADD("尺寸 %dx%d → %dx%d；", a->width, a->height, b->width, b->height);
        if (a->format != b->format)
            ADD("像素格式 %s → %s；", av_get_pix_fmt_name(a->format), av_get_pix_fmt_name(b->format));
        if (av_cmp_q(a->sar, b->sar))
            ADD("宽高比 %d:%d → %d:%d；", a->sar.num, a->sar.den, b->sar.num, b->sar.den);
        if (a->space != b->space)
            ADD("色彩空间 %s → %s；", av_color_space_name(a->space), av_color_space_name(b->space));
        if (a->range != b->range)
            ADD("色彩范围 %s → %s；", av_color_range_name(a->range), av_color_range_name(b->range));
    } else {
        if (a->sample_rate != b->sample_rate)
            ADD("采样率 %d → %d Hz；", a->sample_rate, b->sample_rate);
        if (av_channel_layout_compare(&a->layout, &b->layout)) {
            av_channel_layout_describe(&a->layout, x, sizeof(x));
            av_channel_layout_describe(&b->layout, y, sizeof(y));
            ADD("声道 %s → %s；", x, y);
        }
        if (a->format != b->format)
            ADD("采样格式 %s → %s；", av_get_sample_fmt_name(a->format), av_get_sample_fmt_name(b->format));
    }
#undef ADD
}

/* 一行描述：视频是“320x180 yuv420p”，音频是“fltp 44100 Hz stereo”。 */
static void describe_props(char *buf, size_t size, const Props *p, enum AVMediaType type)
{
    if (type == AVMEDIA_TYPE_VIDEO) {
        snprintf(buf, size, "%dx%d %s", p->width, p->height, av_get_pix_fmt_name(p->format));
    } else {
        char layout[64];

        av_channel_layout_describe(&p->layout, layout, sizeof(layout));
        snprintf(buf, size, "%s %d Hz %s", av_get_sample_fmt_name(p->format), p->sample_rate, layout);
    }
}

/*
 * 裸码流（.h264、.m1v、.aac 这样的 ES 文件）里没有时间戳，解码出来的帧 pts 是 AV_NOPTS_VALUE。
 * fps、trim、setpts 这类要用时间的滤镜遇到它，会把帧全部丢掉，什么也不输出，也不报错。
 * ffmpeg 命令行的做法是按帧率（音频按采样数）自己推算，这里照做：
 * 帧有 pts 就用它，并记下“下一帧应该是多少”；没有就用推算的值。
 */
static void fix_pts(Ctx *c, AVFrame *frame)
{
    int64_t step;

    if (frame->pts == AV_NOPTS_VALUE)
        frame->pts = c->next_pts;
    if (c->type == AVMEDIA_TYPE_VIDEO)
        step = c->frame_rate.num > 0 ? av_rescale_q(1, av_inv_q(c->frame_rate), c->time_base) : 1;
    else
        step = av_rescale_q(frame->nb_samples, (AVRational){ 1, frame->sample_rate }, c->time_base);
    c->next_pts = frame->pts + FFMAX(step, 1);
}

/*
 * 建一张新图。描述按命令行的写法，两头用名字 [in] 和 [out] 接到入口和出口；
 * 出口前面放一个“固定出口格式”的滤镜 pin：
 *   视频：scale=W:H（第一张图不放，因为这时还不知道出口多大）；
 *   音频：aformat=sample_fmts=s16 加上采样率和声道布局（同样第一张图只限定采样格式）。
 * 描述里想怎么改尺寸、采样率都可以，最后都被 pin 拉回第一张图的出口格式。
 */
static int build_graph(Ctx *c, const AVFrame *frame)
{
    const int video = c->type == AVMEDIA_TYPE_VIDEO;
    AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterContext *pin = NULL;
    char args[256];
    int ret;

    c->graph = avfilter_graph_alloc();
    if (!par || !outputs || !inputs || !c->graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* 入口：说的参数和送进去的帧一致。buffer 和 abuffer 都用同一个结构体传参数。 */
    c->src = avfilter_graph_alloc_filter(c->graph, avfilter_get_by_name(video ? "buffer" : "abuffer"), "in");
    if (!c->src) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    par->format    = frame->format;
    par->time_base = c->time_base;
    if (video) {
        par->width               = frame->width;
        par->height              = frame->height;
        par->sample_aspect_ratio = frame->sample_aspect_ratio;
        par->color_space         = frame->colorspace;
        par->color_range         = frame->color_range;
        par->alpha_mode          = frame->alpha_mode;
        par->chroma_location     = frame->chroma_location;
        par->frame_rate          = c->frame_rate;
    } else {
        par->sample_rate = frame->sample_rate;
        ret = av_channel_layout_copy(&par->ch_layout, &frame->ch_layout);
        if (ret < 0)
            goto end;
    }
    ret = av_buffersrc_parameters_set(c->src, par);
    if (ret >= 0)
        ret = avfilter_init_dict(c->src, NULL);
    if (ret < 0) {
        fprintf(stderr, "建立入口滤镜失败：%s\n", av_err2str(ret));
        goto end;
    }

    /* 出口。视频限定 yuv420p；音频的格式由下面的 aformat 限定。 */
    c->sink = avfilter_graph_alloc_filter(c->graph, avfilter_get_by_name(video ? "buffersink" : "abuffersink"), "out");
    if (!c->sink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (video)
        ret = av_opt_set(c->sink, "pixel_formats", "yuv420p", AV_OPT_SEARCH_CHILDREN);
    else
        ret = 0;
    if (ret >= 0)
        ret = avfilter_init_dict(c->sink, NULL);
    if (ret < 0) {
        fprintf(stderr, "建立出口滤镜失败：%s\n", av_err2str(ret));
        goto end;
    }

    /* 出口前面的“固定格式”滤镜。 */
    if (video && c->pinned) {
        snprintf(args, sizeof(args), "%d:%d", c->pin_w, c->pin_h);
        ret = avfilter_graph_create_filter(&pin, avfilter_get_by_name("scale"), "pin", args, NULL, c->graph);
    } else if (!video) {
        if (c->pinned)
            snprintf(args, sizeof(args), "sample_fmts=s16:sample_rates=%d:channel_layouts=%s",
                     c->pin_rate, c->pin_layout);
        else
            snprintf(args, sizeof(args), "sample_fmts=s16");
        ret = avfilter_graph_create_filter(&pin, avfilter_get_by_name("aformat"), "pin", args, NULL, c->graph);
    } else {
        ret = 0;
    }
    if (ret < 0) {
        fprintf(stderr, "建立固定出口格式的滤镜失败：%s\n", av_err2str(ret));
        goto end;
    }

    /*
     * 解析描述。outputs 是入口滤镜空着的输出口，接到描述里的 [in]；
     * inputs 是下游空着的输入口（pin 的输入，没有 pin 时是出口滤镜的输入），接到描述里的 [out]。
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = c->src;
    outputs->pad_idx    = 0;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = pin ? pin : c->sink;
    inputs->pad_idx     = 0;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avfilter_graph_parse_ptr(c->graph, c->descr, &inputs, &outputs, NULL);
    if (ret < 0) {
        fprintf(stderr, "解析滤镜图描述 \"%s\" 失败：%s\n", c->descr, av_err2str(ret));
        goto end;
    }
    if (pin) {
        ret = avfilter_link(pin, 0, c->sink, 0);
        if (ret < 0)
            goto end;
    }
    ret = avfilter_graph_config(c->graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "配置滤镜图失败：%s\n", av_err2str(ret));
        goto end;
    }

    if (!c->pinned) {
        /* 第一张图：记下出口的格式，以后的图都要和它一样。 */
        if (video) {
            c->pin_w = av_buffersink_get_w(c->sink);
            c->pin_h = av_buffersink_get_h(c->sink);
        } else {
            AVChannelLayout layout = { 0 };

            ret = av_buffersink_get_ch_layout(c->sink, &layout);
            if (ret < 0)
                goto end;
            av_channel_layout_describe(&layout, c->pin_layout, sizeof(c->pin_layout));
            av_channel_layout_uninit(&layout);
            c->pin_rate = av_buffersink_get_sample_rate(c->sink);
        }
        c->pinned = 1;
    }
    if (video && (av_buffersink_get_w(c->sink) != c->pin_w || av_buffersink_get_h(c->sink) != c->pin_h)) {
        fprintf(stderr, "新图的出口是 %dx%d，和第一张图的 %dx%d 不一样\n", av_buffersink_get_w(c->sink),
                av_buffersink_get_h(c->sink), c->pin_w, c->pin_h);
        ret = AVERROR_BUG;
        goto end;
    }

    c->nb_graphs++;
    if (c->dump) {
        char *s = avfilter_graph_dump(c->graph, NULL);

        printf("第 %d 张图：\n%s\n", c->nb_graphs, s ? s : "（无法打印）");
        av_free(s);
    }
    ret = 0;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (par)
        av_channel_layout_uninit(&par->ch_layout);
    av_free(par);
    return ret;
}

/* 写一个平面，逐行写，跳过每行末尾的填充字节（L05）。 */
static int write_plane(FILE *f, const uint8_t *data, int linesize, int w, int h)
{
    for (int y = 0; y < h; y++) {
        if (fwrite(data + (ptrdiff_t)y * linesize, 1, w, f) != (size_t)w)
            return AVERROR(EIO);
    }
    return 0;
}

/* 把出口现在能取的帧都取出来，写进文件。返回 0：暂时取完了；1：图已经结束；负数：错误。 */
static int drain(Ctx *c)
{
    AVFrame *out = c->out;

    for (;;) {
        int ret = av_buffersink_get_frame(c->sink, out);

        if (ret == AVERROR(EAGAIN))
            return 0;
        if (ret == AVERROR_EOF)
            return 1;
        if (ret < 0) {
            fprintf(stderr, "从滤镜图取帧出错：%s\n", av_err2str(ret));
            return ret;
        }
        if (c->type == AVMEDIA_TYPE_VIDEO) {
            const int cw = (out->width + 1) / 2, ch = (out->height + 1) / 2;

            ret = write_plane(c->file, out->data[0], out->linesize[0], out->width, out->height);
            if (ret >= 0)
                ret = write_plane(c->file, out->data[1], out->linesize[1], cw, ch);
            if (ret >= 0)
                ret = write_plane(c->file, out->data[2], out->linesize[2], cw, ch);
        } else {
            const size_t bytes = (size_t)out->nb_samples * out->ch_layout.nb_channels * 2;

            ret = fwrite(out->data[0], 1, bytes, c->file) == bytes ? 0 : AVERROR(EIO);
        }
        av_frame_unref(out);
        if (ret < 0) {
            fprintf(stderr, "写输出文件出错\n");
            return ret;
        }
        c->nb_out++;
    }
}

/* 告诉当前这张图输入结束了，把图里剩下的帧取完。 */
static int flush_graph(Ctx *c)
{
    int ret = av_buffersrc_add_frame(c->src, NULL);

    if (ret < 0) {
        fprintf(stderr, "冲刷滤镜图出错：%s\n", av_err2str(ret));
        return ret;
    }
    ret = drain(c);
    if (ret < 0)
        return ret;
    if (ret != 1) {
        fprintf(stderr, "滤镜图没有正常结束\n");
        return AVERROR_BUG;
    }
    return 0;
}

/* 把解码器给的一帧送进滤镜图。第一帧建图；参数变了就按上面说的四步重建。 */
static int feed(Ctx *c, AVFrame *frame)
{
    Props now = { 0 };
    char why[256];
    int ret;

    /* 声道布局没指定的帧（有些码流不写），按声道数补一个默认的，不然每帧都和建图时说的“不同”。 */
    if (c->type == AVMEDIA_TYPE_AUDIO && frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        const int nb_channels = frame->ch_layout.nb_channels;

        av_channel_layout_uninit(&frame->ch_layout);
        av_channel_layout_default(&frame->ch_layout, nb_channels);
    }
    ret = props_from_frame(&now, frame, c->type);
    if (ret < 0)
        goto end;

    if (!c->graph) {
        ret = build_graph(c, frame);
        if (ret >= 0)
            ret = props_from_frame(&c->cur, frame, c->type);
        if (ret < 0)
            goto end;
        describe_props(why, sizeof(why), &c->cur, c->type);
        printf("第 1 张图：输入 %s\n", why);
    } else if (!props_equal(&c->cur, &now, c->type) && c->reconfig) {
        int64_t before = c->nb_out;
        char now_desc[256];

        describe_change(why, sizeof(why), &c->cur, &now, c->type);
        describe_props(now_desc, sizeof(now_desc), &now, c->type);
        printf("第 %" PRId64 " 帧：参数变了，%s重建滤镜图（新输入 %s）\n", c->nb_in + 1, why, now_desc);
        if (!c->flush_old)
            printf("  flush=0：旧图不送结束，只取现在能取的帧\n");

        /* 第 1 步：旧图送空帧、取完；第 2 步：释放；第 3 步：建新图。第 4 步在 build_graph() 里。 */
        ret = c->flush_old ? flush_graph(c) : drain(c);
        if (ret < 0)
            goto end;
        c->nb_flushed += c->nb_out - before;
        avfilter_graph_free(&c->graph);
        c->src = c->sink = NULL;
        ret = build_graph(c, frame);
        if (ret >= 0)
            ret = props_from_frame(&c->cur, frame, c->type);
        if (ret < 0)
            goto end;
    }

    c->nb_in++;
    /* 送进入口。成功时帧里的数据被拿走。参数变了又不重建的话，这里就是入口滤镜的反应。 */
    ret = av_buffersrc_add_frame(c->src, frame);
    if (ret < 0) {
        fprintf(stderr, "第 %" PRId64 " 帧：送进滤镜图出错：%s\n", c->nb_in, av_err2str(ret));
        goto end;
    }
    ret = drain(c);
    if (ret > 0) {
        fprintf(stderr, "滤镜图提前结束了\n");
        ret = AVERROR_BUG;
    }

end:
    props_free(&now);
    return ret < 0 ? ret : 0;
}

/* 送一个包（NULL 表示冲刷解码器），把解出的帧逐个送进 feed()。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Ctx *c)
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
        if (c->type == AVMEDIA_TYPE_VIDEO)
            frame->pts = frame->best_effort_timestamp;
        fix_pts(c, frame);
        ret = feed(c, frame);
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
    Ctx c = { 0 };
    int64_t before_flush;
    int ret, index;

    c.reconfig  = 1;
    c.flush_old = 1;
    if (argc < 5) {
        fprintf(stderr, "用法：%s <输入文件> <输出文件> <video|audio> <滤镜图描述> [reconfig=1|0] [flush=1|0] [dump=0|1]\n",
                argv[0]);
        return 1;
    }
    if (!strcmp(argv[3], "video")) {
        c.type = AVMEDIA_TYPE_VIDEO;
    } else if (!strcmp(argv[3], "audio")) {
        c.type = AVMEDIA_TYPE_AUDIO;
    } else {
        fprintf(stderr, "第三个参数要写 video 或 audio，不是 %s\n", argv[3]);
        return 1;
    }
    c.descr = argv[4];
    for (int i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "reconfig=1")) {
            c.reconfig = 1;
        } else if (!strcmp(argv[i], "reconfig=0")) {
            c.reconfig = 0;
        } else if (!strcmp(argv[i], "flush=1")) {
            c.flush_old = 1;
        } else if (!strcmp(argv[i], "flush=0")) {
            c.flush_old = 0;
        } else if (!strcmp(argv[i], "dump=1")) {
            c.dump = 1;
        } else if (!strcmp(argv[i], "dump=0")) {
            c.dump = 0;
        } else {
            fprintf(stderr, "不认识的参数 %s\n", argv[i]);
            return 1;
        }
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
    index = av_find_best_stream(fmt, c.type, -1, -1, &codec, 0);
    if (index < 0) {
        ret = index;
        fprintf(stderr, "%s 里没有能解码的%s流\n", argv[1], argv[3][0] == 'v' ? "视频" : "音频");
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
    c.time_base  = stream->time_base;
    c.frame_rate = c.type == AVMEDIA_TYPE_VIDEO ? av_guess_frame_rate(fmt, stream, NULL) : (AVRational){ 0, 1 };

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

    /* 从上游到下游依次冲刷：解码器，然后是滤镜图。 */
    ret = decode(dec, NULL, frame, &c);
    if (ret < 0)
        goto end;
    if (!c.graph) {
        fprintf(stderr, "没有解出任何帧\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    before_flush = c.nb_out;
    ret = flush_graph(&c);
    if (ret < 0)
        goto end;

    printf("建了 %d 张图；送进 %" PRId64 " 帧，取出 %" PRId64 " 帧（重建时冲刷旧图取出 %" PRId64
           " 帧，最后冲刷取出 %" PRId64 " 帧）\n",
           c.nb_graphs, c.nb_in, c.nb_out, c.nb_flushed, c.nb_out - before_flush);
    if (c.type == AVMEDIA_TYPE_VIDEO)
        printf("输出：%dx%d yuv420p，每帧 %d 字节\n", c.pin_w, c.pin_h,
               c.pin_w * c.pin_h + 2 * ((c.pin_w + 1) / 2) * ((c.pin_h + 1) / 2));
    else
        printf("输出：s16，%d Hz，%s\n", c.pin_rate, c.pin_layout);
    ret = 0;

end:
    if (c.file && fclose(c.file) && ret >= 0) {
        fprintf(stderr, "写 %s 出错\n", argv[2]);
        ret = AVERROR(EIO);
    }
    avfilter_graph_free(&c.graph);
    props_free(&c.cur);
    av_frame_free(&c.out);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
