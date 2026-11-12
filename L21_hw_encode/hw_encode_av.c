/*
 * L21 硬件编码：读原始像素文件，把每一帧上传到显存，让显卡编码；没有显卡时退回软件编码器。
 *
 * 用法：hw_encode_av <宽> <高> <输入文件|-> <输出文件|-> [选项...]
 *   enc=auto         编码器：auto（默认）先试 h264_vaapi，用不了就退回 libx264；
 *                    写编码器名（h264_vaapi、hevc_vaapi、libx264……）：指定，用不了就失败（加 fallback=1 才退回 libx264）
 *   device_path=路径 创建设备时用的设备名（VAAPI 是 /dev/dri/renderD128 这样的路径），不写就用默认设备
 *   fallback=0       指定了硬件编码器但用不了时：1 退回 libx264，0（默认）报错退出。enc=auto 总是退回
 *   pix_fmt=nv12     输入文件的像素格式（nv12、yuv420p……），帧紧密排列（每行没有填充）。硬件编码器的表面是 nv12，
 *                    输入不是 nv12 时先用 libswscale 转成 nv12 再上传
 *   fps=25           帧率（编码器的时间基是 1/fps，每一帧的 pts 是它的序号）
 *   frames=N         只编码前 N 帧（默认编码到文件末尾）
 *   pool=20          帧池里的显存表面个数（只对硬件编码器有用）
 *   threads=1        软件编码器的线程数（默认 1，输出才可重复；硬件编码器没有这个概念）
 *   opts=k=v:k=v     原样传给编码器的选项，例如 opts=preset=veryfast:crf=28（软件）或 opts=qp=25（VAAPI）
 *   md5=1            算输出码流的 MD5，打印出来（输出文件写 - 的话就只算 MD5，不写文件）
 *   stats=1          打印编码方式、帧数、字节数、耗时
 *   loglevel=名字    FFmpeg 的日志级别：quiet、error、warning、info、verbose……
 *
 * 输出是裸的编码码流（H.264 就是 Annex B 格式的 .h264），没有容器。要装进 MP4 看 L15。
 *
 * 退出码：0 成功；1 失败（参数错误、设备用不了、编码器打不开、输入读不到整数帧、编码出错……），原因写在 stderr。
 *
 * 和官方示例 vaapi_encode.c 的区别：
 *   1. 没有显卡时可以退回软件编码器，而不是直接失败；
 *   2. 建帧上下文之前先问设备“能接受这个尺寸和格式吗”（av_hwdevice_get_hwframe_constraints），失败时能说出原因；
 *   3. 输入格式可选，不是 nv12 时自己转换；
 *   4. 每一帧设了 pts，输出的码流可以装进容器；
 *   5. 所有失败的路径都释放资源、退出码是 1，ASan 下没有泄漏。
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
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

typedef struct Options {
    int                width, height;
    const char        *in, *out;
    const char        *enc;
    const char        *device_path;
    int                fallback;
    enum AVPixelFormat pix_fmt;
    int                fps;
    int                frames;
    int                pool;
    int                threads;
    const char        *opts;
    int                md5, stats;
    int                loglevel;
} Options;

typedef struct Encoder {
    const AVCodec     *codec;
    AVCodecContext    *ctx;
    AVBufferRef       *hw_device;      /* 硬件编码器才有 */
    AVBufferRef       *hw_frames;      /* 硬件编码器才有：显存表面池（帧上下文） */
    enum AVHWDeviceType hw_type;
    enum AVPixelFormat hw_pix_fmt;     /* 硬件格式，例如 vaapi */
    int                is_hw;
} Encoder;

typedef struct Sink {
    FILE              *fp;
    struct AVMD5      *md5;
    int64_t            nb_packets;
    int64_t            nb_bytes;
} Sink;

static int64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static double cpu_seconds(void)
{
    struct rusage ru;

    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static void append(char *buf, size_t size, const char *fmt, ...)
{
    size_t len = strlen(buf);
    va_list ap;

    if (len >= size - 1)
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
    return -1;
}

static int parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max)
        return -1;
    *out = (int)v;
    return 0;
}

static int parse_options(int argc, char **argv, Options *o)
{
    memset(o, 0, sizeof(*o));
    o->enc      = "auto";
    o->pix_fmt  = AV_PIX_FMT_NV12;
    o->fps      = 25;
    o->pool     = 20;
    o->threads  = 1;
    o->loglevel = AV_LOG_WARNING;

    if (argc < 5)
        return -1;
    if (parse_int(argv[1], 2, 16384, &o->width) < 0 || parse_int(argv[2], 2, 16384, &o->height) < 0) {
        fprintf(stderr, "宽和高要是 2 到 16384 之间的整数：%s %s\n", argv[1], argv[2]);
        return -1;
    }
    o->in  = argv[3];
    o->out = argv[4];

    for (int i = 5; i < argc; i++) {
        const char *a = argv[i];

        if (!strncmp(a, "enc=", 4)) {
            o->enc = a + 4;
        } else if (!strncmp(a, "device_path=", 12)) {
            o->device_path = a + 12;
        } else if (!strncmp(a, "fallback=", 9)) {
            if (parse_int(a + 9, 0, 1, &o->fallback) < 0)
                goto bad;
        } else if (!strncmp(a, "pix_fmt=", 8)) {
            o->pix_fmt = av_get_pix_fmt(a + 8);
            if (o->pix_fmt == AV_PIX_FMT_NONE) {
                fprintf(stderr, "不认识的像素格式：%s\n", a + 8);
                return -1;
            }
        } else if (!strncmp(a, "fps=", 4)) {
            if (parse_int(a + 4, 1, 1000, &o->fps) < 0)
                goto bad;
        } else if (!strncmp(a, "frames=", 7)) {
            if (parse_int(a + 7, 0, 100000000, &o->frames) < 0)
                goto bad;
        } else if (!strncmp(a, "pool=", 5)) {
            if (parse_int(a + 5, 1, 1000, &o->pool) < 0)
                goto bad;
        } else if (!strncmp(a, "threads=", 8)) {
            if (parse_int(a + 8, 0, 64, &o->threads) < 0)
                goto bad;
        } else if (!strncmp(a, "opts=", 5)) {
            o->opts = a + 5;
        } else if (!strncmp(a, "md5=", 4)) {
            if (parse_int(a + 4, 0, 1, &o->md5) < 0)
                goto bad;
        } else if (!strncmp(a, "stats=", 6)) {
            if (parse_int(a + 6, 0, 1, &o->stats) < 0)
                goto bad;
        } else if (!strncmp(a, "loglevel=", 9)) {
            o->loglevel = parse_loglevel(a + 9);
            if (o->loglevel < 0) {
                fprintf(stderr, "不认识的日志级别：%s\n", a + 9);
                return -1;
            }
        } else {
            fprintf(stderr, "不认识的选项：%s\n", a);
            return -1;
        }
        continue;
bad:
        fprintf(stderr, "选项的值不对：%s\n", a);
        return -1;
    }
    return 0;
}

/* ---- 选编码器、建设备 ------------------------------------------------------------ */

/*
 * 试一个编码器：找到它；如果它是硬件编码器（硬件配置里有“帧上下文”这一项），再创建设备。
 * 成功返回 0，e 里填好；失败返回负数，why 里追加原因。
 */
static int try_encoder(Encoder *e, const Options *o, const char *name, char *why, size_t why_size)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    const AVCodecHWConfig *cfg = NULL;
    int ret;

    if (!codec) {
        append(why, why_size, "%s这份 FFmpeg 里没有编码器 %s", *why ? "；" : "", name);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    for (int i = 0; (cfg = avcodec_get_hw_config(codec, i)); i++)
        if (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX)
            break;

    e->codec = codec;
    if (!cfg) {
        e->is_hw = 0;
        return 0;
    }
    ret = av_hwdevice_ctx_create(&e->hw_device, cfg->device_type, o->device_path, NULL, 0);
    if (ret < 0) {
        append(why, why_size, "%s%s：创建 %s 设备失败：%s", *why ? "；" : "", name,
               av_hwdevice_get_type_name(cfg->device_type), av_err2str(ret));
        e->codec = NULL;
        return ret;
    }
    e->is_hw      = 1;
    e->hw_type    = cfg->device_type;
    e->hw_pix_fmt = cfg->pix_fmt;
    return 0;
}

static int choose_encoder(Encoder *e, const Options *o, char *why, size_t why_size)
{
    int ret = AVERROR_ENCODER_NOT_FOUND;

    if (!strcmp(o->enc, "auto")) {
        if ((ret = try_encoder(e, o, "h264_vaapi", why, why_size)) == 0)
            return 0;
        fprintf(stderr, "没有可用的硬件编码器（%s），改用软件编码器 libx264\n", why);
        why[0] = 0;
        return try_encoder(e, o, "libx264", why, why_size);
    }
    if ((ret = try_encoder(e, o, o->enc, why, why_size)) == 0)
        return 0;
    if (o->fallback && strcmp(o->enc, "libx264")) {
        fprintf(stderr, "没有可用的编码器 %s（%s），改用软件编码器 libx264\n", o->enc, why);
        why[0] = 0;
        return try_encoder(e, o, "libx264", why, why_size);
    }
    return ret;
}

/*
 * 建帧上下文（显存里的表面池）：先问设备“能接受这个尺寸和格式吗”，再建。
 * 编码器要的是“显存里的帧”，所以帧上下文要我们自己建、在 avcodec_open2() 之前交给它。
 */
static int setup_frames_ctx(Encoder *e, const Options *o, char *why, size_t why_size)
{
    AVHWFramesConstraints *c = av_hwdevice_get_hwframe_constraints(e->hw_device, NULL);
    AVHWFramesContext *fc;
    int ret, ok = 0;

    if (c) {
        for (const enum AVPixelFormat *p = c->valid_sw_formats; p && *p != AV_PIX_FMT_NONE; p++)
            if (*p == AV_PIX_FMT_NV12)
                ok = 1;
        if (c->valid_sw_formats && !ok) {
            append(why, why_size, "设备不接受 nv12 的表面");
            av_hwframe_constraints_free(&c);
            return AVERROR(EINVAL);
        }
        if (o->width < c->min_width || o->height < c->min_height ||
            (c->max_width > 0 && o->width > c->max_width) || (c->max_height > 0 && o->height > c->max_height)) {
            append(why, why_size, "设备不支持 %dx%d：范围是 %dx%d 到 %dx%d", o->width, o->height,
                   c->min_width, c->min_height, c->max_width, c->max_height);
            av_hwframe_constraints_free(&c);
            return AVERROR(EINVAL);
        }
        av_hwframe_constraints_free(&c);
    }

    e->hw_frames = av_hwframe_ctx_alloc(e->hw_device);
    if (!e->hw_frames)
        return AVERROR(ENOMEM);
    fc = (AVHWFramesContext *)e->hw_frames->data;
    fc->format            = e->hw_pix_fmt;
    fc->sw_format         = AV_PIX_FMT_NV12;
    fc->width             = o->width;
    fc->height            = o->height;
    fc->initial_pool_size = o->pool;
    if ((ret = av_hwframe_ctx_init(e->hw_frames)) < 0) {
        append(why, why_size, "初始化帧上下文失败：%s（池 %d 个表面，%dx%d）", av_err2str(ret), o->pool, o->width, o->height);
        return ret;
    }
    return 0;
}

static int open_encoder(Encoder *e, const Options *o, char *why, size_t why_size)
{
    AVDictionary *opts = NULL;
    int ret;

    e->ctx = avcodec_alloc_context3(e->codec);
    if (!e->ctx)
        return AVERROR(ENOMEM);
    e->ctx->width                = o->width;
    e->ctx->height               = o->height;
    e->ctx->time_base            = (AVRational){ 1, o->fps };
    e->ctx->framerate            = (AVRational){ o->fps, 1 };
    e->ctx->pix_fmt              = e->is_hw ? e->hw_pix_fmt : o->pix_fmt;
    if (!e->is_hw)
        e->ctx->thread_count = o->threads;

    if (e->is_hw) {
        if ((ret = setup_frames_ctx(e, o, why, why_size)) < 0)
            return ret;
        e->ctx->hw_frames_ctx = av_buffer_ref(e->hw_frames);
        if (!e->ctx->hw_frames_ctx)
            return AVERROR(ENOMEM);
    }
    if (o->opts && (ret = av_dict_parse_string(&opts, o->opts, "=", ":", 0)) < 0) {
        append(why, why_size, "opts 的写法不对（要 k=v:k=v）：%s", o->opts);
        return ret;
    }
    ret = avcodec_open2(e->ctx, e->codec, &opts);
    if (ret < 0) {
        append(why, why_size, "打不开编码器 %s：%s", e->codec->name, av_err2str(ret));
    } else if (av_dict_count(opts)) {
        const AVDictionaryEntry *t = av_dict_iterate(opts, NULL);

        append(why, why_size, "编码器 %s 不认识选项 %s", e->codec->name, t->key);
        ret = AVERROR_OPTION_NOT_FOUND;
    }
    av_dict_free(&opts);
    return ret;
}

/* ---- 编码、输出 ------------------------------------------------------------------ */

static int sink_packet(Sink *s, const AVPacket *pkt)
{
    if (s->fp && fwrite(pkt->data, 1, pkt->size, s->fp) != (size_t)pkt->size) {
        fprintf(stderr, "写输出文件出错：%s\n", strerror(errno));
        return AVERROR(EIO);
    }
    if (s->md5)
        av_md5_update(s->md5, pkt->data, pkt->size);
    s->nb_packets++;
    s->nb_bytes += pkt->size;
    return 0;
}

/* 送一帧（NULL 是冲刷），取出所有的包。 */
static int encode_frame(Encoder *e, Sink *s, AVPacket *pkt, AVFrame *frame)
{
    int ret = avcodec_send_frame(e->ctx, frame);

    if (ret < 0) {
        fprintf(stderr, "送帧给编码器出错：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_packet(e->ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "从编码器取包出错：%s\n", av_err2str(ret));
            return ret;
        }
        ret = sink_packet(s, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            return ret;
    }
}

/* 读一整帧：返回 1 读到，0 干净地到了文件末尾，负数 出错或者最后一帧不完整。 */
static int read_frame(FILE *fp, uint8_t *buf, size_t size)
{
    size_t n = fread(buf, 1, size, fp);

    if (n == size)
        return 1;
    if (ferror(fp)) {
        fprintf(stderr, "读输入文件出错：%s\n", strerror(errno));
        return AVERROR(EIO);
    }
    if (n == 0)
        return 0;
    fprintf(stderr, "输入文件的最后一帧不完整：只有 %zu 字节，一帧要 %zu 字节\n", n, size);
    return AVERROR_INVALIDDATA;
}

static void print_stats(const Encoder *e, const Options *o, const Sink *s, int64_t nb_in, int64_t wall_us, double cpu_s,
                        int64_t upload_us, int64_t convert_us)
{
    double wall = wall_us / 1e6;

    printf("编码器：%s（%s）\n", e->codec->name, e->is_hw ? "显卡" : "软件（CPU）");
    printf("设备：%s\n", e->is_hw ? av_hwdevice_get_type_name(e->hw_type) : "无");
    printf("输入：%dx%d %s，%lld 帧\n", o->width, o->height, av_get_pix_fmt_name(o->pix_fmt), (long long)nb_in);
    printf("输出：%lld 个包，%lld 字节\n", (long long)s->nb_packets, (long long)s->nb_bytes);
    printf("耗时（墙钟）：%.3f 秒，%.1f 帧/秒\n", wall, wall > 0 ? nb_in / wall : 0.0);
    printf("CPU 时间（用户+系统，所有线程）：%.3f 秒\n", cpu_s);
    printf("上传耗时（内存拷到显存）：%.3f 秒\n", upload_us / 1e6);
    printf("转换耗时（libswscale）：%.3f 秒\n", convert_us / 1e6);
}

int main(int argc, char **argv)
{
    Options o;
    Encoder e = { 0 };
    Sink sink = { 0 };
    FILE *in = NULL;
    AVFrame *sw = NULL, *conv = NULL, *hw = NULL;
    AVPacket *pkt = NULL;
    struct SwsContext *sws = NULL;
    uint8_t *buf = NULL, *data[4];
    int linesize[4];
    char why[1024] = "";
    int64_t nb_in = 0, t_start, upload_us = 0, convert_us = 0;
    double cpu_start;
    size_t frame_size;
    int ret = 0, size;

    if (parse_options(argc, argv, &o) < 0) {
        fprintf(stderr, "用法：%s <宽> <高> <输入文件|-> <输出文件|-> [enc=auto|名字] [device_path=路径] [fallback=0|1] "
                "[pix_fmt=nv12] [fps=25] [frames=N] [pool=20] [threads=1] [opts=k=v:k=v] [md5=1] [stats=1] [loglevel=名字]\n"
                "（文件开头的注释里有每个选项的说明）\n", argv[0]);
        return 1;
    }
    av_log_set_level(o.loglevel);

    size = av_image_get_buffer_size(o.pix_fmt, o.width, o.height, 1);
    if (size <= 0) {
        fprintf(stderr, "算不出 %dx%d %s 一帧的大小\n", o.width, o.height, av_get_pix_fmt_name(o.pix_fmt));
        return 1;
    }
    frame_size = size;

    /* 1. 选编码器（硬件的话同时创建设备），建帧上下文，打开编码器 */
    ret = choose_encoder(&e, &o, why, sizeof(why));
    if (ret < 0) {
        fprintf(stderr, "用不了编码器：%s\n", why);
        if (strcmp(o.enc, "auto") && !o.fallback)
            fprintf(stderr, "（加 fallback=1 可以退回软件编码器，加 loglevel=verbose 看硬件后端报告的原因）\n");
        goto end;
    }
    why[0] = 0;
    if ((ret = open_encoder(&e, &o, why, sizeof(why))) < 0) {
        fprintf(stderr, "%s\n", why);
        goto end;
    }

    /* 2. 输入、输出、缓冲区 */
    in = !strcmp(o.in, "-") ? stdin : fopen(o.in, "rb");
    if (!in) {
        fprintf(stderr, "打不开输入文件 %s：%s\n", o.in, strerror(errno));
        ret = AVERROR(errno);
        goto end;
    }
    if (strcmp(o.out, "-") && !(sink.fp = fopen(o.out, "wb"))) {
        fprintf(stderr, "打不开输出文件 %s：%s\n", o.out, strerror(errno));
        ret = AVERROR(errno);
        goto end;
    }
    if (o.md5) {
        if (!(sink.md5 = av_md5_alloc())) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        av_md5_init(sink.md5);
    }
    pkt = av_packet_alloc();
    sw  = av_frame_alloc();
    hw  = av_frame_alloc();
    buf = av_malloc(frame_size);
    if (!pkt || !sw || !hw || !buf) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    /* 输入帧：紧密排列的缓冲区（读文件用）和一个 AVFrame（每一行可能有填充，上传和编码用）。
     * 不能直接往 AVFrame 的 data[0] 里读：见 L21 README 第 2 步和 patch 0099。 */
    av_image_fill_arrays(data, linesize, buf, o.pix_fmt, o.width, o.height, 1);
    sw->format = o.pix_fmt;
    sw->width  = o.width;
    sw->height = o.height;
    if ((ret = av_frame_get_buffer(sw, 0)) < 0)
        goto end;

    /* 硬件编码器只收 nv12 的表面：输入不是 nv12 时，上传前先转换 */
    if (e.is_hw && o.pix_fmt != AV_PIX_FMT_NV12) {
        conv = av_frame_alloc();
        sws  = sws_alloc_context();
        if (!conv || !sws) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        conv->format = AV_PIX_FMT_NV12;
        conv->width  = o.width;
        conv->height = o.height;
        if ((ret = av_frame_get_buffer(conv, 0)) < 0)
            goto end;
    }

    /* 3. 读一帧、（转格式）、（上传）、编码 */
    t_start   = now_us();
    cpu_start = cpu_seconds();
    for (;;) {
        AVFrame *src = sw;
        int r;

        if (o.frames && nb_in >= o.frames)
            break;
        r = read_frame(in, buf, frame_size);
        if (r == 0)
            break;
        if (r < 0) {
            ret = r;
            goto end;
        }
        if ((ret = av_frame_make_writable(sw)) < 0)
            goto end;
        av_image_copy2(sw->data, sw->linesize, data, linesize, o.pix_fmt, o.width, o.height);

        if (conv) {
            int64_t t0 = now_us();

            if ((ret = av_frame_make_writable(conv)) < 0)
                goto end;
            if ((ret = sws_scale_frame(sws, conv, sw)) < 0) {
                fprintf(stderr, "转换像素格式出错：%s\n", av_err2str(ret));
                goto end;
            }
            convert_us += now_us() - t0;
            src = conv;
        }

        if (e.is_hw) {
            int64_t t0 = now_us();

            av_frame_unref(hw);
            if ((ret = av_hwframe_get_buffer(e.ctx->hw_frames_ctx, hw, 0)) < 0) {
                fprintf(stderr, "从帧池里取不到显存表面：%s（池里 %d 个）\n", av_err2str(ret), o.pool);
                goto end;
            }
            if ((ret = av_hwframe_transfer_data(hw, src, 0)) < 0) {
                fprintf(stderr, "上传帧到显存出错：%s\n", av_err2str(ret));
                goto end;
            }
            upload_us += now_us() - t0;
            hw->pts = nb_in;
            ret = encode_frame(&e, &sink, pkt, hw);
        } else {
            src->pts = nb_in;
            ret = encode_frame(&e, &sink, pkt, src);
        }
        if (ret < 0)
            goto end;
        nb_in++;
    }
    /* 冲刷编码器：把里面还没出来的帧（B 帧重排、延迟）都取出来 */
    if ((ret = encode_frame(&e, &sink, pkt, NULL)) < 0)
        goto end;

    if (o.md5) {
        uint8_t digest[16];

        av_md5_final(sink.md5, digest);
        printf("md5=");
        for (int i = 0; i < 16; i++)
            printf("%02x", digest[i]);
        printf("\n");
    }
    if (o.stats)
        print_stats(&e, &o, &sink, nb_in, now_us() - t_start, cpu_seconds() - cpu_start, upload_us, convert_us);

end:
    if (in && in != stdin)
        fclose(in);
    if (sink.fp && fclose(sink.fp) != 0 && ret >= 0) {
        fprintf(stderr, "写输出文件出错：%s\n", strerror(errno));
        ret = AVERROR(EIO);
    }
    av_free(sink.md5);
    sws_free_context(&sws);
    av_freep(&buf);
    av_packet_free(&pkt);
    av_frame_free(&sw);
    av_frame_free(&conv);
    av_frame_free(&hw);
    avcodec_free_context(&e.ctx);
    av_buffer_unref(&e.hw_frames);
    av_buffer_unref(&e.hw_device);
    return ret < 0 ? 1 : 0;
}
