/*
 * mtool transcode：转码——换编码、换容器、改码率、缩放、加滤镜、裁剪，带进度，可选硬件编码。
 *
 *   mtool transcode [选项] <输入> <输出>
 *
 * 来自 L12（滤镜图）、L13（编码器选项）、L15（复用）、L16（完整转码）、L21（硬件编码）。
 * 每一路流（只处理第一路视频和第一路音频）各有一条流水线，互不相干，只在最后的复用器里汇合：
 *
 *   输入 → 解复用 → 解码 → 滤镜图 [trim、转正、用户滤镜、缩放、帧率] → 编码 → 复用 → 输出
 *
 * 和 L16 transcode_av 的差别：选项是命令行风格（--crf、--vb、--size……）；有进度；出错不留半个输出文件；
 * 视频的旋转默认转正（和 ffmpeg 一样），可变帧率的输入保持可变；硬件编码用 --hw（只加速编码，解码仍在 CPU 上）。
 * 沿用 L16 的结论：音频 pts 用 av_rescale_delta 接连续；编码前把帧标成“没有类型”让编码器自己决定关键帧；
 * 帧要带时长（不然 MP4 最后一帧没有时长，patch 0011）；先配滤镜图再按出口的参数打开编码器。
 */
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/eval.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>
#include <libavutil/time.h>

#include "mtool.h"

#define PREROLL_SECONDS 1.0

/* 一路流的流水线 */
typedef struct TStream {
    const char      *name;          /* “视频”或“音频” */
    enum AVMediaType type;
    int              enabled;
    AVStream        *ist, *ost;
    AVCodecContext  *dec, *enc;
    MtGraph          graph;
    AVFrame         *frame, *fframe;
    AVPacket        *pkt;
    AVDictionary    *enc_opts;
    int64_t          rescale_last;  /* av_rescale_delta 的状态（音频） */
    int              past_end;      /* 过了结束时间，不用再处理这一路的包 */
    int64_t          frames_in, frames_out, packets, bytes, decode_errors;
    double           end_time;      /* 最后一个包的结束时间（秒，输出时间线） */
    int              vfr;           /* 视频：可变帧率，不转成固定帧率 */
} TStream;

typedef struct Transcode {
    AVFormatContext *ic, *oc;
    TStream          v, a;
    double           start, end;    /* 输入时间线上的起止（已经加上文件起始时间）；end <= 0 不限 */
    double           total;         /* 要转的时长（秒），未知 0 */
    double           limit;         /* 给了 -e 或 -d 时输出时间线上的终点（秒），0 不限 */
    AVBufferRef     *hw_dev;
    Progress         prog;
    double           last_report;
} Transcode;

typedef struct Opts {
    double      start, end, dur;
    const char *vcodec, *acodec, *preset, *crf, *vf, *af, *fmt_name, *hw;
    double      vb, ab;
    int         gop, fps, width, box_w, box_h, ar, ac, threads;
    int         no_video, no_audio, no_rotate, faststart, overwrite;
    AVDictionary *vopts, *aopts;
} Opts;

static void usage(FILE *f)
{
    fprintf(f,
            "用法：mtool transcode [选项] <输入> <输出>\n\n"
            "转码：换编码、换容器、改码率、缩放、加滤镜、裁剪。只处理第一路视频和第一路音频（字幕等丢掉）。\n"
            "编码器按输出容器选默认的（MP4、MKV 是 H.264 加 AAC），可以用选项换。\n\n"
            "编码：\n"
            "      --vcodec 名字   视频编码器（ffmpeg -encoders 看有哪些），例如 libx264、mpeg4、h264_vaapi\n"
            "      --acodec 名字   音频编码器，例如 aac、mp2、flac\n"
            "      --crf 数        恒定质量（libx264：0 到 51，越小越好；VAAPI 编码器当作 qp）\n"
            "      --vb 码率       视频码率，例如 1500k、2M（和 --crf 同时给时以 --vb 为准）\n"
            "      --ab 码率       音频码率，例如 96k\n"
            "      --preset 名字   编码器的 preset，例如 ultrafast、medium、slow\n"
            "      --gop 数        关键帧间隔（帧数）\n"
            "      --vopt 名=值    直接交给视频编码器的选项（可以重复），例如 --vopt tune=film\n"
            "      --aopt 名=值    直接交给音频编码器的选项（可以重复）\n"
            "      --threads 数    编码线程数，默认 0（编码器自己决定）；1 的结果最可重复（L13）\n"
            "      --hw 模式       none（默认）、auto、vaapi：用 VAAPI 硬件编码。auto 没有设备时退回软件，vaapi 没有设备就失败\n\n"
            "画面和声音：\n"
            "  -w, --width 宽      视频宽，高按比例（取偶数）\n"
            "      --size 宽x高    缩进这个框里，保持比例（和 -w 只能给一个）\n"
            "      --fps 数        输出帧率（不给就保持输入的，可变帧率保持可变）\n"
            "      --ar 采样率     音频采样率，默认和输入相同\n"
            "      --ac 声道数     音频声道数，默认和输入相同\n"
            "      --vf 滤镜图     视频滤镜图，写法和 ffmpeg 的 -vf 相同（L12），放在转正之后、缩放之前\n"
            "      --af 滤镜图     音频滤镜图\n"
            "      --no-rotate     不按视频里的旋转信息转正：保持像素原样，旋转信息带到输出里\n"
            "      --no-video      不要视频\n"
            "      --no-audio      不要音频\n\n"
            "范围和输出：\n"
            "  -s, --start 时间    从这里开始（秒，也可以写 1:30），精确到帧和采样\n"
            "  -e, --end 时间      到这里结束，从文件开头算起（和 -d 只能给一个）\n"
            "  -d, --duration 时间 时长，从 -s 算起\n"
            "  -f, --format 格式   指定输出容器，默认由扩展名决定\n"
            "      --faststart     MP4 把索引移到文件开头（先写完再搬）\n"
            "  -y                  已经存在的输出覆盖掉\n"
            "  -h, --help          显示这段说明\n");
}

/* ------------------------------------------------------------------------ */
/* 编码和写出                                                                */
/* ------------------------------------------------------------------------ */

static int is_vaapi_encoder(const AVCodec *c)
{
    const size_t n = strlen(c->name);

    return n > 6 && !strcmp(c->name + n - 6, "_vaapi");
}

/* 进度按输出时间线上已经写出的最大时间算（视频和音频谁大用谁） */
static void report_progress(Transcode *t, double out_time)
{
    if (out_time > t->last_report)
        t->last_report = out_time;
    mt_progress_update(&t->prog, t->last_report, t->v.frames_out);
}

/* 编码一帧（NULL 是冲刷），把出来的包全部写进输出。 */
static int encode_write(Transcode *t, TStream *s, AVFrame *frame)
{
    int ret;

    if (frame) {
        /* 帧的时间戳现在是滤镜图出口的时间基（frame->time_base），要换成编码器的时间基 */
        if (frame->pts != AV_NOPTS_VALUE)
            frame->pts = av_rescale_q(frame->pts, frame->time_base, s->enc->time_base);
        /* 固定帧率会把时间戳取整到帧格子上：起点不在格子上时，trim 放过的最后一帧取整后可能正好落在终点上。
         * 视频的输出时长不能超过要求的（ffmpeg 命令行也这样），这一帧不要。音频由 atrim 精确到采样，不用管 */
        if (s->type == AVMEDIA_TYPE_VIDEO && t->limit > 0 && frame->pts != AV_NOPTS_VALUE &&
            frame->pts * av_q2d(s->enc->time_base) >= t->limit - 1e-6)
            return 0;
        /* 时长也要给，不然编码器输出的包没有时长，MP4 丢最后一帧的时长（patch 0011）。理由见 L16 */
        if (s->type == AVMEDIA_TYPE_AUDIO)
            frame->duration = frame->nb_samples;
        else if (!s->vfr && s->enc->framerate.num > 0)
            frame->duration = 1;
        else
            frame->duration = av_rescale_q(frame->duration, frame->time_base, s->enc->time_base);
        frame->time_base = s->enc->time_base;
        s->frames_out++;
    }
    ret = avcodec_send_frame(s->enc, frame);
    if (ret < 0) {
        mt_averr(ret, "送%s帧给编码器 %s 失败", s->name, s->enc->codec->name);
        return ret;
    }
    while ((ret = avcodec_receive_packet(s->enc, s->pkt)) >= 0) {
        av_packet_rescale_ts(s->pkt, s->enc->time_base, s->ost->time_base);
        s->pkt->stream_index = s->ost->index;
        s->packets++;
        s->bytes += s->pkt->size;
        if (s->pkt->pts != AV_NOPTS_VALUE) {
            const double e = (s->pkt->pts + FFMAX(s->pkt->duration, 0)) * av_q2d(s->ost->time_base);

            if (e > s->end_time)
                s->end_time = e;
            report_progress(t, s->pkt->pts * av_q2d(s->ost->time_base));
        }
        ret = av_interleaved_write_frame(t->oc, s->pkt);       /* 不管成败，包都已经交出去了 */
        if (ret < 0) {
            mt_averr(ret, "写%s包失败", s->name);
            return ret;
        }
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    mt_averr(ret, "从%s编码器取包失败", s->name);
    return ret;
}

/* frame 为 NULL 表示输入结束，要让滤镜图把缓存的帧都吐出来 */
static int filter_encode_write(Transcode *t, TStream *s, AVFrame *frame)
{
    int ret = av_buffersrc_add_frame_flags(s->graph.src, frame, 0);

    if (ret == AVERROR_EOF) {
        s->past_end = 1;        /* trim 已经输出了最后一帧，后面的帧它都不要，不是错误 */
    } else if (ret < 0) {
        mt_averr(ret, "把%s帧送进滤镜图失败", s->name);
        return ret;
    }
    while (1) {
        ret = av_buffersink_get_frame(s->graph.sink, s->fframe);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            mt_averr(ret, "从%s滤镜图取帧失败", s->name);
            return ret;
        }
        /* 滤镜图里的 fps 这类滤镜可能改了时间基：以出口的为准（L12） */
        s->fframe->time_base = av_buffersink_get_time_base(s->graph.sink);
        /* 解码器把关键帧标成 I 帧，原样交给编码器会让每个 I 帧都变成关键帧：标成“没有类型”，让编码器自己决定（L16） */
        s->fframe->pict_type = AV_PICTURE_TYPE_NONE;
        /* 旋转已经在滤镜里做掉了（或者在 --no-rotate 时由流上的旋转信息带过去），帧上的显示矩阵不能再留着：
         * 留着的话编码器和复用器可能把它写进输出，播放器就转第二次（和 thumb 的 PNG 同一个坑） */
        av_frame_remove_side_data(s->fframe, AV_FRAME_DATA_DISPLAYMATRIX);
        ret = encode_write(t, s, s->fframe);
        av_frame_unref(s->fframe);
        if (ret < 0)
            return ret;
    }
}

/*
 * 音频帧的时间戳整理（L16 讲过）：容器的时间基很粗时每一帧的 pts 都被舍入过，av_rescale_delta 利用
 * “每帧正好 nb_samples 个采样”把它们在舍入误差范围内接成连续的，真有断档时自动改用普通换算；ffmpeg 命令行也这样做。
 */
static void fix_audio_pts(TStream *s, AVFrame *frame)
{
    const AVRational sr_tb = { 1, frame->sample_rate };

    if (frame->pts == AV_NOPTS_VALUE) {
        frame->pts = s->rescale_last == AV_NOPTS_VALUE ? 0 : s->rescale_last;
        s->rescale_last = frame->pts + frame->nb_samples;
    } else {
        frame->pts = av_rescale_delta(s->ist->time_base, frame->pts, sr_tb, frame->nb_samples, &s->rescale_last, sr_tb);
    }
    frame->time_base = sr_tb;
}

/* 把解码器输出的帧都取出来，送进滤镜图。pkt 是 NULL 表示冲刷解码器 */
static int decode_packet(Transcode *t, TStream *s, AVPacket *pkt)
{
    int ret = avcodec_send_packet(s->dec, pkt);

    if (ret < 0 && ret != AVERROR_EOF) {
        /* 损坏的包：计数，继续往后读（和 info、thumb 一样，损坏的文件不算失败） */
        if (ret == AVERROR_INVALIDDATA) {
            s->decode_errors++;
            return 0;
        }
        mt_averr(ret, "送%s包给解码器失败", s->name);
        return ret;
    }
    while ((ret = avcodec_receive_frame(s->dec, s->frame)) >= 0) {
        s->frame->pts = s->frame->best_effort_timestamp;    /* pts 缺失时解码器按 dts 等线索推测的值 */
        if (s->type == AVMEDIA_TYPE_AUDIO)
            fix_audio_pts(s, s->frame);
        s->frames_in++;
        ret = filter_encode_write(t, s, s->frame);
        av_frame_unref(s->frame);
        if (ret < 0)
            return ret;
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    if (ret == AVERROR_INVALIDDATA)
        return 0;
    mt_averr(ret, "从%s解码器取帧失败", s->name);
    return ret;
}

/* 一路流读完以后从上游到下游依次冲刷：解码器 → 滤镜图 → 编码器（L11） */
static int flush_stream(Transcode *t, TStream *s)
{
    int ret = decode_packet(t, s, NULL);

    if (ret < 0)
        return ret;
    if ((ret = filter_encode_write(t, s, NULL)) < 0)
        return ret;
    return encode_write(t, s, NULL);
}

/* ------------------------------------------------------------------------ */
/* 建流水线：编码器的选择、滤镜图、编码器、输出流                              */
/* ------------------------------------------------------------------------ */

/* 输出扩展名是只有音频的格式吗？这时视频丢掉，不报错（ffmpeg 命令行对 wav 也这样，对 m4a 则会把封面带进去） */
static int audio_only_output(const char *path)
{
    static const char *const exts[] = { "wav", "flac", "aac", "mp2", "mp3", "m4a", "ogg", "oga", "opus", "ac3", "mka", "aiff" };
    const char *dot = strrchr(path, '.');

    if (dot)
        for (size_t i = 0; i < FF_ARRAY_ELEMS(exts); i++)
            if (!av_strcasecmp(dot + 1, exts[i]))
                return 1;
    return 0;
}

/*
 * 输出格式放得下这个编码，并且这份 FFmpeg 有它的编码器吗？
 * avformat_query_codec 返回 1 是放得下，0 是放不下，负数是“复用器没有办法回答”（mpegts 就是）：
 * 负数当作放得下，真放不下的话写文件头时会失败，错误说得清楚。
 */
static int container_accepts(const AVOutputFormat *of, enum AVCodecID id)
{
    return avformat_query_codec(of, id, FF_COMPLIANCE_NORMAL) != 0;
}

static const AVCodec *usable_encoder(const AVOutputFormat *of, enum AVCodecID id)
{
    if (id == AV_CODEC_ID_NONE || !container_accepts(of, id))
        return NULL;
    return avcodec_find_encoder(id);
}

static const AVCodec *pick_video_encoder(const Transcode *t, const Opts *o)
{
    const AVCodec *c;

    if (o->vcodec) {
        c = avcodec_find_encoder_by_name(o->vcodec);
        if (!c || c->type != AVMEDIA_TYPE_VIDEO)
            mt_err("没有叫 %s 的视频编码器（ffmpeg -encoders 能看到这份 FFmpeg 有哪些）", o->vcodec);
        return c && c->type == AVMEDIA_TYPE_VIDEO ? c : NULL;
    }
    if (t->hw_dev && container_accepts(t->oc->oformat, AV_CODEC_ID_H264) && (c = avcodec_find_encoder_by_name("h264_vaapi")))
        return c;
    if (container_accepts(t->oc->oformat, AV_CODEC_ID_H264) && (c = avcodec_find_encoder_by_name("libx264")))
        return c;
    if ((c = usable_encoder(t->oc->oformat, t->oc->oformat->video_codec)))
        return c;
    mt_err("输出格式 %s 没有能用的视频编码器：用 --vcodec 指定一个", t->oc->oformat->name);
    return NULL;
}

static const AVCodec *pick_audio_encoder(const Transcode *t, const Opts *o)
{
    const AVCodec *c;

    if (o->acodec) {
        c = avcodec_find_encoder_by_name(o->acodec);
        if (!c || c->type != AVMEDIA_TYPE_AUDIO)
            mt_err("没有叫 %s 的音频编码器（ffmpeg -encoders 能看到这份 FFmpeg 有哪些）", o->acodec);
        return c && c->type == AVMEDIA_TYPE_AUDIO ? c : NULL;
    }
    if ((c = usable_encoder(t->oc->oformat, AV_CODEC_ID_AAC)))
        return c;
    if ((c = usable_encoder(t->oc->oformat, t->oc->oformat->audio_codec)))
        return c;
    if ((c = usable_encoder(t->oc->oformat, AV_CODEC_ID_MP2)))
        return c;
    mt_err("输出格式 %s 没有能用的音频编码器：用 --acodec 指定一个", t->oc->oformat->name);
    return NULL;
}

/* 用户指定的采样率必须是编码器支持的，不然滤镜图协商失败，错误信息看不懂 */
static int check_audio_rate(const AVCodec *enc, int rate)
{
    const int *rates = NULL;
    char buf[256];
    size_t len = 0;
    int n = 0;

    if (avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_RATE, 0, (const void **)&rates, &n) < 0 || !rates || n <= 0)
        return 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (rates[i] == rate)
            return 0;
        if (len + 12 < sizeof(buf))
            len += snprintf(buf + len, sizeof(buf) - len, "%s%d", i ? "、" : "", rates[i]);
    }
    mt_err("编码器 %s 不支持采样率 %d Hz，它支持：%s", enc->name, rate, buf);
    return AVERROR(EINVAL);
}

/* 视频的滤镜图描述：[trim,setpts,][转正,][用户的,][缩放,][fps,][format=nv12,hwupload] */
static void build_video_desc(const Transcode *t, const Opts *o, const TStream *s, int hw, char *desc, size_t size)
{
    char rot[48] = "";
    int n = 0;

    desc[0] = '\0';
    if (t->start > 0 || t->end > 0) {
        n += snprintf(desc + n, size - n, "trim=start=%.6f", t->start);
        if (t->end > 0)
            n += snprintf(desc + n, size - n, ":end=%.6f", t->end);
        n += snprintf(desc + n, size - n, ",setpts=PTS-%.6f/TB,", t->start);
    }
    if (!o->no_rotate) {
        mt_rotation_filter(s->ist, rot, sizeof(rot));
        if (rot[0])
            n += snprintf(desc + n, size - n, "%s,", rot);
    }
    if (o->vf && *o->vf)
        n += snprintf(desc + n, size - n, "%s,", o->vf);
    if (o->width)
        n += snprintf(desc + n, size - n, "scale=%d:-2,", o->width);
    else if (o->box_w)
        n += snprintf(desc + n, size - n, "scale=%d:%d:force_original_aspect_ratio=decrease:force_divisible_by=2,", o->box_w, o->box_h);
    if (o->fps)
        n += snprintf(desc + n, size - n, "fps=%d,", o->fps);
    if (hw)
        n += snprintf(desc + n, size - n, "format=nv12,hwupload,");
    if (n > 0 && desc[n - 1] == ',')
        desc[n - 1] = '\0';
    if (!desc[0])
        snprintf(desc, size, "null");
}

static void build_audio_desc(const Transcode *t, const Opts *o, char *desc, size_t size)
{
    int n = 0;

    desc[0] = '\0';
    if (t->start > 0 || t->end > 0) {
        n += snprintf(desc + n, size - n, "atrim=start=%.6f", t->start);
        if (t->end > 0)
            n += snprintf(desc + n, size - n, ":end=%.6f", t->end);
        n += snprintf(desc + n, size - n, ",asetpts=PTS-%.6f/TB,", t->start);
    }
    if (o->af && *o->af)
        n += snprintf(desc + n, size - n, "%s,", o->af);
    if (o->ar || o->ac) {
        n += snprintf(desc + n, size - n, "aformat=");
        if (o->ar)
            n += snprintf(desc + n, size - n, "sample_rates=%d%s", o->ar, o->ac ? ":" : "");
        if (o->ac) {
            AVChannelLayout l = { 0 };
            char name[64];

            av_channel_layout_default(&l, o->ac);
            av_channel_layout_describe(&l, name, sizeof(name));
            av_channel_layout_uninit(&l);
            n += snprintf(desc + n, size - n, "channel_layouts=%s", name);
        }
        n += snprintf(desc + n, size - n, ",");
    }
    if (n > 0 && desc[n - 1] == ',')
        desc[n - 1] = '\0';
    if (!desc[0])
        snprintf(desc, size, "anull");
}

/* 滤镜图配好以后向出口问它实际输出什么，用来设置编码器（L16） */
static int open_video_encoder(Transcode *t, const Opts *o, TStream *s, const AVCodec *codec)
{
    AVCodecContext *enc, *dec = s->dec;
    const AVRational fr = av_buffersink_get_frame_rate(s->graph.sink);
    int ret;

    enc = avcodec_alloc_context3(codec);
    if (!enc) {
        mt_err("内存不够");
        return AVERROR(ENOMEM);
    }
    s->enc = enc;
    enc->width                  = av_buffersink_get_w(s->graph.sink);
    enc->height                 = av_buffersink_get_h(s->graph.sink);
    enc->pix_fmt                = av_buffersink_get_format(s->graph.sink);
    enc->sample_aspect_ratio    = av_buffersink_get_sample_aspect_ratio(s->graph.sink);
    enc->colorspace             = av_buffersink_get_colorspace(s->graph.sink);
    enc->color_range            = av_buffersink_get_color_range(s->graph.sink);
    enc->chroma_sample_location = av_buffersink_get_chroma_location(s->graph.sink);
    enc->color_primaries        = dec->color_primaries;     /* 出口不提供原色和传输特性，照着解码器抄（L13、patch 0074） */
    enc->color_trc              = dec->color_trc;
    /* 固定帧率：编码器时间基是帧率的倒数，每帧 pts 差 1；可变帧率：用出口的时间基，保持每一帧的时间 */
    if (fr.num > 0 && fr.den > 0) {
        enc->framerate = fr;
        enc->time_base = s->vfr ? av_buffersink_get_time_base(s->graph.sink) : av_inv_q(fr);
    } else {
        enc->time_base = av_buffersink_get_time_base(s->graph.sink);
    }
    if (enc->pix_fmt == AV_PIX_FMT_VAAPI) {
        enc->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(s->graph.sink));
        if (!enc->hw_frames_ctx) {
            mt_err("硬件编码器需要滤镜图出口的 hw_frames_ctx，但图里没有（hwupload 没有生效？）");
            return AVERROR(EINVAL);
        }
    }
    if (o->gop > 0)
        enc->gop_size = o->gop;
    if (o->vb > 0)
        enc->bit_rate = (int64_t)o->vb;
    if (o->crf && !(o->vb > 0)) {
        /* libx264 叫 crf；VAAPI 编码器没有 crf，恒定质量的选项叫 qp */
        av_dict_set(&s->enc_opts, enc->pix_fmt == AV_PIX_FMT_VAAPI ? "qp" : "crf", o->crf, 0);
    }
    if (o->preset)
        av_dict_set(&s->enc_opts, "preset", o->preset, 0);
    av_dict_copy(&s->enc_opts, o->vopts, 0);
    enc->thread_count = o->threads;
    if (t->oc->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;          /* 复用器要全局头（MP4、MKV）：打开编码器之前告诉它（L15） */
    enc->flags |= AV_CODEC_FLAG_FRAME_DURATION;             /* 送给编码器的帧带着时长，它就把时长带到输出的包上 */

    ret = avcodec_open2(enc, codec, &s->enc_opts);
    if (ret < 0) {
        mt_averr(ret, "打不开视频编码器 %s（参数 %dx%d %s）", codec->name, enc->width, enc->height, av_get_pix_fmt_name(enc->pix_fmt));
        return ret;
    }
    if (av_dict_count(s->enc_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(s->enc_opts, NULL);

        mt_err("视频编码器 %s 不认识选项 %s（--preset、--crf、--vopt 里的名字）", codec->name, e->key);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int open_audio_encoder(Transcode *t, const Opts *o, TStream *s, const AVCodec *codec)
{
    AVCodecContext *enc;
    int ret;

    enc = avcodec_alloc_context3(codec);
    if (!enc) {
        mt_err("内存不够");
        return AVERROR(ENOMEM);
    }
    s->enc = enc;
    enc->sample_rate = av_buffersink_get_sample_rate(s->graph.sink);
    enc->sample_fmt  = av_buffersink_get_format(s->graph.sink);
    if ((ret = av_buffersink_get_ch_layout(s->graph.sink, &enc->ch_layout)) < 0) {
        mt_averr(ret, "读滤镜图出口的声道布局失败");
        return ret;
    }
    enc->time_base = (AVRational){ 1, enc->sample_rate };
    if (o->ab > 0)
        enc->bit_rate = (int64_t)o->ab;
    av_dict_copy(&s->enc_opts, o->aopts, 0);
    if (t->oc->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    enc->flags |= AV_CODEC_FLAG_FRAME_DURATION;

    ret = avcodec_open2(enc, codec, &s->enc_opts);
    if (ret < 0) {
        mt_averr(ret, "打不开音频编码器 %s", codec->name);
        return ret;
    }
    if (av_dict_count(s->enc_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(s->enc_opts, NULL);

        mt_err("音频编码器 %s 不认识选项 %s（--aopt 里的名字）", codec->name, e->key);
        return AVERROR(EINVAL);
    }
    /* 音频编码器要求每帧的采样数固定（AAC 是 1024），打开以后才知道，这时告诉滤镜图出口，它会重新切帧（L14） */
    if (enc->frame_size > 0 && !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        av_buffersink_set_frame_size(s->graph.sink, enc->frame_size);
    return 0;
}

/* 一路流：解码器、滤镜图、编码器、输出流。失败时已经打印好错误 */
static int setup_stream(Transcode *t, const Opts *o, TStream *s)
{
    const AVCodec *codec;
    int use_hw = 0;
    char desc[1024];
    int ret;

    s->frame  = av_frame_alloc();
    s->fframe = av_frame_alloc();
    s->pkt    = av_packet_alloc();
    if (!s->frame || !s->fframe || !s->pkt) {
        mt_err("内存不够");
        return AVERROR(ENOMEM);
    }
    s->rescale_last = AV_NOPTS_VALUE;

    codec = s->type == AVMEDIA_TYPE_VIDEO ? pick_video_encoder(t, o) : pick_audio_encoder(t, o);
    if (!codec)
        return AVERROR_ENCODER_NOT_FOUND;
    if (s->type == AVMEDIA_TYPE_VIDEO && is_vaapi_encoder(codec)) {
        use_hw = 1;
        if (!t->hw_dev) {
            mt_err("编码器 %s 需要 VAAPI 设备，加 --hw vaapi（没有显卡的机器上用不了）", codec->name);
            return AVERROR(EINVAL);
        }
    }
    if (s->type == AVMEDIA_TYPE_AUDIO && o->ar && (ret = check_audio_rate(codec, o->ar)) < 0)
        return ret;

    if (mt_open_decoder(s->ist, 0, &s->dec) < 0)
        return AVERROR_DECODER_NOT_FOUND;
    if (s->type == AVMEDIA_TYPE_AUDIO && s->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&s->dec->ch_layout, s->dec->ch_layout.nb_channels);

    if (s->type == AVMEDIA_TYPE_VIDEO) {
        const AVRational r = s->ist->r_frame_rate, avg = s->ist->avg_frame_rate;

        /* 可变帧率：平均帧率和流上标的“最小公倍”帧率差得多（vfr.mp4 平均 15.4、标 25）。--fps 指定了就是固定的 */
        s->vfr = !o->fps && r.num > 0 && avg.num > 0 && fabs(av_q2d(r) - av_q2d(avg)) > 0.02 * av_q2d(r);
        build_video_desc(t, o, s, use_hw, desc, sizeof(desc));
        s->graph.hw_device = use_hw ? t->hw_dev : NULL;
        ret = mt_graph_init(&s->graph, s->dec, av_guess_frame_rate(t->ic, s->ist, NULL), desc, codec);
    } else {
        build_audio_desc(t, o, desc, sizeof(desc));
        ret = mt_graph_init(&s->graph, s->dec, (AVRational){ 0, 1 }, desc, codec);
    }
    if (ret < 0)
        return ret;
    if ((ret = s->type == AVMEDIA_TYPE_VIDEO ? open_video_encoder(t, o, s, codec) : open_audio_encoder(t, o, s, codec)) < 0)
        return ret;

    s->ost = avformat_new_stream(t->oc, NULL);
    if (!s->ost) {
        mt_err("内存不够");
        return AVERROR(ENOMEM);
    }
    if ((ret = avcodec_parameters_from_context(s->ost->codecpar, s->enc)) < 0) {
        mt_averr(ret, "处理%s流参数失败", s->name);
        return ret;
    }
    s->ost->time_base = s->enc->time_base;      /* 只是建议，写完文件头以后以复用器给的为准 */
    s->ost->sample_aspect_ratio = s->enc->sample_aspect_ratio;
    /* --no-rotate：像素没转，旋转信息要跟着带到输出流上 */
    if (s->type == AVMEDIA_TYPE_VIDEO && o->no_rotate) {
        const AVPacketSideData *sd = av_packet_side_data_get(s->ist->codecpar->coded_side_data,
                                                             s->ist->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);

        if (sd && !av_packet_side_data_add(&s->ost->codecpar->coded_side_data, &s->ost->codecpar->nb_coded_side_data,
                                           AV_PKT_DATA_DISPLAYMATRIX, av_memdup(sd->data, sd->size), sd->size, 0)) {
            mt_err("内存不够");
            return AVERROR(ENOMEM);
        }
    }
    /* 语言等流元数据和标志一起带过去（patch 0075）；“是谁编码的”换成我们用的编码器 */
    av_dict_copy(&s->ost->metadata, s->ist->metadata, 0);
    s->ost->disposition = s->ist->disposition;
    {
        char ident[64];

        snprintf(ident, sizeof(ident), "%s %s", LIBAVCODEC_IDENT, codec->name);
        av_dict_set(&s->ost->metadata, "encoder", ident, 0);
    }
    return 0;
}

static void free_stream(TStream *s)
{
    avcodec_free_context(&s->dec);
    avcodec_free_context(&s->enc);
    mt_graph_free(&s->graph);
    av_dict_free(&s->enc_opts);
    av_frame_free(&s->frame);
    av_frame_free(&s->fframe);
    av_packet_free(&s->pkt);
}

/* ------------------------------------------------------------------------ */
/* 主流程                                                                    */
/* ------------------------------------------------------------------------ */

static int process_packet(Transcode *t, AVPacket *pkt)
{
    TStream *s;

    if (t->v.enabled && pkt->stream_index == t->v.ist->index)
        s = &t->v;
    else if (t->a.enabled && pkt->stream_index == t->a.ist->index)
        s = &t->a;
    else
        return 0;               /* 不处理的流，丢掉 */
    /* 结束时间：dts 单调增加，dts 到了结束时间以后的包解出来的帧都在结束时间之外；用 dts 不用 pts，因为有 B 帧时 pts 乱序（L8、L16） */
    if (t->end > 0) {
        const int64_t ts = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;

        if (ts != AV_NOPTS_VALUE && ts * av_q2d(s->ist->time_base) >= t->end) {
            s->past_end = 1;
            return 0;
        }
    }
    return decode_packet(t, s, pkt);
}

static void print_stream_summary(const TStream *s, const AVCodecParameters *in)
{
    char layout[64];

    if (s->type == AVMEDIA_TYPE_VIDEO) {
        printf("  视频：%s %dx%d → %s %dx%d %s，%" PRId64 " 帧", avcodec_get_name(in->codec_id), in->width, in->height,
               s->enc->codec->name, s->enc->width, s->enc->height, av_get_pix_fmt_name(s->enc->pix_fmt), s->frames_out);
        if (s->vfr)
            printf("，可变帧率");
        else if (s->enc->framerate.num > 0)
            printf("，%.3f 帧/秒", av_q2d(s->enc->framerate));
    } else {
        av_channel_layout_describe(&s->enc->ch_layout, layout, sizeof(layout));
        printf("  音频：%s %d Hz → %s %d Hz %s，%" PRId64 " 个包", avcodec_get_name(in->codec_id), in->sample_rate,
               s->enc->codec->name, s->enc->sample_rate, layout, s->packets);
    }
    if (s->end_time > 0)
        printf("，约 %.0f kb/s", s->bytes * 8 / s->end_time / 1000);
    printf("\n");
}

int cmd_transcode(int argc, char **argv)
{
    enum { OPT_VCODEC = 256, OPT_ACODEC, OPT_CRF, OPT_VB, OPT_AB, OPT_PRESET, OPT_GOP, OPT_VOPT, OPT_AOPT, OPT_THREADS, OPT_HW,
           OPT_SIZE, OPT_FPS, OPT_AR, OPT_AC, OPT_VF, OPT_AF, OPT_NOROT, OPT_NOV, OPT_NOA, OPT_FAST };
    static const struct option longopts[] = {
        { "vcodec", required_argument, NULL, OPT_VCODEC }, { "acodec", required_argument, NULL, OPT_ACODEC },
        { "crf", required_argument, NULL, OPT_CRF },       { "vb", required_argument, NULL, OPT_VB },
        { "ab", required_argument, NULL, OPT_AB },         { "preset", required_argument, NULL, OPT_PRESET },
        { "gop", required_argument, NULL, OPT_GOP },       { "vopt", required_argument, NULL, OPT_VOPT },
        { "aopt", required_argument, NULL, OPT_AOPT },     { "threads", required_argument, NULL, OPT_THREADS },
        { "hw", required_argument, NULL, OPT_HW },         { "width", required_argument, NULL, 'w' },
        { "size", required_argument, NULL, OPT_SIZE },     { "fps", required_argument, NULL, OPT_FPS },
        { "ar", required_argument, NULL, OPT_AR },         { "ac", required_argument, NULL, OPT_AC },
        { "vf", required_argument, NULL, OPT_VF },         { "af", required_argument, NULL, OPT_AF },
        { "no-rotate", no_argument, NULL, OPT_NOROT },     { "no-video", no_argument, NULL, OPT_NOV },
        { "no-audio", no_argument, NULL, OPT_NOA },        { "start", required_argument, NULL, 's' },
        { "end", required_argument, NULL, 'e' },           { "duration", required_argument, NULL, 'd' },
        { "format", required_argument, NULL, 'f' },        { "faststart", no_argument, NULL, OPT_FAST },
        { "help", no_argument, NULL, 'h' },                { NULL, 0, NULL, 0 }
    };
#define USAGE(...) do { rc = mt_usage_err("transcode", __VA_ARGS__); goto end_noinput; } while (0)
    Opts o = { .start = -1, .end = -1, .dur = -1, .hw = "none" };
    Transcode t = { 0 };
    AVDictionary *mux_opts = NULL;
    const char *in_path, *out_path;
    int vi = -1, ai = -1, header_written = 0, created = 0, opt, ret = 0, rc = MT_FAIL;
    double file_start = 0;
    int64_t t_begin;
    char *endp;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "w:s:e:d:f:yh", longopts, NULL)) != -1) {
        switch (opt) {
        case OPT_VCODEC: o.vcodec = optarg; break;
        case OPT_ACODEC: o.acodec = optarg; break;
        case OPT_CRF:
            if (av_strtod(optarg, &endp) < 0 || *endp || !*optarg)
                USAGE("--crf %s 不是数字", optarg);
            o.crf = optarg;
            break;
        case OPT_VB:
            o.vb = av_strtod(optarg, &endp);
            if (*endp || o.vb < 1000 || o.vb > 1e9)
                USAGE("视频码率 %s 不对，写成 1500k 或 2M", optarg);
            break;
        case OPT_AB:
            o.ab = av_strtod(optarg, &endp);
            if (*endp || o.ab < 8000 || o.ab > 10e6)
                USAGE("音频码率 %s 不对，写成 96k 或 128000", optarg);
            break;
        case OPT_PRESET: o.preset = optarg; break;
        case OPT_GOP:
            o.gop = atoi(optarg);
            if (o.gop < 1 || o.gop > 100000)
                USAGE("关键帧间隔 %s 要在 1 到 100000 之间", optarg);
            break;
        case OPT_VOPT:
        case OPT_AOPT: {
            const char *eq = strchr(optarg, '=');
            char *key;

            if (!eq || eq == optarg)
                USAGE("编码器选项 %s 要写成 名=值", optarg);
            key = av_strndup(optarg, eq - optarg);
            if (!key)
                USAGE("内存不够");
            av_dict_set(opt == OPT_VOPT ? &o.vopts : &o.aopts, key, eq + 1, 0);
            av_free(key);
            break;
        }
        case OPT_THREADS:
            o.threads = atoi(optarg);
            if (o.threads < 0 || o.threads > 64 || (o.threads == 0 && strcmp(optarg, "0")))
                USAGE("线程数 %s 要在 0 到 64 之间", optarg);
            break;
        case OPT_HW:
            if (strcmp(optarg, "none") && strcmp(optarg, "auto") && strcmp(optarg, "vaapi"))
                USAGE("--hw %s 不对，只有 none、auto、vaapi", optarg);
            o.hw = optarg;
            break;
        case 'w':
            o.width = atoi(optarg);
            if (o.width < 16 || o.width > 8192)
                USAGE("宽 %s 要在 16 到 8192 之间", optarg);
            break;
        case OPT_SIZE: {
            char x;

            if (sscanf(optarg, "%dx%d%c", &o.box_w, &o.box_h, &x) != 2 || o.box_w < 16 || o.box_h < 16 || o.box_w > 8192 || o.box_h > 8192)
                USAGE("尺寸 %s 要写成 宽x高，各在 16 到 8192 之间", optarg);
            break;
        }
        case OPT_FPS:
            o.fps = atoi(optarg);
            if (o.fps < 1 || o.fps > 240)
                USAGE("帧率 %s 要在 1 到 240 之间", optarg);
            break;
        case OPT_AR:
            o.ar = atoi(optarg);
            if (o.ar < 1000 || o.ar > 384000)
                USAGE("采样率 %s 要在 1000 到 384000 之间", optarg);
            break;
        case OPT_AC:
            o.ac = atoi(optarg);
            if (o.ac < 1 || o.ac > 8)
                USAGE("声道数 %s 要在 1 到 8 之间", optarg);
            break;
        case OPT_VF: o.vf = optarg; break;
        case OPT_AF: o.af = optarg; break;
        case OPT_NOROT: o.no_rotate = 1; break;
        case OPT_NOV: o.no_video = 1; break;
        case OPT_NOA: o.no_audio = 1; break;
        case OPT_FAST: o.faststart = 1; break;
        case 's':
            if (mt_parse_time(optarg, &o.start) < 0)
                USAGE("起点 %s 不是时间", optarg);
            break;
        case 'e':
            if (mt_parse_time(optarg, &o.end) < 0)
                USAGE("终点 %s 不是时间", optarg);
            break;
        case 'd':
            if (mt_parse_time(optarg, &o.dur) < 0 || o.dur <= 0)
                USAGE("时长 %s 不是正的时间", optarg);
            break;
        case 'f': o.fmt_name = optarg; break;
        case 'y': o.overwrite = 1; break;
        case 'h': usage(stdout); rc = MT_OK; goto end_noinput;
        default:  USAGE("不认识的选项");
        }
    }
    if (argc - optind != 2)
        USAGE("需要一个输入和一个输出");
    in_path  = argv[optind];
    out_path = argv[optind + 1];
    if (o.end >= 0 && o.dur >= 0)
        USAGE("-e 和 -d 只能给一个");
    if (o.dur >= 0)
        o.end = (o.start >= 0 ? o.start : 0) + o.dur;
    if (o.end >= 0 && o.end <= (o.start >= 0 ? o.start : 0))
        USAGE("终点必须在起点之后");
    if (o.width && o.box_w)
        USAGE("-w 和 --size 只能给一个");
    if (o.no_video && o.no_audio)
        USAGE("--no-video 和 --no-audio 不能同时用");
    if (!strcmp(in_path, out_path)) {
        mt_err("输入和输出是同一个文件，会把输入写坏：%s", in_path);
        goto end_noinput;
    }
    if (!o.overwrite && access(out_path, F_OK) == 0) {
        mt_err("输出文件 %s 已经存在，加 -y 覆盖它", out_path);
        goto end_noinput;
    }

    t_begin = av_gettime_relative();
    if (mt_open_input(&t.ic, in_path) < 0)
        goto end;

    /* ---- 选流 ---- */
    if (!o.no_video) {
        vi = av_find_best_stream(t.ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (vi >= 0 && (t.ic->streams[vi]->disposition & AV_DISPOSITION_ATTACHED_PIC))
            vi = -1;                    /* 封面图不是视频 */
    }
    if (!o.no_audio)
        ai = av_find_best_stream(t.ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (vi < 0 && ai < 0) {
        mt_err("%s 里没有可以转码的流（视频或音频）", in_path);
        goto end;
    }
    /* 开始和结束是相对输入文件开头的，但 TS 这类文件的时间戳不从 0 开始，要加上起始时间（L3、L16） */
    if (t.ic->start_time != AV_NOPTS_VALUE)
        file_start = t.ic->start_time / (double)AV_TIME_BASE;
    if (o.start > 0 && t.ic->duration != AV_NOPTS_VALUE && o.start >= t.ic->duration / (double)AV_TIME_BASE) {
        mt_err("起点 %.3f 秒超过了文件的长度 %.3f 秒", o.start, t.ic->duration / (double)AV_TIME_BASE);
        goto end;
    }
    t.start = file_start + FFMAX(o.start, 0);   /* 即使没有 -s，文件不从 0 开始时 start 也不是 0：滤镜图把时间戳整体平移，输出才从 0 开始 */
    t.end   = o.end >= 0 ? file_start + o.end : 0;
    if (o.end >= 0) {
        t.total = o.end - FFMAX(o.start, 0);
        t.limit = t.total;
    }
    else if (t.ic->duration != AV_NOPTS_VALUE)
        t.total = t.ic->duration / (double)AV_TIME_BASE - FFMAX(o.start, 0);
    if (t.total > 0 && t.ic->duration != AV_NOPTS_VALUE)
        t.total = FFMIN(t.total, t.ic->duration / (double)AV_TIME_BASE - FFMAX(o.start, 0));

    /* ---- 输出容器 ---- */
    ret = avformat_alloc_output_context2(&t.oc, NULL, o.fmt_name, out_path);
    if (ret < 0) {
        mt_averr(ret, "不知道 %s 该用什么格式（扩展名认不出来，用 -f 指定）", out_path);
        goto end;
    }
    /* ffmpeg 命令行默认 -muxdelay 0.7：TS 的第一个 PCR 比第一帧早 0.7 秒（L17 讲过），播放器才有缓冲时间；不设的话 TS 的起点是 0 */
    t.oc->max_delay = (int)(0.7 * AV_TIME_BASE);
    if (vi >= 0 && audio_only_output(out_path) && !o.fmt_name) {
        mt_warn("输出 %s 是只放音频的格式，丢掉视频", out_path);
        vi = -1;
    }
    if (vi < 0 && ai < 0) {
        mt_err("%s 里没有音频，而输出 %s 只放音频", in_path, out_path);
        goto end;
    }

    /* ---- 硬件设备（只用于编码） ---- */
    if (strcmp(o.hw, "none") && vi >= 0) {
        const enum AVHWDeviceType type = av_hwdevice_find_type_by_name("vaapi");

        ret = type == AV_HWDEVICE_TYPE_NONE ? AVERROR(ENOSYS) : av_hwdevice_ctx_create(&t.hw_dev, type, NULL, NULL, 0);
        if (ret < 0) {
            if (!strcmp(o.hw, "vaapi")) {
                mt_averr(ret, "打不开 VAAPI 设备（没有显卡、没有 /dev/dri，或者驱动没装）");
                goto end;
            }
            mt_warn("没有可用的 VAAPI 设备（%s），改用软件编码", av_err2str(ret));
            t.hw_dev = NULL;
        }
    }

    /* ---- 每一路流：解码器、滤镜图、编码器、输出流 ---- */
    t.v.name = "视频"; t.v.type = AVMEDIA_TYPE_VIDEO;
    t.a.name = "音频"; t.a.type = AVMEDIA_TYPE_AUDIO;
    if (vi >= 0) {
        t.v.enabled = 1;
        t.v.ist = t.ic->streams[vi];
        if (setup_stream(&t, &o, &t.v) < 0)
            goto end;
    }
    if (ai >= 0) {
        t.a.enabled = 1;
        t.a.ist = t.ic->streams[ai];
        if (setup_stream(&t, &o, &t.a) < 0)
            goto end;
    }
    /* 输入的全局元数据（标题等）也带过去；MP4 的文件类型标签只描述输入文件自己，不拷贝（L16） */
    av_dict_copy(&t.oc->metadata, t.ic->metadata, 0);
    av_dict_set(&t.oc->metadata, "major_brand", NULL, 0);
    av_dict_set(&t.oc->metadata, "minor_version", NULL, 0);
    av_dict_set(&t.oc->metadata, "compatible_brands", NULL, 0);

    /* ---- 打开输出，写文件头 ---- */
    if (!(t.oc->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&t.oc->pb, out_path, AVIO_FLAG_WRITE)) < 0) {
            mt_averr(ret, "打不开输出 %s", out_path);
            goto end;
        }
        created = 1;
    }
    if (o.faststart)
        av_dict_set(&mux_opts, "movflags", "+faststart", 0);
    if ((ret = avformat_write_header(t.oc, &mux_opts)) < 0) {
        mt_averr(ret, "写文件头失败（输出格式 %s）", t.oc->oformat->name);
        goto end;
    }
    header_written = 1;
    if (av_dict_count(mux_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(mux_opts, NULL);

        mt_err("复用器 %s 不认识选项 %s（--faststart 只对 MP4、MOV 有用）", t.oc->oformat->name, e->key);
        goto end;
    }

    /* ---- 定位到起点之前一点：精确裁剪要从起点之前最近的关键帧开始解码，解出来的帧里起点之前的由 trim 丢掉（L16）；
     *      有音频时再往前 1 秒，让 AAC 这类有重叠的编码在起点处解对 ---- */
    if (o.start > 0) {
        const int64_t target = (int64_t)(FFMAX(file_start, t.start - (ai >= 0 ? PREROLL_SECONDS : 0)) * AV_TIME_BASE);

        if ((ret = avformat_seek_file(t.ic, -1, INT64_MIN, target, target, 0)) < 0) {
            mt_averr(ret, "定位到 %.3f 秒失败", o.start);
            goto end;
        }
    }

    /* ---- 主循环 ---- */
    {
        AVPacket *pkt = av_packet_alloc();

        if (!pkt) {
            mt_err("内存不够");
            goto end;
        }
        mt_progress_start(&t.prog, "转码", t.total);
        while ((ret = av_read_frame(t.ic, pkt)) >= 0) {
            const int r = process_packet(&t, pkt);

            av_packet_unref(pkt);
            if (r < 0) {
                av_packet_free(&pkt);
                goto end;
            }
            if ((!t.v.enabled || t.v.past_end) && (!t.a.enabled || t.a.past_end))
                break;              /* 所有处理的流都过了结束时间 */
        }
        av_packet_free(&pkt);
    }
    if (ret < 0 && ret != AVERROR_EOF)
        mt_warn("读输入到一半出错了：%s。已经转换的部分保留", av_err2str(ret));

    /* ---- 冲刷：每一路依次从上游到下游 ---- */
    if (t.v.enabled && flush_stream(&t, &t.v) < 0)
        goto end;
    if (t.a.enabled && flush_stream(&t, &t.a) < 0)
        goto end;
    mt_progress_end(&t.prog, FFMAX(t.v.end_time, t.a.end_time), t.v.frames_out);

    /* 某一路一个包也没有：多半是起点超过了输入的长度，或者范围太短。输出是个空壳，要报错 */
    if ((t.v.enabled && !t.v.packets) || (t.a.enabled && !t.a.packets)) {
        mt_err("%s一个包也没有写出来（起点超过了输入的长度，或者范围太短？）", t.v.enabled && !t.v.packets ? "视频" : "音频");
        goto end;
    }
    header_written = 0;
    if ((ret = av_write_trailer(t.oc)) < 0) {
        mt_averr(ret, "写文件尾失败（MP4 的索引在文件尾里，写不进去文件是打不开的）");
        goto end;
    }
    if (!(t.oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_closep(&t.oc->pb);
        if (ret < 0) {
            mt_averr(ret, "关闭输出文件失败");
            goto end;
        }
    }

    {
        /* 自检：把写好的输出文件重新打开，读它的时长（ffprobe 的 format duration 就是这个值），和要求的比（验收标准：差不到 0.1 秒） */
        const double elapsed = (av_gettime_relative() - t_begin) / 1e6;
        AVFormatContext *oic = NULL;
        double out_dur = -1;

        if (avformat_open_input(&oic, out_path, NULL, NULL) >= 0) {
            if (avformat_find_stream_info(oic, NULL) >= 0 && oic->duration != AV_NOPTS_VALUE)
                out_dur = oic->duration / (double)AV_TIME_BASE;
            avformat_close_input(&oic);
        }

        printf("转码完成：%s → %s（%s）\n", in_path, out_path, t.oc->oformat->name);
        if (t.v.enabled)
            print_stream_summary(&t.v, t.v.ist->codecpar);
        if (t.a.enabled)
            print_stream_summary(&t.a, t.a.ist->codecpar);
        if (out_dur >= 0) {
            printf("  时长 %.3f 秒", out_dur);
            if (t.total > 0)
                printf("（要求的 %.3f 秒，相差 %.3f 秒）", t.total, fabs(out_dur - t.total));
            if (t.total > 0 && fabs(out_dur - t.total) > 0.1)
                mt_warn("输出时长 %.3f 秒和要求的 %.3f 秒相差超过 0.1 秒", out_dur, t.total);
        } else {
            printf("  时长未知");
        }
        if (t.v.enabled && is_vaapi_encoder(t.v.enc->codec))
            printf("，视频用 VAAPI 硬件编码");
        printf("，用时 %.2f 秒", elapsed);
        if (elapsed > 0 && out_dur > 0)
            printf("（%.1f 倍速）", out_dur / elapsed);
        printf("\n");
    }
    if (t.v.decode_errors + t.a.decode_errors)
        mt_warn("解码时遇到 %" PRId64 " 个出错的包，对应的画面或声音可能有花屏、杂音或缺一段", t.v.decode_errors + t.a.decode_errors);
    rc = MT_OK;

end:
    if (header_written)
        av_write_trailer(t.oc);
    if (t.oc && !(t.oc->oformat->flags & AVFMT_NOFILE) && t.oc->pb)
        avio_closep(&t.oc->pb);
    if (rc != MT_OK && created)
        unlink(out_path);               /* 失败时不留下一个看起来像成功的坏文件 */
    free_stream(&t.v);
    free_stream(&t.a);
    av_buffer_unref(&t.hw_dev);
    avformat_free_context(t.oc);
    avformat_close_input(&t.ic);
end_noinput:
    av_dict_free(&mux_opts);
    av_dict_free(&o.vopts);
    av_dict_free(&o.aopts);
    return rc;
}
