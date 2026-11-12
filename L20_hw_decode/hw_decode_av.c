/*
 * L20 硬件解码：自己写的版本，和官方 hw_decode.c 的区别是：能自动选设备、没有显卡时退回软件解码、
 * 能指定输出格式、会统计耗时。可以拿去做自己的程序的起点。
 *
 * 用法：hw_decode_av <输入文件> <输出文件|-> [选项]
 *   输出文件写 - 表示不写文件（只解码；配合 md5=1 看内容，配合 stats=1 量速度）。
 *
 * 选项（名字=值）：
 *   device=auto      auto：按解码器的硬件配置表逐个试，哪个创建得出设备就用哪个，都不行就用软件解码；
 *                    none：不用硬件，纯软件解码；
 *                    vaapi、cuda、qsv……：只试这一种
 *   device_path=路径   传给 av_hwdevice_ctx_create() 的设备名，例如 VAAPI 的 /dev/dri/renderD129（有多块显卡时）；不写用默认设备
 *   fallback=0       指定了设备类型但用不了时：1 退回软件解码，0（默认）报错退出。device=auto 总是退回软件
 *   pix_fmt=nv12     输出的像素格式。硬件帧能直接传成这种格式就直接传，否则传回内存以后用 libswscale 转；不写就保持解码器给的格式
 *   frames=N         只解 N 帧；0（默认）解完
 *   threads=N        软件解码的线程数；0（默认）让解码器自己定
 *   extra=N          给硬件帧池多留 N 个帧（AVCodecContext.extra_hw_frames），帧要在解码器外面放一会儿的程序才需要
 *   md5=1            打印输出的全部字节的 MD5（两种解码方式的结果能不能逐字节一样，一眼看出来）
 *   stats=1          结束时打印帧数、耗时、CPU 时间、搬运耗时
 *   loglevel=info    日志级别：quiet、error、warning、info、verbose、debug。创建设备失败的原因要 verbose 才看得到
 *
 * 退出码：0 成功；1 失败（用法错误、打不开输入、设备用不了又不许退回、解码或写文件出错）。
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

typedef struct Options {
    const char        *input, *output;
    const char        *device;          /* "auto"、"none" 或设备类型名 */
    const char        *device_path;     /* NULL：默认设备 */
    int                fallback;
    enum AVPixelFormat pix_fmt;         /* AV_PIX_FMT_NONE：不指定 */
    long               frames;
    int                threads, extra, md5, stats, loglevel;
} Options;

typedef struct Decoder {
    AVFormatContext   *fmt;
    AVCodecContext    *ctx;
    const AVCodec     *codec;
    int                stream;
    AVBufferRef       *hw_device;
    enum AVPixelFormat hw_pix_fmt;      /* 显存里的帧的格式；AV_PIX_FMT_NONE：不用硬件 */
    const char        *hw_name;         /* 实际用的设备类型名，软件解码是 NULL */
    int                fell_back;       /* get_format 里找不到硬件格式，退回了软件格式 */
} Decoder;

typedef struct Sink {
    FILE              *file;
    struct AVMD5      *md5;
    struct SwsContext *sws;
    AVFrame           *sw, *cvt;
    uint8_t           *buf;
    unsigned           buf_cap;
    enum AVPixelFormat transfer_fmt;    /* 硬件帧直接传成什么格式；NONE：让 FFmpeg 选第一种 */
    int                transfer_ready;
    long               nb_frames, nb_hw, nb_sw;
    int64_t            transfer_us, convert_us;
    long long          bytes;
} Sink;

/* ---- 小工具 ---------------------------------------------------------------- */

static int64_t now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

/* 往 buf 末尾追加一段文字，放不下的部分丢掉。 */
static void append(char *buf, size_t size, const char *fmt, ...)
{
    size_t len = strlen(buf);
    va_list ap;

    if (len + 1 >= size)
        return;
    va_start(ap, fmt);
    vsnprintf(buf + len, size - len, fmt, ap);
    va_end(ap);
}

static int parse_loglevel(const char *s)
{
    static const struct { const char *name; int level; } t[] = {
        { "quiet", AV_LOG_QUIET }, { "error", AV_LOG_ERROR }, { "warning", AV_LOG_WARNING },
        { "info", AV_LOG_INFO }, { "verbose", AV_LOG_VERBOSE }, { "debug", AV_LOG_DEBUG },
    };

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(s, t[i].name))
            return t[i].level;
    return -1000;
}

static int parse_options(int argc, char **argv, Options *o)
{
    o->device    = "auto";
    o->pix_fmt   = AV_PIX_FMT_NONE;
    o->loglevel  = AV_LOG_INFO;
    if (argc < 3)
        return -1;
    o->input  = argv[1];
    o->output = argv[2];
    for (int i = 3; i < argc; i++) {
        const char *a = argv[i];

        if (!strncmp(a, "device=", 7))
            o->device = a + 7;
        else if (!strncmp(a, "device_path=", 12))
            o->device_path = a + 12;
        else if (!strncmp(a, "fallback=", 9))
            o->fallback = atoi(a + 9);
        else if (!strncmp(a, "pix_fmt=", 8)) {
            o->pix_fmt = av_get_pix_fmt(a + 8);
            if (o->pix_fmt == AV_PIX_FMT_NONE) {
                fprintf(stderr, "不认识的像素格式 %s（ffmpeg -pix_fmts 能列出全部）\n", a + 8);
                return -1;
            }
        } else if (!strncmp(a, "frames=", 7))
            o->frames = atol(a + 7);
        else if (!strncmp(a, "threads=", 8))
            o->threads = atoi(a + 8);
        else if (!strncmp(a, "extra=", 6))
            o->extra = atoi(a + 6);
        else if (!strncmp(a, "md5=", 4))
            o->md5 = atoi(a + 4);
        else if (!strncmp(a, "stats=", 6))
            o->stats = atoi(a + 6);
        else if (!strncmp(a, "loglevel=", 9)) {
            o->loglevel = parse_loglevel(a + 9);
            if (o->loglevel == -1000) {
                fprintf(stderr, "不认识的日志级别 %s\n", a + 9);
                return -1;
            }
        } else {
            fprintf(stderr, "不认识的选项 %s\n", a);
            return -1;
        }
    }
    if (o->frames < 0 || o->threads < 0 || o->extra < 0)
        return -1;
    return 0;
}

/* ---- 选设备 ---------------------------------------------------------------- */

/*
 * 解码器的第 index 项硬件配置里，找一项“给一个设备上下文就能用”的（HW_DEVICE_CTX）。
 * type 不是 NONE 时只找这种设备类型。返回配置，没有返回 NULL；*next 是下一次继续找的序号。
 */
static const AVCodecHWConfig *next_config(const AVCodec *codec, enum AVHWDeviceType type, int *next)
{
    for (;;) {
        const AVCodecHWConfig *c = avcodec_get_hw_config(codec, (*next)++);

        if (!c)
            return NULL;
        if ((c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
            (type == AV_HWDEVICE_TYPE_NONE || c->device_type == type))
            return c;
    }
}

/*
 * 按 o->device 选一个硬件设备，成功时 d->hw_device、d->hw_pix_fmt、d->hw_name 有值，返回 0。
 * 失败返回负数，why 里写着每一种试过的设备失败的原因。device=none 不算失败，d->hw_name 保持 NULL，返回 0。
 */
static int open_hw_device(Decoder *d, const Options *o, char *why, size_t why_size)
{
    enum AVHWDeviceType want = AV_HWDEVICE_TYPE_NONE;
    const AVCodecHWConfig *c;
    int next = 0, tried = 0, last = AVERROR(ENOSYS);

    if (!strcmp(o->device, "none"))
        return 0;
    if (strcmp(o->device, "auto")) {
        want = av_hwdevice_find_type_by_name(o->device);
        if (want == AV_HWDEVICE_TYPE_NONE) {
            enum AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;

            append(why, why_size, "这份 FFmpeg 不认识设备类型 %s（有：", o->device);
            while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE)
                append(why, why_size, " %s", av_hwdevice_get_type_name(t));
            append(why, why_size, "）");
            return AVERROR(EINVAL);
        }
    }

    while ((c = next_config(d->codec, want, &next))) {
        AVBufferRef *dev = NULL;
        int ret;

        tried++;
        ret = av_hwdevice_ctx_create(&dev, c->device_type, o->device_path, NULL, 0);
        if (ret < 0) {
            append(why, why_size, "%s%s：%s", tried > 1 ? "；" : "", av_hwdevice_get_type_name(c->device_type),
                   av_err2str(ret));
            last = ret;
            continue;
        }
        d->hw_device  = dev;
        d->hw_pix_fmt = c->pix_fmt;
        d->hw_name    = av_hwdevice_get_type_name(c->device_type);
        return 0;
    }
    if (!tried)
        append(why, why_size, "解码器 %s 没有%s的硬件配置", d->codec->name,
               want == AV_HWDEVICE_TYPE_NONE ? "任何" : o->device);
    return last;
}

/*
 * get_format 回调：解码器给一个像素格式列表，硬件格式（如果显卡能解这个视频）在前，软件格式在后。
 * 有硬件格式就选它。没有（显卡不支持这个视频的 profile、位深或者尺寸）就退回软件格式，让 CPU 解，
 * 并记下来，免得统计的时候以为一直是硬件在解。官方示例这里返回 NONE，解码就失败了。
 */
static enum AVPixelFormat get_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    Decoder *d = ctx->opaque;

    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == d->hw_pix_fmt)
            return *p;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);

        if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
            fprintf(stderr, "警告：显卡解不了这个视频（解码器没有给出硬件格式 %s），改用软件格式 %s\n",
                    av_get_pix_fmt_name(d->hw_pix_fmt), av_get_pix_fmt_name(*p));
            d->fell_back = 1;
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

/* ---- 输出 ------------------------------------------------------------------ */

/* 第一次拿到硬件帧时：问它能传成哪些内存格式，决定怎么传。 */
static int setup_transfer(Sink *s, const Options *o, const AVFrame *frame)
{
    enum AVPixelFormat *fmts = NULL;
    int ret = av_hwframe_transfer_get_formats(frame->hw_frames_ctx, AV_HWFRAME_TRANSFER_DIRECTION_FROM, &fmts, 0);

    if (ret < 0)
        return ret;
    fprintf(stderr, "硬件帧 %s %dx%d，可以直接传回内存的格式：", av_get_pix_fmt_name(frame->format),
            frame->width, frame->height);
    s->transfer_fmt = AV_PIX_FMT_NONE;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        fprintf(stderr, " %s", av_get_pix_fmt_name(*p));
        if (*p == o->pix_fmt)
            s->transfer_fmt = *p;
    }
    if (s->transfer_fmt != AV_PIX_FMT_NONE)
        fprintf(stderr, "；要的 %s 能直接传\n", av_get_pix_fmt_name(s->transfer_fmt));
    else
        fprintf(stderr, "；不指定格式时传成第一种（%s）\n", fmts[0] != AV_PIX_FMT_NONE ? av_get_pix_fmt_name(fmts[0]) : "?");
    s->transfer_ready = 1;
    av_freep(&fmts);
    return 0;
}

/* 处理一帧：硬件帧先搬回内存，需要的话转格式，然后写文件、算 MD5。 */
static int sink_frame(Sink *s, const Options *o, AVFrame *frame)
{
    AVFrame *out = frame;
    int size, ret;

    if (frame->hw_frames_ctx) {
        int64_t t0;

        s->nb_hw++;
        if (!s->transfer_ready && (ret = setup_transfer(s, o, frame)) < 0) {
            fprintf(stderr, "查不到硬件帧能传成什么格式：%s\n", av_err2str(ret));
            return ret;
        }
        /*
         * 目标帧每次都要先 unref（unref 会把 format 清成 NONE），再设格式：
         * 设了就传成这个格式（必须是硬件帧支持的），没设（NONE）就传成第一种。
         */
        av_frame_unref(s->sw);
        s->sw->format = s->transfer_fmt;
        t0  = now_us();
        ret = av_hwframe_transfer_data(s->sw, frame, 0);
        s->transfer_us += now_us() - t0;
        if (ret < 0) {
            fprintf(stderr, "把硬件帧搬回内存出错：%s\n", av_err2str(ret));
            return ret;
        }
        /* 时间戳、色彩属性这些要不要跟着拷过来，各种硬件后端的 transfer_data 做法不一样（VAAPI 的会拷），自己再拷一次，保证一致。 */
        ret = av_frame_copy_props(s->sw, frame);
        if (ret < 0)
            return ret;
        out = s->sw;
    } else {
        s->nb_sw++;
    }

    if (o->pix_fmt != AV_PIX_FMT_NONE && out->format != o->pix_fmt) {
        int64_t t0 = now_us();

        av_frame_unref(s->cvt);
        s->cvt->format = o->pix_fmt;
        s->cvt->width  = out->width;
        s->cvt->height = out->height;
        ret = sws_scale_frame(s->sws, s->cvt, out);
        s->convert_us += now_us() - t0;
        if (ret < 0) {
            fprintf(stderr, "转换到 %s 出错：%s\n", av_get_pix_fmt_name(o->pix_fmt), av_err2str(ret));
            return ret;
        }
        out = s->cvt;
    }

    /* 按行距排列的帧拷成紧密排列的一块内存（L5），这就是原始像素文件里的一帧。 */
    size = av_image_get_buffer_size(out->format, out->width, out->height, 1);
    if (size < 0)
        return size;
    av_fast_malloc(&s->buf, &s->buf_cap, size);
    if (!s->buf)
        return AVERROR(ENOMEM);
    ret = av_image_copy_to_buffer(s->buf, size, (const uint8_t * const *)out->data, out->linesize,
                                  out->format, out->width, out->height, 1);
    if (ret < 0)
        return ret;
    if (s->md5)
        av_md5_update(s->md5, s->buf, size);
    if (s->file && fwrite(s->buf, 1, size, s->file) != (size_t)size) {
        fprintf(stderr, "写输出文件失败\n");
        return AVERROR(EIO);
    }
    s->bytes += size;
    s->nb_frames++;
    return 0;
}

/* 送一个包（NULL 是冲刷），取出全部帧。*done 在 frames= 的帧数到了以后置 1。 */
static int decode_packet(Decoder *d, Sink *s, const Options *o, AVPacket *pkt, AVFrame *frame, int *done)
{
    int ret = avcodec_send_packet(d->ctx, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包给解码器出错：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_frame(d->ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
            return ret;
        }
        ret = sink_frame(s, o, frame);
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
        if (o->frames && s->nb_frames >= o->frames) {
            *done = 1;
            return 0;
        }
    }
}

static void print_stats(const Decoder *d, const Sink *s, int64_t wall_us, double cpu_s)
{
    const double wall = wall_us / 1e6;

    printf("解码方式：%s\n", d->hw_name ? (d->fell_back ? "显卡（退回了软件格式）" : "显卡") : "软件（CPU）");
    printf("设备：%s\n", d->hw_name ? d->hw_name : "无");
    printf("帧数：%ld（硬件帧 %ld，内存帧 %ld）\n", s->nb_frames, s->nb_hw, s->nb_sw);
    printf("输出：%lld 字节\n", s->bytes);
    printf("耗时（墙钟）：%.3f 秒，%.1f 帧/秒\n", wall, wall > 0 ? s->nb_frames / wall : 0.0);
    printf("CPU 时间（用户+系统，所有线程）：%.3f 秒\n", cpu_s);
    printf("搬运耗时（显存拷回内存）：%.3f 秒\n", s->transfer_us / 1e6);
    printf("转换耗时（libswscale）：%.3f 秒\n", s->convert_us / 1e6);
}

static double cpu_seconds(void)
{
    struct rusage ru;

    if (getrusage(RUSAGE_SELF, &ru))
        return 0;
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

int main(int argc, char **argv)
{
    Options o = { 0 };
    Decoder d = { .hw_pix_fmt = AV_PIX_FMT_NONE };
    Sink s = { .transfer_fmt = AV_PIX_FMT_NONE };
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    char why[512] = "";
    int ret, done = 0;
    int64_t t_start;
    double cpu_start;

    if (parse_options(argc, argv, &o) < 0) {
        fprintf(stderr, "用法：%s <输入文件> <输出文件|-> [device=auto|none|vaapi…] [device_path=…] [fallback=0|1] "
                "[pix_fmt=…] [frames=N] [threads=N] [extra=N] [md5=1] [stats=1] [loglevel=…]\n", argv[0]);
        return 1;
    }
    av_log_set_level(o.loglevel);

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    s.sw  = av_frame_alloc();
    s.cvt = av_frame_alloc();
    if (!pkt || !frame || !s.sw || !s.cvt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avformat_open_input(&d.fmt, o.input, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法打开 %s：%s\n", o.input, av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(d.fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不出流信息：%s\n", av_err2str(ret));
        goto end;
    }
    d.stream = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &d.codec, 0);
    if (d.stream < 0) {
        fprintf(stderr, "%s 里没有能解码的视频流\n", o.input);
        ret = d.stream;
        goto end;
    }

    /* 选设备。auto 或者 fallback=1 时，用不了硬件就退回软件，并说明原因。 */
    ret = open_hw_device(&d, &o, why, sizeof(why));
    if (ret < 0) {
        if (strcmp(o.device, "auto") && !o.fallback) {
            fprintf(stderr, "用不了硬件设备：%s\n（加 fallback=1 可以退回软件解码，加 loglevel=verbose 看硬件后端报告的原因）\n", why);
            goto end;
        }
        fprintf(stderr, "没有可用的硬件设备（%s），改用软件解码\n", why);
    }

    d.ctx = avcodec_alloc_context3(d.codec);
    if (!d.ctx) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avcodec_parameters_to_context(d.ctx, d.fmt->streams[d.stream]->codecpar);
    if (ret < 0)
        goto end;
    d.ctx->pkt_timebase = d.fmt->streams[d.stream]->time_base;
    d.ctx->thread_count = o.threads;
    if (d.hw_name) {
        d.ctx->opaque       = &d;
        d.ctx->get_format   = get_format;
        d.ctx->hw_device_ctx = av_buffer_ref(d.hw_device);
        if (o.extra > 0)
            d.ctx->extra_hw_frames = o.extra;     /* 默认值是 -1，意思是“不多留”，0 也一样 */
        if (!d.ctx->hw_device_ctx) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }
    ret = avcodec_open2(d.ctx, d.codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器 %s：%s\n", d.codec->name, av_err2str(ret));
        goto end;
    }
    fprintf(stderr, "解码器 %s，%s\n", d.codec->name,
            d.hw_name ? "用硬件设备 " : "软件解码");
    if (d.hw_name)
        fprintf(stderr, "  设备类型 %s，硬件帧格式 %s\n", d.hw_name, av_get_pix_fmt_name(d.hw_pix_fmt));

    s.sws = sws_alloc_context();
    if (!s.sws) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (o.md5 && !(s.md5 = av_md5_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    if (s.md5)
        av_md5_init(s.md5);
    if (strcmp(o.output, "-")) {
        s.file = fopen(o.output, "wb");
        if (!s.file) {
            ret = AVERROR(errno);
            fprintf(stderr, "无法写 %s：%s\n", o.output, av_err2str(ret));
            goto end;
        }
    }

    t_start   = now_us();
    cpu_start = cpu_seconds();
    ret = 0;
    while (!done) {
        ret = av_read_frame(d.fmt, pkt);
        if (ret < 0) {
            if (ret != AVERROR_EOF)
                fprintf(stderr, "读包出错：%s\n", av_err2str(ret));
            break;
        }
        if (pkt->stream_index == d.stream)
            ret = decode_packet(&d, &s, &o, pkt, frame, &done);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }
    if (ret == AVERROR_EOF)
        ret = 0;
    if (ret >= 0 && !done)
        ret = decode_packet(&d, &s, &o, NULL, frame, &done);    /* 冲刷：解码器里还有延迟的帧 */

    if (ret >= 0) {
        const int64_t wall_us = now_us() - t_start;

        if (s.md5) {
            uint8_t sum[16];

            av_md5_final(s.md5, sum);
            printf("md5=");
            for (int i = 0; i < 16; i++)
                printf("%02x", sum[i]);
            printf("\n");
        }
        if (o.stats)
            print_stats(&d, &s, wall_us, cpu_seconds() - cpu_start);
    }

end:
    if (s.file && fclose(s.file) && ret >= 0) {
        fprintf(stderr, "写输出文件失败（关闭时）\n");
        ret = AVERROR(EIO);
    }
    av_freep(&s.md5);
    sws_free_context(&s.sws);
    av_freep(&s.buf);
    av_frame_free(&s.sw);
    av_frame_free(&s.cvt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&d.ctx);
    avformat_close_input(&d.fmt);
    av_buffer_unref(&d.hw_device);
    return ret < 0 ? 1 : 0;
}
