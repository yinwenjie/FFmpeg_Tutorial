/*
 * L16 转码：读文件 → 解码 → 滤镜 → 编码 → 封装，把一个媒体文件转成另一种编码、另一种格式。
 *
 * 用法：transcode_av <输入文件> <输出文件> [名字=值 ...]
 *
 * 例子：
 *   transcode_av samples/small_360p25.mp4 out/t.mp4
 *   transcode_av samples/small_360p25.mp4 out/t.mkv
 *   transcode_av samples/small_360p25.mp4 out/cut.mp4 ss=2 t=1.5
 *   transcode_av samples/small_360p25.mp4 out/small.mp4 vf=scale=320:-2 copy=a
 *   transcode_av samples/small_360p25.mp4 out/t.mp4 trace=2
 *
 * 本程序自己用的参数：
 *   ss=秒         从输入的这个时间开始，精确到帧（视频）和采样（音频）
 *   t=秒          只转这么长
 *   vf=滤镜图     视频滤镜图，写法和 ffmpeg 的 -vf 相同（L11、L12），默认什么都不做
 *   af=滤镜图     音频滤镜图
 *   vcodec=名字   视频编码器          acodec=名字   音频编码器
 *   copy=v|a|va   这些流不解码也不编码，把压缩好的包原样拷贝到输出
 *   format=名字   输出格式（输出文件名没有扩展名或者不是普通文件时需要）
 *   trace=N       把前 N 帧、N 个包的时间戳在每个环节怎么变化打印出来
 *   log=1         打印每一个写进文件的包
 *   v:名字=值     交给视频编码器的选项，例如 v:crf=18 v:preset=fast
 *   a:名字=值     交给音频编码器的选项，例如 a:b=96k
 * 其余的“名字=值”都当作复用器的选项交给 avformat_write_header()，例如 movflags=+faststart。
 *
 * 数据的流向（每一路流各有一条，视频和音频互不相干，只在最后的复用器里汇合）：
 *
 *   输入文件 ──► 解复用 ──► 包 ──► 解码器 ──► 帧 ──► 滤镜图 ──► 帧 ──► 编码器 ──► 包 ──► 复用器 ──► 输出文件
 *                    │                                                                      ▲
 *                    └────────────────────── copy=：包原样拷贝 ────────────────────────────┘
 *
 * 和官方示例 transcode.c 的区别：
 *   1. 只处理第一路视频和第一路音频（和 L15 一样），官方示例处理所有流；
 *   2. 编码器按输出格式的默认编码器选（MP4、MKV 是 libx264 和 AAC），不是“解码器是什么就编什么”；
 *   3. 先配好滤镜图，再按滤镜图出口的参数（尺寸、像素格式、帧率）打开编码器，
 *      所以 vf=scale=…、vf=fps=… 这样会改变画面参数的滤镜可以用；编码器支持的格式交给滤镜图出口去挑；
 *   4. 可以精确裁剪（ss=、t=）、可以对某一类流只拷贝不重新编码（copy=）；
 *   5. 可以用 trace= 看时间戳在读、解码、滤镜、编码、写之间怎么一路换算；
 *   6. 检查每一步的返回值，出错时释放资源并返回非 0 的退出码；输入里的元数据和标志也一并拷贝。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>

/* 一路要处理的流：输入里的一路，对应输出里的一路。 */
typedef struct Stream {
    const char      *name;      /* “视频”或“音频”，打印用 */
    enum AVMediaType type;
    int              copy;      /* 1：流拷贝，不解码不编码 */
    AVStream        *ist, *ost; /* 输入文件里的流、输出文件里的流 */

    AVCodecContext  *dec;       /* 解码器（流拷贝时没有） */
    AVFilterGraph   *graph;     /* 滤镜图：src → 用户的滤镜 → sink */
    AVFilterContext *src, *sink;
    AVCodecContext  *enc;       /* 编码器（流拷贝时没有） */
    AVDictionary    *enc_opts;  /* 交给编码器的选项 */

    AVFrame         *frame;     /* 解码器输出的帧 */
    AVFrame         *fframe;    /* 滤镜图出口的帧 */
    AVPacket        *pkt;       /* 编码器输出的包 */

    int64_t          rescale_last; /* 音频：av_rescale_delta() 记的状态 */
    int              started;   /* 流拷贝：已经遇到可以开始的包 */
    int              past_end;  /* 已经读到 t= 规定的结束时间以后 */

    int64_t          nb_dec;    /* 解码器输出的帧数 */
    int64_t          nb_filt;   /* 滤镜图出口的帧数 */
    int64_t          nb_pkt;    /* 写进文件的包数 */
    int64_t          bytes;     /* 写进文件的包的总字节数 */
} Stream;

typedef struct Ctx {
    AVFormatContext *ic, *oc;
    Stream           v, a;
    int              has_v, has_a;
    double           start;     /* 开始时间，单位秒，已经加上输入文件的起始时间 */
    double           end;       /* start + t=，没有 t= 时是 0 */
    int64_t          trace;
    int              log;
} Ctx;

/* 时间戳转成字符串，AV_NOPTS_VALUE 写成 N/A。 */
static const char *ts2str(char *buf, size_t size, int64_t ts)
{
    if (ts == AV_NOPTS_VALUE)
        snprintf(buf, size, "N/A");
    else
        snprintf(buf, size, "%" PRId64, ts);
    return buf;
}

/*
 * trace=N 时打印时间戳。每个环节都打印“值、时间基、时长”，看数字怎么一路变：
 *   解码输出  帧在输入流的时间基里
 *   滤镜出口  帧在滤镜图出口的时间基里（用户的滤镜可以改时间基）
 *   编码输入  换算到编码器时间基以后
 *   编码输出  包在编码器时间基里，同时给出换算到流时间基以后的值
 */
static void trace_ts(const Ctx *c, const Stream *s, const char *stage, int64_t n,
                     int64_t pts, int64_t duration, AVRational tb)
{
    char a[32];

    if (n >= c->trace)
        return;
    printf("%s %s #%-3" PRId64 " pts %8s  时长 %5" PRId64 "  时间基 %d/%d  = %.6f 秒\n",
           s->name, stage, n, ts2str(a, sizeof(a), pts), duration, tb.num, tb.den,
           pts == AV_NOPTS_VALUE ? 0.0 : pts * av_q2d(tb));
}

/* ------------------------------------------------------------------------ */
/* 写：把一个包交给复用器                                                    */
/* ------------------------------------------------------------------------ */

/*
 * 从编码器取出所有可以取的包，换算时间基，写进文件。
 * 这一段和 L15 的 mux_av.c 相同：编码器的时间基和流的时间基通常不同，包写进去之前要换算。
 * flush 的时候 frame 是 NULL。
 */
static int encode_write(Ctx *c, Stream *s, AVFrame *frame)
{
    int ret;

    if (frame) {
        /* 帧的时间戳现在是滤镜图出口的时间基（frame->time_base），要换成编码器的时间基。 */
        if (frame->pts != AV_NOPTS_VALUE)
            frame->pts = av_rescale_q(frame->pts, frame->time_base, s->enc->time_base);
        /*
         * 时长也要给：没有时长的话编码器输出的包就没有时长（patch 0011 说的 MP4 丢最后一帧）。
         * 音频帧的时长就是采样数（编码器的时间基是 1/采样率）；恒定帧率的视频每帧占一个时间基单位。
         * 都不去换算解码器给的时长：它来自输入容器，容器的时间基很粗（FLV、MKV 是毫秒），
         * 或者根本不可靠（裸码流），换算出来的数每一帧都有几个单位的抖动。
         * 只有可变帧率的视频没有别的办法，才用滤镜出口给的时长。
         */
        if (s->type == AVMEDIA_TYPE_AUDIO)
            frame->duration = frame->nb_samples;
        else if (s->enc->framerate.num > 0)
            frame->duration = 1;
        else
            frame->duration = av_rescale_q(frame->duration, frame->time_base, s->enc->time_base);
        frame->time_base = s->enc->time_base;
        trace_ts(c, s, "编码输入", s->nb_filt - 1, frame->pts, frame->duration,
                 s->enc->time_base);
    }

    ret = avcodec_send_frame(s->enc, frame);
    if (ret < 0) {
        fprintf(stderr, "向%s编码器送帧失败：%s\n", s->name, av_err2str(ret));
        return ret;
    }

    while ((ret = avcodec_receive_packet(s->enc, s->pkt)) >= 0) {
        int64_t pts = s->pkt->pts, dts = s->pkt->dts;

        av_packet_rescale_ts(s->pkt, s->enc->time_base, s->ost->time_base);
        s->pkt->stream_index = s->ost->index;
        if (s->nb_pkt < c->trace) {
            char a[32], b[32], d[32], e[32];

            printf("%s 编码输出 #%-3" PRId64 " pts %8s  dts %8s  时间基 %d/%d  →  流 pts %8s  dts %8s  时间基 %d/%d\n",
                   s->name, s->nb_pkt, ts2str(a, sizeof(a), pts), ts2str(b, sizeof(b), dts),
                   s->enc->time_base.num, s->enc->time_base.den,
                   ts2str(d, sizeof(d), s->pkt->pts), ts2str(e, sizeof(e), s->pkt->dts),
                   s->ost->time_base.num, s->ost->time_base.den);
        }
        if (c->log)
            printf("%s 包 %5" PRId64 "  pts %8" PRId64 "  dts %8" PRId64 "  时长 %5" PRId64 "  %6d 字节%s\n",
                   s->name, s->nb_pkt, s->pkt->pts, s->pkt->dts, s->pkt->duration, s->pkt->size,
                   (s->pkt->flags & AV_PKT_FLAG_KEY) ? "  关键帧" : "");
        s->nb_pkt++;
        s->bytes += s->pkt->size;

        /* 交给复用器以后包里的数据归复用器管，返回时 pkt 已经是空的。 */
        ret = av_interleaved_write_frame(c->oc, s->pkt);
        if (ret < 0) {
            fprintf(stderr, "写%s包失败：%s\n", s->name, av_err2str(ret));
            return ret;
        }
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    fprintf(stderr, "从%s编码器取包失败：%s\n", s->name, av_err2str(ret));
    return ret;
}

/* ------------------------------------------------------------------------ */
/* 滤镜：帧送进滤镜图，从出口取出来，交给编码器                              */
/* ------------------------------------------------------------------------ */

/* frame 是 NULL 表示输入结束，要让滤镜图把里面缓存的帧都吐出来。 */
static int filter_encode_write(Ctx *c, Stream *s, AVFrame *frame)
{
    int ret = av_buffersrc_add_frame_flags(s->src, frame, 0);

    if (ret == AVERROR_EOF) {
        /*
         * 滤镜图不再接收帧了：trim 滤镜已经输出了最后一帧，后面的帧它都不要。这不是错误，
         * 只要把出口里剩下的帧取走就行，也不用再读这一路流的包了。
         */
        s->past_end = 1;
    } else if (ret < 0) {
        fprintf(stderr, "把%s帧送进滤镜图失败：%s\n", s->name, av_err2str(ret));
        return ret;
    }

    while (1) {
        ret = av_buffersink_get_frame(s->sink, s->fframe);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "从%s滤镜图取帧失败：%s\n", s->name, av_err2str(ret));
            return ret;
        }

        /*
         * 滤镜图出口的帧要用出口的时间基换算（L12 说过）。
         * 滤镜图里的 fps 这些滤镜可能改了时间基，所以不能直接拿输入流的时间基当作帧的时间基。
         * 先向出口问一次，写在帧上，后面换算时间戳只看 frame->time_base，不会用错。
         */
        s->fframe->time_base = av_buffersink_get_time_base(s->sink);
        trace_ts(c, s, "滤镜出口", s->nb_filt, s->fframe->pts,
                 s->fframe->duration, s->fframe->time_base);
        s->nb_filt++;
        /*
         * 解码器把关键帧标成 I 帧，如果原样交给编码器，编码器会照着做，每个 I 帧都变成关键帧，
         * 输出的关键帧间隔被输入带偏。标成“没有类型”，让编码器自己决定。
         */
        s->fframe->pict_type = AV_PICTURE_TYPE_NONE;

        ret = encode_write(c, s, s->fframe);
        av_frame_unref(s->fframe);
        if (ret < 0)
            return ret;
    }
}

/*
 * 音频帧的时间戳整理。
 * 容器的时间基很粗的时候（FLV、MKV 是毫秒，TS 是 1/90000），每一帧的 pts 都被取整过，
 * 1024 个采样的帧在 44100 Hz 下是 23.22 毫秒，容器里存的却是 23、23、24、23……
 * 直接换成 1/44100 以后，相邻两帧的间隔是 1014、1015……，不是 1024，编码出来的包时间戳也跟着抖。
 * av_rescale_delta() 利用“每帧正好 nb_samples 个采样”这个已知的时长，在取整的误差范围内把时间戳接成连续的；
 * 时间戳真的出现断档（丢了数据）的时候它会自动改用普通的换算。ffmpeg 命令行也是这样做的。
 * 整理以后帧的时间基是 1/采样率，滤镜图的入口也按这个时间基创建。
 */
static void fix_audio_pts(Stream *s, AVFrame *frame)
{
    AVRational sr_tb = { 1, frame->sample_rate };

    if (frame->pts == AV_NOPTS_VALUE) {
        /* 没有时间戳，接在上一帧后面。 */
        frame->pts = s->rescale_last == AV_NOPTS_VALUE ? 0 : s->rescale_last;
        s->rescale_last = frame->pts + frame->nb_samples;
    } else {
        frame->pts = av_rescale_delta(s->ist->time_base, frame->pts, sr_tb, frame->nb_samples,
                                      &s->rescale_last, sr_tb);
    }
    frame->time_base = sr_tb;
}

/* 把解码器输出的所有帧取出来，送进滤镜图。pkt 是 NULL 表示冲刷解码器。 */
static int decode_packet(Ctx *c, Stream *s, AVPacket *pkt)
{
    int ret = avcodec_send_packet(s->dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "向%s解码器送包失败：%s\n", s->name, av_err2str(ret));
        return ret;
    }

    while ((ret = avcodec_receive_frame(s->dec, s->frame)) >= 0) {
        /* 帧的时间戳统一用 best_effort_timestamp：解码器在 pts 缺失时按 dts 等线索推测出的值。 */
        s->frame->pts = s->frame->best_effort_timestamp;
        trace_ts(c, s, "解码输出", s->nb_dec, s->frame->pts,
                 s->frame->duration, s->ist->time_base);
        if (s->type == AVMEDIA_TYPE_AUDIO) {
            fix_audio_pts(s, s->frame);
            trace_ts(c, s, "整理时间戳", s->nb_dec, s->frame->pts,
                     s->frame->nb_samples, s->frame->time_base);
        }
        s->nb_dec++;

        ret = filter_encode_write(c, s, s->frame);
        av_frame_unref(s->frame);
        if (ret < 0)
            return ret;
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    fprintf(stderr, "从%s解码器取帧失败：%s\n", s->name, av_err2str(ret));
    return ret;
}

/* 一路流读完以后，从上游到下游依次冲刷：解码器 → 滤镜图 → 编码器（L11 说过）。 */
static int flush_stream(Ctx *c, Stream *s)
{
    int ret;

    if (s->copy)
        return 0;
    ret = decode_packet(c, s, NULL);
    if (ret < 0)
        return ret;
    ret = filter_encode_write(c, s, NULL);
    if (ret < 0)
        return ret;
    return encode_write(c, s, NULL);
}

/* ------------------------------------------------------------------------ */
/* 流拷贝                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * 不解码，把包的时间戳换到输出流的时间基以后直接写出去。
 *
 * 流拷贝没法在任意位置切开：视频的帧要靠前面的帧才能解出来，只能从关键帧开始。
 * 主循环开始以前已经定位到了 ss= 之前最近的关键帧，所以这里收到的第一个视频包就是它（L04 的 remux_cut 也是这样）。
 * 时间戳减去的是 ss=，不是这个关键帧的时间：关键帧在 ss= 之前，它和后面几帧的时间戳变成负数，
 * 复用器（MP4）会用编辑列表把这一段“预卷”遮住，播放器从 ss= 开始播，和 ffmpeg 命令行的做法一致。
 * 遮住不等于没有：这一段数据仍然在文件里，文件比想要的大。
 */
static int copy_packet(Ctx *c, Stream *s, AVPacket *pkt)
{
    int64_t offset = av_rescale_q((int64_t)(c->start * AV_TIME_BASE), AV_TIME_BASE_Q, s->ist->time_base);

    /* 视频要等到关键帧才能开始；定位以后第一个包通常就是，万一不是，丢掉直到遇到为止。 */
    if (!s->started) {
        if (s->type == AVMEDIA_TYPE_VIDEO && !(pkt->flags & AV_PKT_FLAG_KEY))
            return 0;
        s->started = 1;
    }

    if (pkt->pts != AV_NOPTS_VALUE)
        pkt->pts -= offset;
    if (pkt->dts != AV_NOPTS_VALUE)
        pkt->dts -= offset;
    av_packet_rescale_ts(pkt, s->ist->time_base, s->ost->time_base);
    pkt->stream_index = s->ost->index;
    pkt->pos = -1;
    if (c->log)
        printf("%s 拷贝 %5" PRId64 "  pts %8" PRId64 "  dts %8" PRId64 "  %6d 字节%s\n",
               s->name, s->nb_pkt, pkt->pts, pkt->dts, pkt->size,
               (pkt->flags & AV_PKT_FLAG_KEY) ? "  关键帧" : "");
    s->nb_pkt++;
    s->bytes += pkt->size;
    return av_interleaved_write_frame(c->oc, pkt);
}

/* ------------------------------------------------------------------------ */
/* 滤镜图的搭建                                                              */
/* ------------------------------------------------------------------------ */

/*
 * 搭一路流的滤镜图：
 *
 *   buffer（入口，参数照着解码器）→ [trim、setpts] → 用户的滤镜 → buffersink（出口，参数照着编码器）
 *
 * 官方示例是先打开编码器再搭滤镜图，把编码器的参数写死在出口上。这里反过来：
 * 出口只告诉滤镜图“编码器接受哪些格式”，格式由滤镜图自己挑（挑最接近输入的，转换最少），
 * 配置好以后再向出口问“你实际给我的是什么”，用这些值去打开编码器。
 */
static int init_filter(Ctx *c, Stream *s, const AVCodec *codec, const char *user_spec)
{
    char args[512], spec[1024], buf[64];
    const AVFilter *buffersrc, *buffersink;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVCodecContext *dec = s->dec;
    const void *configs;
    int nb_configs, ret;

    s->graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !s->graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 裁剪放在滤镜图的最前面：trim/atrim 只留下 [start, end) 里的帧，
     * setpts/asetpts 再把时间戳减去 start，让输出从 0 开始。
     * 这样裁剪的位置是帧（或采样）的时间，不受关键帧的限制。
     * start 和 end 是输入文件里的时间（已经加上文件的起始时间），所以 setpts 减的也是它。
     */
    spec[0] = '\0';
    if (c->start > 0 || c->end > 0) {
        const char *trim = s->type == AVMEDIA_TYPE_VIDEO ? "trim" : "atrim";
        const char *setpts = s->type == AVMEDIA_TYPE_VIDEO ? "setpts" : "asetpts";
        int n = snprintf(spec, sizeof(spec), "%s=start=%.6f", trim, c->start);

        if (c->end > 0)
            n += snprintf(spec + n, sizeof(spec) - n, ":end=%.6f", c->end);
        snprintf(spec + n, sizeof(spec) - n, ",%s=PTS-%.6f/TB", setpts, c->start);
        if (user_spec && *user_spec)
            av_strlcatf(spec, sizeof(spec), ",%s", user_spec);
    } else if (user_spec && *user_spec) {
        snprintf(spec, sizeof(spec), "%s", user_spec);
    } else {
        snprintf(spec, sizeof(spec), "%s", s->type == AVMEDIA_TYPE_VIDEO ? "null" : "anull");
    }

    if (s->type == AVMEDIA_TYPE_VIDEO) {
        AVRational fr = av_guess_frame_rate(c->ic, s->ist, NULL);

        buffersrc  = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        /* 入口的参数照着解码器：尺寸、像素格式、时间基，还有 L13 说的色彩属性。 */
        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:frame_rate=%d/%d:"
                 "colorspace=%d:range=%d:alpha_mode=%d:chroma_location=%d",
                 dec->width, dec->height, dec->pix_fmt,
                 dec->pkt_timebase.num, dec->pkt_timebase.den,
                 dec->sample_aspect_ratio.num, dec->sample_aspect_ratio.den,
                 fr.num, fr.den,
                 dec->colorspace, dec->color_range, dec->alpha_mode, dec->chroma_sample_location);
    } else {
        buffersrc  = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        /* 有的文件没有声道布局，只知道声道数，按声道数给一个默认的布局。 */
        if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&dec->ch_layout, dec->ch_layout.nb_channels);
        av_channel_layout_describe(&dec->ch_layout, buf, sizeof(buf));
        /* 时间基是 1/采样率，因为送进来的帧要先过 fix_audio_pts()。 */
        snprintf(args, sizeof(args), "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                 dec->sample_rate, dec->sample_rate, av_get_sample_fmt_name(dec->sample_fmt), buf);
    }
    if (!buffersrc || !buffersink) {
        ret = AVERROR_FILTER_NOT_FOUND;
        goto end;
    }

    ret = avfilter_graph_create_filter(&s->src, buffersrc, "in", args, NULL, s->graph);
    if (ret < 0) {
        fprintf(stderr, "创建%s滤镜图的入口失败（%s）：%s\n", s->name, args, av_err2str(ret));
        goto end;
    }

    /* 出口：先分配，设置编码器接受的格式，再初始化。设置要在初始化之前。 */
    s->sink = avfilter_graph_alloc_filter(s->graph, buffersink, "out");
    if (!s->sink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (s->type == AVMEDIA_TYPE_VIDEO) {
        ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &nb_configs);
        if (ret >= 0 && configs)    /* configs 是 NULL 表示编码器对这一项没有限制 */
            ret = av_opt_set_array(s->sink, "pixel_formats", AV_OPT_SEARCH_CHILDREN, 0, nb_configs,
                                   AV_OPT_TYPE_PIXEL_FMT, configs);
    } else {
        ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &nb_configs);
        if (ret >= 0 && configs)
            ret = av_opt_set_array(s->sink, "sample_formats", AV_OPT_SEARCH_CHILDREN, 0, nb_configs,
                                   AV_OPT_TYPE_SAMPLE_FMT, configs);
        if (ret >= 0)
            ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &configs, &nb_configs);
        if (ret >= 0 && configs)
            ret = av_opt_set_array(s->sink, "samplerates", AV_OPT_SEARCH_CHILDREN, 0, nb_configs,
                                   AV_OPT_TYPE_INT, configs);
        if (ret >= 0)
            ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, &configs, &nb_configs);
        if (ret >= 0 && configs)
            ret = av_opt_set_array(s->sink, "channel_layouts", AV_OPT_SEARCH_CHILDREN, 0, nb_configs,
                                   AV_OPT_TYPE_CHLAYOUT, configs);
    }
    if (ret < 0) {
        fprintf(stderr, "设置%s滤镜图出口的格式失败：%s\n", s->name, av_err2str(ret));
        goto end;
    }
    ret = avfilter_init_dict(s->sink, NULL);
    if (ret < 0)
        goto end;

    /*
     * 把入口、出口接到用户写的滤镜图两头。名字是 in 和 out：
     * 对解析器来说，“in”是滤镜图的输入端，所以它接在入口滤镜的输出上，反之亦然。
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = s->src;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = s->sink;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avfilter_graph_parse_ptr(s->graph, spec, &inputs, &outputs, NULL);
    if (ret < 0) {
        fprintf(stderr, "解析%s滤镜图“%s”失败：%s\n", s->name, spec, av_err2str(ret));
        goto end;
    }
    ret = avfilter_graph_config(s->graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "配置%s滤镜图“%s”失败：%s\n", s->name, spec, av_err2str(ret));
        goto end;
    }
    printf("%s滤镜图：%s\n", s->name, spec);

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* 解码器、编码器和输出流                                                    */
/* ------------------------------------------------------------------------ */

static int open_decoder(Stream *s)
{
    const AVCodec *codec = avcodec_find_decoder(s->ist->codecpar->codec_id);
    int ret;

    if (!codec) {
        fprintf(stderr, "找不到%s解码器\n", s->name);
        return AVERROR_DECODER_NOT_FOUND;
    }
    s->dec = avcodec_alloc_context3(codec);
    if (!s->dec)
        return AVERROR(ENOMEM);
    ret = avcodec_parameters_to_context(s->dec, s->ist->codecpar);
    if (ret < 0)
        return ret;
    /* 解码器输出的帧和滤镜入口都要知道包的时间基，帧的时间戳就是以它为单位的。 */
    s->dec->pkt_timebase = s->ist->time_base;
    ret = avcodec_open2(s->dec, codec, NULL);
    if (ret < 0)
        fprintf(stderr, "打不开%s解码器 %s：%s\n", s->name, codec->name, av_err2str(ret));
    return ret;
}

/*
 * 滤镜图配好以后，向出口问它实际输出的是什么，用来设置编码器。
 * 色彩属性里的范围和矩阵系数也问出口，因为滤镜可能改了它们；原色和传输特性滤镜图出口不提供，
 * 照着解码器抄（官方示例的 patch 0074 就是漏了这一步，输出的文件不知道自己的颜色怎么解释）。
 */
static int open_encoder(Ctx *c, Stream *s, const AVCodec *codec)
{
    AVCodecContext *enc, *dec = s->dec;
    int ret;

    enc = avcodec_alloc_context3(codec);
    if (!enc)
        return AVERROR(ENOMEM);
    s->enc = enc;

    if (s->type == AVMEDIA_TYPE_VIDEO) {
        AVRational fr = av_buffersink_get_frame_rate(s->sink);

        enc->width                 = av_buffersink_get_w(s->sink);
        enc->height                = av_buffersink_get_h(s->sink);
        enc->pix_fmt               = av_buffersink_get_format(s->sink);
        enc->sample_aspect_ratio   = av_buffersink_get_sample_aspect_ratio(s->sink);
        enc->colorspace            = av_buffersink_get_colorspace(s->sink);
        enc->color_range           = av_buffersink_get_color_range(s->sink);
        enc->chroma_sample_location = av_buffersink_get_chroma_location(s->sink);
        enc->color_primaries       = dec->color_primaries;
        enc->color_trc             = dec->color_trc;
        /*
         * 有帧率就让编码器的时间基是帧率的倒数（每一帧的 pts 差 1），没有（可变帧率）就用出口的时间基。
         * 官方示例总是用 av_guess_frame_rate() 的倒数，可变帧率的输入会被取整到那个网格上。
         */
        if (fr.num > 0 && fr.den > 0) {
            enc->framerate = fr;
            enc->time_base = av_inv_q(fr);
        } else {
            enc->time_base = av_buffersink_get_time_base(s->sink);
        }
    } else {
        enc->sample_rate = av_buffersink_get_sample_rate(s->sink);
        enc->sample_fmt  = av_buffersink_get_format(s->sink);
        ret = av_buffersink_get_ch_layout(s->sink, &enc->ch_layout);
        if (ret < 0)
            return ret;
        enc->time_base = (AVRational){ 1, enc->sample_rate };
    }

    /* 复用器要求全局头（MP4、MKV）的时候，要在打开编码器之前告诉它（L15）。 */
    if (c->oc->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* 送给编码器的帧带着时长，编码器就把时长带到它输出的包上。 */
    enc->flags |= AV_CODEC_FLAG_FRAME_DURATION;
    /* 和 L13、L14、L15 一样默认单线程，保证每台机器上结果相同；v:threads=N 可以改。 */
    enc->thread_count = 1;

    ret = avcodec_open2(enc, codec, &s->enc_opts);
    if (ret < 0) {
        fprintf(stderr, "打不开%s编码器 %s：%s\n", s->name, codec->name, av_err2str(ret));
        return ret;
    }
    if (av_dict_count(s->enc_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(s->enc_opts, NULL);

        fprintf(stderr, "%s编码器不认识选项 %s\n", s->name, e->key);
        return AVERROR(EINVAL);
    }

    /*
     * 音频编码器要求每一帧的采样数固定（AAC 是 1024），打开以后才知道，这时再告诉滤镜图出口，
     * 出口就会把帧重新切成这个长度。可以在滤镜图配置好以后设置。
     */
    if (s->type == AVMEDIA_TYPE_AUDIO && enc->frame_size > 0)
        av_buffersink_set_frame_size(s->sink, enc->frame_size);
    return 0;
}

/* 加一路输出流，复制元数据和标志。encoder 是 NULL 表示流拷贝。 */
static int add_stream(Ctx *c, Stream *s, const AVCodec *codec)
{
    int ret;

    s->ost = avformat_new_stream(c->oc, NULL);
    if (!s->ost)
        return AVERROR(ENOMEM);

    if (s->copy) {
        ret = avcodec_parameters_copy(s->ost->codecpar, s->ist->codecpar);
        if (ret < 0)
            return ret;
        /* 输入里的 codec_tag 是输入容器的编号，输出容器不一定认，清掉让复用器自己选（L04）。 */
        s->ost->codecpar->codec_tag = 0;
        s->ost->time_base = s->ist->time_base;
        s->ost->sample_aspect_ratio = s->ist->sample_aspect_ratio;
    } else {
        ret = avcodec_parameters_from_context(s->ost->codecpar, s->enc);
        if (ret < 0)
            return ret;
        /* 这只是向复用器“建议”的时间基，avformat_write_header() 以后以流里的值为准。 */
        s->ost->time_base = s->enc->time_base;
        s->ost->sample_aspect_ratio = s->enc->sample_aspect_ratio;
    }

    /* 流的元数据（语言、handler 名）和标志（default、forced）一起带过去，官方示例没有做（patch 0075）。 */
    ret = av_dict_copy(&s->ost->metadata, s->ist->metadata, 0);
    if (ret < 0)
        return ret;
    s->ost->disposition = s->ist->disposition;
    if (!s->copy) {
        /* 重新编码的流，把“是谁编码的”换成我们用的编码器，和 ffmpeg 命令行一致（L15）。 */
        char ident[64];

        snprintf(ident, sizeof(ident), "%s %s", LIBAVCODEC_IDENT, codec->name);
        ret = av_dict_set(&s->ost->metadata, "encoder", ident, 0);
    }
    return ret;
}

/* 选编码器：用户用 vcodec=、acodec= 指定了就用指定的，否则用输出格式的默认编码器。 */
static const AVCodec *pick_encoder(const char *name, enum AVCodecID default_id, const char *kind, const char *opt)
{
    const AVCodec *codec;

    if (name) {
        codec = avcodec_find_encoder_by_name(name);
        if (!codec)
            fprintf(stderr, "找不到%s编码器 %s\n", kind, name);
        return codec;
    }
    codec = avcodec_find_encoder(default_id);
    if (!codec)
        fprintf(stderr, "这个构建里没有 %s 的%s编码器，可以用 %s= 换一个\n",
                avcodec_get_name(default_id), kind, opt);
    return codec;
}

static void free_stream(Stream *s)
{
    avcodec_free_context(&s->dec);
    avcodec_free_context(&s->enc);
    avfilter_graph_free(&s->graph);
    av_dict_free(&s->enc_opts);
    av_frame_free(&s->frame);
    av_frame_free(&s->fframe);
    av_packet_free(&s->pkt);
}

static void print_stream(const Stream *s)
{
    printf("%s：", s->name);
    if (s->copy)
        printf("流拷贝（%s），", avcodec_get_name(s->ost->codecpar->codec_id));
    else
        printf("%s → %s，", s->dec->codec->name, s->enc->codec->name);
    printf("输入流时间基 %d/%d", s->ist->time_base.num, s->ist->time_base.den);
    if (!s->copy)
        printf("，编码器时间基 %d/%d", s->enc->time_base.num, s->enc->time_base.den);
    printf("，输出流时间基 %d/%d\n", s->ost->time_base.num, s->ost->time_base.den);
}

static void print_summary(const Stream *s)
{
    if (s->copy)
        printf("%s：拷贝 %" PRId64 " 个包，%" PRId64 " 字节\n", s->name, s->nb_pkt, s->bytes);
    else
        printf("%s：解码 %" PRId64 " 帧，滤镜输出 %" PRId64 " 帧，写出 %" PRId64 " 个包，%" PRId64 " 字节\n",
               s->name, s->nb_dec, s->nb_filt, s->nb_pkt, s->bytes);
}

/* 读一个包，交给它所属的那一路流处理。 */
static int process_packet(Ctx *c, AVPacket *pkt)
{
    Stream *s;

    if (c->has_v && pkt->stream_index == c->v.ist->index)
        s = &c->v;
    else if (c->has_a && pkt->stream_index == c->a.ist->index)
        s = &c->a;
    else
        return 0;               /* 不处理的流，丢掉 */

    /*
     * t= 规定了结束时间。dts 单调增加，后面所有包的 dts 都不小于这一个，帧的 pts 又不会小于它的包的 dts，
     * 所以 dts 到了结束时间以后的包解出来的帧都在结束时间之外，不用再处理。
     * 用 dts 而不是 pts 判断，是因为视频有 B 帧时 pts 的顺序和包的顺序不一样（L08）：
     * 只丢掉“一段后缀”，不会丢了被后面的帧引用的包。
     */
    if (c->end > 0) {
        int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

        if (ts != AV_NOPTS_VALUE) {
            if (ts * av_q2d(s->ist->time_base) >= c->end) {
                s->past_end = 1;
                return 0;
            }
        }
    }

    if (s->copy)
        return copy_packet(c, s, pkt);
    return decode_packet(c, s, pkt);
}

int main(int argc, char **argv)
{
    const char *in_name, *out_name;
    const char *vf = NULL, *af = NULL, *vcodec_name = NULL, *acodec_name = NULL, *format_name = NULL;
    const char *copy = "";
    AVDictionary *mux_opts = NULL;
    AVPacket *pkt = NULL;
    Ctx c = { 0 };
    double ss = 0, t = 0;
    int vi, ai, header_written = 0, ret = 0;

    if (argc < 3) {
        fprintf(stderr, "用法：%s <输入文件> <输出文件> [名字=值 ...]\n", argv[0]);
        return 1;
    }
    in_name  = argv[1];
    out_name = argv[2];

    for (int i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (!eq || eq == argv[i]) {
            fprintf(stderr, "选项要写成“名字=值”：%s\n", argv[i]);
            ret = AVERROR(EINVAL);
            goto end;
        }
        *eq = '\0';
        ret = 0;                /* 只有交给字典的几个分支会改它 */
        if (!strcmp(argv[i], "ss"))
            ss = atof(eq + 1);
        else if (!strcmp(argv[i], "t"))
            t = atof(eq + 1);
        else if (!strcmp(argv[i], "vf"))
            vf = eq + 1;
        else if (!strcmp(argv[i], "af"))
            af = eq + 1;
        else if (!strcmp(argv[i], "vcodec"))
            vcodec_name = eq + 1;
        else if (!strcmp(argv[i], "acodec"))
            acodec_name = eq + 1;
        else if (!strcmp(argv[i], "copy"))
            copy = eq + 1;
        else if (!strcmp(argv[i], "format"))
            format_name = eq + 1;
        else if (!strcmp(argv[i], "trace"))
            c.trace = atoll(eq + 1);
        else if (!strcmp(argv[i], "log"))
            c.log = atoi(eq + 1);
        else if (!strncmp(argv[i], "v:", 2))
            ret = av_dict_set(&c.v.enc_opts, argv[i] + 2, eq + 1, 0);
        else if (!strncmp(argv[i], "a:", 2))
            ret = av_dict_set(&c.a.enc_opts, argv[i] + 2, eq + 1, 0);
        else
            ret = av_dict_set(&mux_opts, argv[i], eq + 1, 0);
        if (ret < 0)
            goto end;
    }
    if (ss < 0 || t < 0) {
        fprintf(stderr, "ss= 和 t= 不能是负数\n");
        ret = AVERROR(EINVAL);
        goto end;
    }
    /* 流拷贝不解码，滤镜没有地方用。静悄悄地忽略 vf=、af= 会让人以为已经生效，所以直接报错。 */
    if ((vf && strchr(copy, 'v')) || (af && strchr(copy, 'a'))) {
        fprintf(stderr, "流拷贝（copy=）的那一路不能用滤镜，请去掉 vf= 或 af=，或者不要拷贝这一路\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* ---------- 打开输入，找到第一路视频和第一路音频 ---------- */
    ret = avformat_open_input(&c.ic, in_name, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开输入文件 %s：%s\n", in_name, av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(c.ic, NULL);
    if (ret < 0) {
        fprintf(stderr, "找不到流信息：%s\n", av_err2str(ret));
        goto end;
    }
    vi = av_find_best_stream(c.ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    ai = av_find_best_stream(c.ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vi < 0 && ai < 0) {
        fprintf(stderr, "输入里没有视频也没有音频\n");
        ret = AVERROR_STREAM_NOT_FOUND;
        goto end;
    }
    c.has_v = vi >= 0;
    c.has_a = ai >= 0;

    /*
     * ss=、t= 是相对于输入文件开头的，但有的文件（比如 TS）的时间戳不是从 0 开始，
     * 所以要加上文件的起始时间，才是流里时间戳的值。
     */
    {
        double base = c.ic->start_time != AV_NOPTS_VALUE ? c.ic->start_time / (double)AV_TIME_BASE : 0;

        /* 即使没有 ss=，只要文件不是从 0 开始，start 也不是 0，滤镜图里会把时间戳整体平移，输出才从 0 开始。 */
        c.start = base + ss;
        c.end   = t > 0 ? c.start + t : 0;
    }

    /* ---------- 输出上下文 ---------- */
    ret = avformat_alloc_output_context2(&c.oc, NULL, format_name, out_name);
    if (!c.oc) {
        fprintf(stderr, "不知道输出文件 %s 是什么格式，请用 format= 指定\n", out_name);
        ret = ret < 0 ? ret : AVERROR(EINVAL);
        goto end;
    }
    printf("输出格式：%s（%s）\n", c.oc->oformat->name, c.oc->oformat->long_name);

    /* ---------- 每一路流：解码器、滤镜图、编码器、输出流 ---------- */
    for (int k = 0; k < 2; k++) {
        Stream *s = k == 0 ? &c.v : &c.a;
        int idx = k == 0 ? vi : ai;
        const AVCodec *codec = NULL;

        if (idx < 0)
            continue;
        s->name = k == 0 ? "视频" : "音频";
        s->type = k == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
        s->ist  = c.ic->streams[idx];
        s->copy = strchr(copy, k == 0 ? 'v' : 'a') != NULL;
        s->rescale_last = AV_NOPTS_VALUE;
        s->frame  = av_frame_alloc();
        s->fframe = av_frame_alloc();
        s->pkt    = av_packet_alloc();
        if (!s->frame || !s->fframe || !s->pkt) {
            ret = AVERROR(ENOMEM);
            goto end;
        }

        if (!s->copy) {
            const char *name = k == 0 ? vcodec_name : acodec_name;
            enum AVCodecID default_id = k == 0 ? c.oc->oformat->video_codec : c.oc->oformat->audio_codec;

            /* 输出格式放不下这一类流（GIF 没有声音，WAV 没有画面），丢掉这一路，ffmpeg 命令行也是这样。 */
            if (!name && default_id == AV_CODEC_ID_NONE) {
                printf("输出格式 %s 不能放%s，丢掉输入里的%s\n", c.oc->oformat->name, s->name, s->name);
                if (k == 0)
                    c.has_v = 0;
                else
                    c.has_a = 0;
                continue;
            }
            codec = pick_encoder(name, default_id, s->name, k == 0 ? "vcodec" : "acodec");
            if (!codec) {
                ret = AVERROR_ENCODER_NOT_FOUND;
                goto end;
            }
            if ((ret = open_decoder(s)) < 0 ||
                (ret = init_filter(&c, s, codec, k == 0 ? vf : af)) < 0 ||
                (ret = open_encoder(&c, s, codec)) < 0)
                goto end;
        }
        if ((ret = add_stream(&c, s, codec)) < 0)
            goto end;
    }

    if (!c.has_v && !c.has_a) {
        fprintf(stderr, "输出格式 %s 放不下输入里的任何一路流\n", c.oc->oformat->name);
        ret = AVERROR(EINVAL);
        goto end;
    }

    /*
     * 输入文件的元数据（标题等）也带过去。但 MP4 的文件类型标签（major_brand 这三个）只描述输入文件自己：
     * 输出是 MP4 时复用器自己写，输出是别的格式时它们没有意义，ffmpeg 命令行也不拷贝。
     */
    ret = av_dict_copy(&c.oc->metadata, c.ic->metadata, 0);
    if (ret < 0)
        goto end;
    av_dict_set(&c.oc->metadata, "major_brand", NULL, 0);
    av_dict_set(&c.oc->metadata, "minor_version", NULL, 0);
    av_dict_set(&c.oc->metadata, "compatible_brands", NULL, 0);

    /* ---------- 打开输出文件，写文件头 ---------- */
    av_dump_format(c.oc, 0, out_name, 1);
    if (!(c.oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&c.oc->pb, out_name, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "打不开输出文件 %s：%s\n", out_name, av_err2str(ret));
            goto end;
        }
    }
    ret = avformat_write_header(c.oc, &mux_opts);
    if (ret < 0) {
        fprintf(stderr, "写文件头失败：%s\n", av_err2str(ret));
        goto end;
    }
    header_written = 1;
    if (av_dict_count(mux_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(mux_opts, NULL);

        fprintf(stderr, "复用器不认识选项 %s\n", e->key);
        ret = AVERROR(EINVAL);
        goto end;
    }
    if (c.has_v)
        print_stream(&c.v);
    if (c.has_a)
        print_stream(&c.a);

    /* ---------- 定位到 ss= ---------- */
    if (ss > 0) {
        /*
         * 先跳到 ss= 之前最近的关键帧，从那里开始解码，解出来的帧里 ss= 之前的那些由 trim 滤镜丢掉。
         * 这就是“精确裁剪要多解码一段”的代价：最多多解码一个 GOP。
         */
        int64_t target = (int64_t)(c.start * AV_TIME_BASE);

        ret = avformat_seek_file(c.ic, -1, INT64_MIN, target, target, 0);
        if (ret < 0) {
            fprintf(stderr, "定位到 %.3f 秒失败：%s\n", c.start, av_err2str(ret));
            goto end;
        }
    }

    /* ---------- 主循环：读包、处理 ---------- */
    pkt = av_packet_alloc();
    if (!pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    while ((ret = av_read_frame(c.ic, pkt)) >= 0) {
        int r = process_packet(&c, pkt);

        av_packet_unref(pkt);
        if (r < 0) {
            ret = r;
            goto end;
        }
        /* 所有处理的流都过了结束时间，不用再读了。 */
        if ((!c.has_v || c.v.past_end) && (!c.has_a || c.a.past_end))
            break;
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "读输入失败：%s\n", av_err2str(ret));
        goto end;
    }

    /* ---------- 冲刷 ---------- */
    if (c.has_v && (ret = flush_stream(&c, &c.v)) < 0)
        goto end;
    if (c.has_a && (ret = flush_stream(&c, &c.a)) < 0)
        goto end;

    if (c.has_v)
        print_summary(&c.v);
    if (c.has_a)
        print_summary(&c.a);
    /* 某一路一个包也没有写出来：多半是 ss= 超过了输入的长度，或者 t= 太短。输出是个空壳，要报错。 */
    ret = 0;
    for (int k = 0; k < 2; k++) {
        const Stream *s = k == 0 ? &c.v : &c.a;

        if ((k == 0 ? c.has_v : c.has_a) && s->nb_pkt == 0) {
            fprintf(stderr, "%s一个包也没有写出来，ss= 是不是超过了输入的长度，或者 t= 太短？\n", s->name);
            ret = AVERROR(EINVAL);
        }
    }

end:
    /*
     * 文件尾和关闭文件的结果都要检查（patch 0072）：MP4 的索引在文件尾里写，
     * 写不进去的话文件是打不开的，不能让程序说“成功”。
     * 只有文件头已经写成功才能写文件尾。
     */
    if (header_written) {
        int r = av_write_trailer(c.oc);

        if (r < 0) {
            fprintf(stderr, "写文件尾失败：%s\n", av_err2str(r));
            if (ret >= 0)
                ret = r;
        }
    }
    if (c.oc && !(c.oc->oformat->flags & AVFMT_NOFILE)) {
        int r = avio_closep(&c.oc->pb);

        if (r < 0) {
            fprintf(stderr, "关闭输出文件失败：%s\n", av_err2str(r));
            if (ret >= 0)
                ret = r;
        }
    }
    av_packet_free(&pkt);
    free_stream(&c.v);
    free_stream(&c.a);
    av_dict_free(&mux_opts);
    avformat_free_context(c.oc);
    avformat_close_input(&c.ic);

    if (ret < 0) {
        fprintf(stderr, "出错：%s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}
