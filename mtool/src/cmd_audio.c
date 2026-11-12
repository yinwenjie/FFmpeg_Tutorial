/*
 * mtool audio：提取音频，转成指定的采样率、声道数和编码（WAV、AAC、FLAC、MP2）。
 *
 *   mtool audio [-r 采样率] [-c 声道数] [-b 码率] [--codec 编码器] [-s 起点] [-e 终点 | -d 时长]
 *               [--track 序号] [-f 格式] [-y] <输入> <输出>
 *
 * 来自 L06（解码音频）、L10（重采样：采样率、声道、采样格式）、L14（固定帧长）、L17（音频转码：解码 → 滤镜图 → 编码 → 复用）。
 * 做法：
 *   1. 先按输出的扩展名（或 --codec）选编码器，再决定编码器的参数：采样率、声道布局、采样格式都要是编码器支持的；
 *   2. 解码出来的帧进一张滤镜图：[atrim 精确裁剪,] aformat 转成编码器要的格式（重采样、混声道都是图自己插入的 aresample）；
 *   3. 图的出口按编码器的帧长切帧（AAC 每帧 1024 个采样，PCM 没有要求）；
 *   4. 帧的时间戳自己数（已经编码的采样数），不用输入的时间戳（L17）；
 *   5. 文件尾要把解码器、滤镜图、编码器依次冲刷干净。
 * 截取是解码以后按采样裁的，精确到一个采样；为了让有重叠的编码（AAC、MP3）在起点处解对，从起点之前 1 秒开始解码。
 */
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/eval.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>

#include "mtool.h"

#define PREROLL_SECONDS 1.0

typedef struct Audio {
    AVFormatContext *ic, *oc;
    AVStream        *ist, *ost;
    AVCodecContext  *dec, *enc;
    MtGraph          graph;
    AVFrame         *frame, *filt;
    AVPacket        *pkt, *opkt;
    int64_t          next_pts;          /* 已经送去编码的采样数，也是下一帧的 pts（时间基 1/采样率） */
    int64_t          in_pts;            /* 下一个输入采样的 pts（时间基 1/采样率）：自己数出来的 */
    int              in_valid;          /* in_pts 已经有了（第一帧用容器里的 pts 定下来） */
    int              graph_done;        /* atrim 已经到了终点，滤镜图不再收帧 */
    int64_t          packets;           /* 写出的包数 */
    int64_t          decode_errors;
    Progress         prog;
} Audio;

static void usage(FILE *f)
{
    fprintf(f,
            "用法：mtool audio [选项] <输入> <输出>\n\n"
            "把输入里的一路音频解码，转成指定的格式，写成音频文件。输出的编码由扩展名决定：\n"
            "  .wav → PCM 16 位    .m4a .mp4 .aac → AAC    .flac → FLAC    .mp2 → MP2\n\n"
            "选项：\n"
            "  -r, --rate 采样率    输出的采样率（Hz），默认和输入相同（编码器不支持时取最接近的）\n"
            "  -c, --channels 数    输出的声道数（1 单声道，2 立体声，6 是 5.1），默认和输入相同\n"
            "  -b, --bitrate 码率   有损编码的码率，例如 96k、128000（WAV、FLAC 不用）\n"
            "      --codec 名字     指定编码器，例如 pcm_s24le、pcm_f32le、aac、flac、mp2\n"
            "  -s, --start 时间     从这里开始（秒，也可以写 1:30），精确到采样，默认从头开始\n"
            "  -e, --end 时间       到这里结束，从文件开头算起（和 -d 只能给一个）\n"
            "  -d, --duration 时间  时长，从 -s 算起\n"
            "      --track 序号     输入有多路音频时选第几路（从 0 数），默认选最好的一路\n"
            "  -f, --format 格式    指定输出容器，默认由扩展名决定\n"
            "  -y                   已经存在的输出覆盖掉\n"
            "  -h, --help           显示这段说明\n");
}

static enum AVCodecID codec_for_ext(const char *path)
{
    static const struct { const char *ext; enum AVCodecID id; } map[] = {
        { "wav", AV_CODEC_ID_PCM_S16LE }, { "m4a", AV_CODEC_ID_AAC }, { "mp4", AV_CODEC_ID_AAC },
        { "aac", AV_CODEC_ID_AAC },       { "flac", AV_CODEC_ID_FLAC }, { "mp2", AV_CODEC_ID_MP2 },
    };
    const char *dot = strrchr(path, '.');

    if (dot)
        for (size_t i = 0; i < FF_ARRAY_ELEMS(map); i++)
            if (!av_strcasecmp(dot + 1, map[i].ext))
                return map[i].id;
    return AV_CODEC_ID_NONE;
}

/* 把编码器支持的采样率、声道布局列成一句话，给错误提示用 */
static void list_rates(const int *rates, int n, char *buf, size_t size)
{
    size_t len = 0;

    buf[0] = '\0';
    for (int i = 0; i < n && len + 12 < size; i++)
        len += snprintf(buf + len, size - len, "%s%d", i ? "、" : "", rates[i]);
}

/*
 * 决定编码器的参数。用户给了就必须满足，不满足报错；没给就跟输入，输入的不支持时取最接近的。
 * 返回 0 成功；失败时已经打印好错误。
 */
static int choose_params(const AVCodec *enc, const AVCodecContext *dec, int want_rate, int want_ch,
                         int *rate, AVChannelLayout *layout, enum AVSampleFormat *fmt)
{
    const enum AVSampleFormat *fmts = NULL;
    const int *rates = NULL;
    const AVChannelLayout *layouts = NULL;
    char buf[256];
    int nf = 0, nr = 0, nl = 0, ret;

    /* 采样格式：编码器支持的里面，优先选和输入同样位宽（打包或平面都算）的，否则取第一个 */
    ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, (const void **)&fmts, &nf);
    if (ret < 0)
        goto err;
    *fmt = fmts && nf > 0 ? fmts[0] : dec->sample_fmt;
    for (int i = 0; fmts && i < nf; i++)
        if (av_get_packed_sample_fmt(fmts[i]) == av_get_packed_sample_fmt(dec->sample_fmt)) {
            *fmt = fmts[i];
            break;
        }

    /* 采样率 */
    ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_SAMPLE_RATE, 0, (const void **)&rates, &nr);
    if (ret < 0)
        goto err;
    *rate = want_rate > 0 ? want_rate : dec->sample_rate;
    if (rates && nr > 0) {
        int best = rates[0], found = 0;

        for (int i = 0; i < nr; i++) {
            if (rates[i] == *rate)
                found = 1;
            if (abs(rates[i] - *rate) < abs(best - *rate))
                best = rates[i];
        }
        if (!found) {
            list_rates(rates, nr, buf, sizeof(buf));
            if (want_rate > 0) {
                mt_err("编码器 %s 不支持采样率 %d Hz，它支持：%s", enc->name, want_rate, buf);
                return AVERROR(EINVAL);
            }
            mt_warn("编码器 %s 不支持输入的采样率 %d Hz，改成最接近的 %d Hz", enc->name, *rate, best);
            *rate = best;
        }
    }

    /* 声道布局 */
    ret = avcodec_get_supported_config(NULL, enc, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0, (const void **)&layouts, &nl);
    if (ret < 0)
        goto err;
    av_channel_layout_uninit(layout);
    if (want_ch > 0) {
        av_channel_layout_default(layout, want_ch);
    } else {
        ret = av_channel_layout_copy(layout, &dec->ch_layout);
        if (ret < 0)
            goto err;
        if (layout->order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(layout, layout->nb_channels);
    }
    if (layouts && nl > 0) {
        int found = 0;

        for (int i = 0; i < nl; i++)
            if (!av_channel_layout_compare(&layouts[i], layout))
                found = 1;
        if (!found) {
            char name[64];

            av_channel_layout_describe(layout, name, sizeof(name));
            mt_err("编码器 %s 不支持 %d 个声道（%s）", enc->name, layout->nb_channels, name);
            return AVERROR(EINVAL);
        }
    }
    return 0;
err:
    mt_averr(ret, "查询编码器 %s 支持的格式失败", enc->name);
    return ret;
}

/* 编码一帧（NULL 是冲刷），把出来的包全部写进输出。 */
static int encode_write(Audio *a, AVFrame *frame)
{
    int ret = avcodec_send_frame(a->enc, frame);

    if (ret < 0) {
        mt_averr(ret, "送帧给编码器 %s 失败", a->enc->codec->name);
        return ret;
    }
    while ((ret = avcodec_receive_packet(a->enc, a->opkt)) >= 0) {
        av_packet_rescale_ts(a->opkt, a->enc->time_base, a->ost->time_base);
        a->opkt->stream_index = a->ost->index;
        ret = av_interleaved_write_frame(a->oc, a->opkt);     /* 不管成败，包都已经交出去了 */
        if (ret < 0) {
            mt_averr(ret, "写输出失败");
            return ret;
        }
        a->packets++;
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    mt_averr(ret, "从编码器 %s 取包失败", a->enc->codec->name);
    return ret;
}

/* 从滤镜图出口取帧、数时间戳、编码，取到没有为止。 */
static int drain_graph(Audio *a)
{
    int ret;

    while ((ret = av_buffersink_get_frame(a->graph.sink, a->filt)) >= 0) {
        a->filt->pts = a->next_pts;
        a->next_pts += a->filt->nb_samples;
        ret = encode_write(a, a->filt);
        av_frame_unref(a->filt);
        if (ret < 0)
            return ret;
        mt_progress_update(&a->prog, a->next_pts / (double)a->enc->sample_rate, 0);
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    mt_averr(ret, "从滤镜图取帧失败");
    return ret;
}

/*
 * 从解码器取尽帧送进滤镜图。
 * 帧的 pts 换成 1/采样率 的时间基再进图：解码出来的 pts 以流的时间基为单位（AVI 里的 MP2 一帧是 1 个单位，
 * TS 是 1/90000），而 abuffer 和 atrim 都按 1/采样率 算，不换的话 atrim 的起点终点就全错了（L10、L17）。
 */
static int receive_frames(Audio *a)
{
    const AVRational tb = { 1, a->dec->sample_rate };
    int ret;

    while ((ret = avcodec_receive_frame(a->dec, a->frame)) >= 0) {
        if (a->graph_done) {
            av_frame_unref(a->frame);
            continue;
        }
        /*
         * 帧的 pts：只有第一帧（和时间戳跳变之后的第一帧）用容器里的，之后自己数。
         * 容器的时间基很粗时（FLV、MKV 是 1/1000，一个 AAC 帧 23.219 毫秒），每一帧的 pts 都被舍入到毫秒，
         * 换算回采样要差几个到二十几个采样；逐帧用它做裁剪，起点终点就差几个采样。
         */
        {
            int64_t pts = a->frame->pts == AV_NOPTS_VALUE ? AV_NOPTS_VALUE
                                                          : av_rescale_q(a->frame->pts, a->dec->pkt_timebase, tb);

            if (pts == AV_NOPTS_VALUE || (a->in_valid && llabs(pts - a->in_pts) <= a->dec->sample_rate / 50))
                pts = a->in_pts;            /* 没有 pts，或者和数出来的差不到 20 毫秒：连续的，用数出来的 */
            a->frame->pts = pts;
            a->in_valid = 1;
        }
        a->frame->time_base = tb;
        a->in_pts = a->frame->pts + a->frame->nb_samples;
        ret = av_buffersrc_add_frame_flags(a->graph.src, a->frame, AV_BUFFERSRC_FLAG_KEEP_REF);
        av_frame_unref(a->frame);
        if (ret == AVERROR_EOF) {           /* atrim 到了终点，后面的帧用不着了 */
            a->graph_done = 1;
            continue;
        }
        if (ret < 0) {
            mt_averr(ret, "把帧送进滤镜图失败");
            return ret;
        }
        if ((ret = drain_graph(a)) < 0)
            return ret;
    }
    return 0;
}

/* 无损编码（PCM、FLAC）没有码率可调 */
static int is_lossless(const AVCodec *c)
{
    const AVCodecDescriptor *d = avcodec_descriptor_get(c->id);

    return d && (d->props & AV_CODEC_PROP_LOSSLESS);
}

static void describe_decoder(const AVCodecContext *dec, char *buf, size_t size)
{
    char layout[64];

    av_channel_layout_describe(&dec->ch_layout, layout, sizeof(layout));
    snprintf(buf, size, "%s %d Hz %s", avcodec_get_name(dec->codec_id), dec->sample_rate, layout);
}

int cmd_audio(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "rate",     required_argument, NULL, 'r' },
        { "channels", required_argument, NULL, 'c' },
        { "bitrate",  required_argument, NULL, 'b' },
        { "codec",    required_argument, NULL, 'C' },
        { "start",    required_argument, NULL, 's' },
        { "end",      required_argument, NULL, 'e' },
        { "duration", required_argument, NULL, 'd' },
        { "track",    required_argument, NULL, 'T' },
        { "format",   required_argument, NULL, 'f' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    Audio a = { 0 };
    AVChannelLayout layout = { 0 };
    enum AVSampleFormat sample_fmt = AV_SAMPLE_FMT_NONE;
    const AVCodec *enc_codec;
    const char *in_path, *out_path, *fmt_name = NULL, *codec_name = NULL;
    char desc[512], layout_name[64], in_desc[160], out_desc[160];
    double start = -1, end = -1, dur = -1, bitrate = 0, file_start = 0, total;
    int want_rate = 0, want_ch = 0, track = -1, overwrite = 0;
    int out_rate = 0, aidx, created = 0, header_written = 0, opt, ret = 0, rc = MT_FAIL;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "r:c:b:s:e:d:f:yh", longopts, NULL)) != -1) {
        char *endp;

        switch (opt) {
        case 'r':
            want_rate = atoi(optarg);
            if (want_rate < 1000 || want_rate > 384000)
                return mt_usage_err("audio", "采样率 %s 要在 1000 到 384000 之间", optarg);
            break;
        case 'c':
            want_ch = atoi(optarg);
            if (want_ch < 1 || want_ch > 8)
                return mt_usage_err("audio", "声道数 %s 要在 1 到 8 之间", optarg);
            break;
        case 'b':
            bitrate = av_strtod(optarg, &endp);
            if (*endp || bitrate < 8000 || bitrate > 10e6)
                return mt_usage_err("audio", "码率 %s 不对，写成 96k 或 128000", optarg);
            break;
        case 'C': codec_name = optarg; break;
        case 's':
            if (mt_parse_time(optarg, &start) < 0)
                return mt_usage_err("audio", "起点 %s 不是时间", optarg);
            break;
        case 'e':
            if (mt_parse_time(optarg, &end) < 0)
                return mt_usage_err("audio", "终点 %s 不是时间", optarg);
            break;
        case 'd':
            if (mt_parse_time(optarg, &dur) < 0 || dur <= 0)
                return mt_usage_err("audio", "时长 %s 不是正的时间", optarg);
            break;
        case 'T':
            track = atoi(optarg);
            if (track < 0 || (track == 0 && strcmp(optarg, "0")))
                return mt_usage_err("audio", "音频流序号 %s 要是从 0 开始的整数", optarg);
            break;
        case 'f': fmt_name = optarg; break;
        case 'y': overwrite = 1; break;
        case 'h': usage(stdout); return MT_OK;
        default:  return mt_usage_err("audio", "不认识的选项");
        }
    }
    if (argc - optind != 2)
        return mt_usage_err("audio", "需要一个输入和一个输出");
    in_path  = argv[optind];
    out_path = argv[optind + 1];
    if (end >= 0 && dur >= 0)
        return mt_usage_err("audio", "-e 和 -d 只能给一个");
    if (dur >= 0)
        end = (start >= 0 ? start : 0) + dur;
    if (end >= 0 && end <= (start >= 0 ? start : 0))
        return mt_usage_err("audio", "终点必须在起点之后");
    if (!strcmp(in_path, out_path)) {
        mt_err("输入和输出是同一个文件，会把输入写坏：%s", in_path);
        return MT_FAIL;
    }
    if (!overwrite && access(out_path, F_OK) == 0) {
        mt_err("输出文件 %s 已经存在，加 -y 覆盖它", out_path);
        return MT_FAIL;
    }

    a.pkt  = av_packet_alloc();
    a.opkt = av_packet_alloc();
    a.frame = av_frame_alloc();
    a.filt  = av_frame_alloc();
    if (!a.pkt || !a.opkt || !a.frame || !a.filt) {
        mt_err("内存不够");
        goto end;
    }
    if (mt_open_input(&a.ic, in_path) < 0)
        goto end;

    /* ---- 选音频流，开解码器 ---- */
    if (track >= 0) {
        int n = 0;

        aidx = -1;
        for (unsigned i = 0; i < a.ic->nb_streams; i++)
            if (a.ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && n++ == track) {
                aidx = i;
                break;
            }
        if (aidx < 0) {
            mt_err("%s 里只有 %d 路音频流，没有第 %d 路", in_path, n, track);
            goto end;
        }
    } else {
        aidx = av_find_best_stream(a.ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (aidx < 0) {
            mt_err("%s 里没有音频流", in_path);
            goto end;
        }
    }
    a.ist = a.ic->streams[aidx];
    if (a.ic->start_time != AV_NOPTS_VALUE)
        file_start = a.ic->start_time / (double)AV_TIME_BASE;       /* TS 的时间不从 0 开始，用户给的时间从文件开头算（L03） */
    if (mt_open_decoder(a.ist, 0, &a.dec) < 0)
        goto end;
    if (a.dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&a.dec->ch_layout, a.dec->ch_layout.nb_channels);
    if (a.dec->sample_rate <= 0 || a.dec->ch_layout.nb_channels <= 0) {
        mt_err("%s 的音频流缺少采样率或声道数，没法转换", in_path);
        goto end;
    }
    a.in_pts = (int64_t)(file_start * a.dec->sample_rate);
    if (start > 0 && a.ic->duration != AV_NOPTS_VALUE && start >= a.ic->duration / (double)AV_TIME_BASE) {
        mt_err("起点 %.3f 秒超过了文件的长度 %.3f 秒", start, a.ic->duration / (double)AV_TIME_BASE);
        goto end;
    }

    /* ---- 输出容器和编码器 ---- */
    ret = avformat_alloc_output_context2(&a.oc, NULL, fmt_name, out_path);
    if (ret < 0) {
        mt_averr(ret, "不知道 %s 该用什么格式（扩展名认不出来，用 -f 指定）", out_path);
        goto end;
    }
    if (codec_name) {
        enc_codec = avcodec_find_encoder_by_name(codec_name);
        if (!enc_codec || enc_codec->type != AVMEDIA_TYPE_AUDIO) {
            mt_err("没有叫 %s 的音频编码器（ffmpeg -encoders 能看到这份 FFmpeg 有哪些）", codec_name);
            goto end;
        }
    } else {
        enum AVCodecID id = codec_for_ext(out_path);

        if (id == AV_CODEC_ID_NONE)         /* 扩展名认不出来（用 -f 指定了容器）：用容器默认的音频编码 */
            id = a.oc->oformat->audio_codec;
        if (id == AV_CODEC_ID_NONE) {
            mt_err("不知道 %s 该用什么音频编码，用 --codec 指定（扩展名认得 wav m4a mp4 aac flac mp2）", out_path);
            goto end;
        }
        enc_codec = avcodec_find_encoder(id);
        if (!enc_codec) {
            mt_err("这份 FFmpeg 没有 %s 的编码器", avcodec_get_name(id));
            goto end;
        }
    }
    if (avformat_query_codec(a.oc->oformat, enc_codec->id, FF_COMPLIANCE_NORMAL) == 0) {
        mt_err("输出格式 %s 装不下 %s 编码：换一个扩展名（或者 -f），或者换 --codec", a.oc->oformat->name, enc_codec->name);
        goto end;
    }
    if (!strcmp(a.oc->oformat->name, "wav") && strncmp(enc_codec->name, "pcm_", 4)) {
        mt_err("WAV 里只放 PCM，不放 %s：换一个扩展名（m4a、flac……），或者换 --codec pcm_s16le", enc_codec->name);
        goto end;
    }
    if (choose_params(enc_codec, a.dec, want_rate, want_ch, &out_rate, &layout, &sample_fmt) < 0)
        goto end;
    if (bitrate > 0 && is_lossless(enc_codec))
        mt_warn("%s 是无损编码，-b 不起作用", enc_codec->name);

    a.enc = avcodec_alloc_context3(enc_codec);
    if (!a.enc) {
        mt_err("内存不够");
        goto end;
    }
    a.enc->sample_rate = out_rate;
    a.enc->sample_fmt  = sample_fmt;
    a.enc->time_base   = (AVRational){ 1, out_rate };
    if ((ret = av_channel_layout_copy(&a.enc->ch_layout, &layout)) < 0)
        goto end_av;
    if (bitrate > 0)
        a.enc->bit_rate = (int64_t)bitrate;
    if (a.oc->oformat->flags & AVFMT_GLOBALHEADER)
        a.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;        /* 容器要 extradata 放在文件头里的（MP4、M4A），编码器要知道（L15） */
    if ((ret = avcodec_open2(a.enc, enc_codec, NULL)) < 0) {
        mt_averr(ret, "打不开编码器 %s", enc_codec->name);
        goto end;
    }

    a.ost = avformat_new_stream(a.oc, NULL);
    if (!a.ost) {
        mt_err("内存不够");
        goto end;
    }
    if ((ret = avcodec_parameters_from_context(a.ost->codecpar, a.enc)) < 0)
        goto end_av;
    a.ost->time_base = a.enc->time_base;
    av_dict_copy(&a.oc->metadata, a.ic->metadata, 0);
    av_dict_copy(&a.ost->metadata, a.ist->metadata, 0);
    av_dict_set(&a.ost->metadata, "encoder", NULL, 0);

    /* ---- 滤镜图：[裁剪,] 转成编码器要的格式；出口按编码器的帧长切 ---- */
    {
        int n = 0;

        av_channel_layout_describe(&layout, layout_name, sizeof(layout_name));
        if (start > 0 || end >= 0) {
            n += snprintf(desc + n, sizeof(desc) - n, "atrim=");
            if (start > 0)
                n += snprintf(desc + n, sizeof(desc) - n, "start=%.9f", file_start + start);
            if (end >= 0)
                n += snprintf(desc + n, sizeof(desc) - n, "%send=%.9f", start > 0 ? ":" : "", file_start + end);
            n += snprintf(desc + n, sizeof(desc) - n, ",");
        }
        snprintf(desc + n, sizeof(desc) - n, "aformat=sample_fmts=%s:sample_rates=%d:channel_layouts=%s",
                 av_get_sample_fmt_name(sample_fmt), out_rate, layout_name);
    }
    if (mt_graph_init(&a.graph, a.dec, (AVRational){ 0, 1 }, desc, enc_codec) < 0)
        goto end;
    if (a.enc->frame_size > 0 && !(enc_codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
        av_buffersink_set_frame_size(a.graph.sink, a.enc->frame_size);

    /* ---- 打开输出，写文件头 ---- */
    if (!(a.oc->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&a.oc->pb, out_path, AVIO_FLAG_WRITE)) < 0) {
            mt_averr(ret, "打不开输出 %s", out_path);
            goto end;
        }
        created = 1;
    }
    if ((ret = avformat_write_header(a.oc, NULL)) < 0) {
        mt_averr(ret, "写文件头失败（输出格式 %s）", a.oc->oformat->name);
        goto end;
    }
    header_written = 1;

    /*
     * ---- 定位：起点之前留 1 秒让解码器预热 ----
     * 只有时间基够细（一个刻度不超过一个采样）的流才定位：定位以后第一帧的时间要取容器里的，
     * FLV、MKV 的 1/1000 秒比一个采样（1/44100 秒）粗，取出来差几个到二十几个采样，截取就不是精确到采样了。
     * 这时从头解码，音频解码很快（AAC 是实时的几百倍），多花的时间可以忽略。
     */
    if (start > PREROLL_SECONDS && a.ist->time_base.num * (double)a.dec->sample_rate <= a.ist->time_base.den) {
        const int64_t ts = (int64_t)((file_start + start - PREROLL_SECONDS) * AV_TIME_BASE);

        if ((ret = av_seek_frame(a.ic, -1, ts, AVSEEK_FLAG_BACKWARD)) < 0) {
            mt_averr(ret, "定位到 %.3f 秒失败", start);
            goto end;
        }
        a.in_pts = (int64_t)((file_start + start - PREROLL_SECONDS) * a.dec->sample_rate);   /* 只给没有 pts 的帧用 */
        a.in_valid = 0;                         /* 定位以后第一帧的时间要重新取 */
    }

    total = end >= 0 ? end - FFMAX(start, 0) : (a.ic->duration != AV_NOPTS_VALUE ? a.ic->duration / (double)AV_TIME_BASE - FFMAX(start, 0) : 0);
    mt_progress_start(&a.prog, "音频", total);

    /* ---- 读包、解码、过滤、编码、写 ---- */
    while (!a.graph_done && (ret = av_read_frame(a.ic, a.pkt)) >= 0) {
        if (a.pkt->stream_index != aidx) {
            av_packet_unref(a.pkt);
            continue;
        }
        ret = avcodec_send_packet(a.dec, a.pkt);
        av_packet_unref(a.pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            a.decode_errors++;      /* 损坏的包：记下来，继续往后读 */
        if (receive_frames(&a) < 0)
            goto end;
    }
    if (!a.graph_done && ret != AVERROR_EOF) {
        mt_warn("读包到一半出错了：%s。已经转换的部分保留", av_err2str(ret));
    }
    /* 冲刷，顺序是解码器 → 滤镜图 → 编码器（L11、L17）：每一级吐出来的东西要交给下一级 */
    avcodec_send_packet(a.dec, NULL);
    if (receive_frames(&a) < 0)
        goto end;
    ret = av_buffersrc_add_frame_flags(a.graph.src, NULL, 0);
    if (ret < 0 && ret != AVERROR_EOF) {
        mt_averr(ret, "通知滤镜图输入结束失败");
        goto end;
    }
    if (drain_graph(&a) < 0 || encode_write(&a, NULL) < 0)
        goto end;
    mt_progress_end(&a.prog, a.next_pts / (double)out_rate, 0);

    if (a.next_pts == 0) {
        mt_err("没有输出任何采样（起点超过了音频的长度？）");
        goto end;
    }
    header_written = 0;
    if ((ret = av_write_trailer(a.oc)) < 0) {
        mt_averr(ret, "写文件尾失败");
        goto end;
    }

    describe_decoder(a.dec, in_desc, sizeof(in_desc));
    snprintf(out_desc, sizeof(out_desc), "%s %d Hz %s", enc_codec->name, out_rate, layout_name);
    printf("音频提取完成：%s → %s（%s）\n", in_path, out_path, a.oc->oformat->name);
    printf("  输入：%s，第 %d 路流\n", in_desc, aidx);
    printf("  输出：%s", out_desc);
    if (a.enc->bit_rate > 0 && !is_lossless(enc_codec))
        printf("，%" PRId64 " kb/s", a.enc->bit_rate / 1000);
    printf("\n  时长 %.3f 秒（%" PRId64 " 个采样），%" PRId64 " 个包\n", a.next_pts / (double)out_rate, a.next_pts, a.packets);
    if (a.decode_errors)
        mt_warn("解码时遇到 %" PRId64 " 个出错的包，对应的声音可能有杂音或缺一段", a.decode_errors);
    rc = MT_OK;
    goto end;

end_av:
    mt_averr(ret, "处理流参数失败");
end:
    if (header_written)
        av_write_trailer(a.oc);
    if (a.oc && !(a.oc->oformat->flags & AVFMT_NOFILE) && a.oc->pb)
        avio_closep(&a.oc->pb);
    if (rc != MT_OK && created)
        unlink(out_path);                       /* 失败时不留下一个看起来像成功的坏文件 */
    mt_graph_free(&a.graph);
    av_channel_layout_uninit(&layout);
    avcodec_free_context(&a.dec);
    avcodec_free_context(&a.enc);
    avformat_free_context(a.oc);
    avformat_close_input(&a.ic);
    av_frame_free(&a.frame);
    av_frame_free(&a.filt);
    av_packet_free(&a.pkt);
    av_packet_free(&a.opkt);
    return rc;
}
