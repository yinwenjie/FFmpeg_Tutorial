/*
 * mtool info：显示容器、流、GOP 和帧类型统计。
 *
 *   mtool info [--json] [--no-decode] <文件>
 *
 * 来自 L03（打开文件、读流信息）和 L08（帧类型、GOP、side data）。
 * 要把整个文件读一遍：数每一路流的包、关键帧，算 GOP 长度，判断是不是可变帧率；
 * 视频还要解码，数 I、P、B 帧和解码出错的次数（--no-decode 跳过解码，快得多，但没有帧类型）。
 * 不知道的值不输出，而不是输出一个假的。
 */
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

#include "mtool.h"

typedef struct StreamStats {
    int64_t packets, bytes, keyframes;
    int64_t since_key, gops, gop_min, gop_max, gop_sum;   /* GOP 长度按包数算，包是按解码顺序排的 */
    int64_t *pts;                                          /* 视频：每个包的显示时间，判断可变帧率用 */
    size_t   npts, cap_pts;
    AVCodecContext *dec;
    int64_t frames, frame_types[4], decode_errors, corrupt_frames;
    int     started;
} StreamStats;

static void usage(FILE *f)
{
    fprintf(f,
            "用法：mtool info [选项] <文件>\n\n"
            "显示容器、每一路流的参数，以及整个文件的包数、关键帧、GOP 长度、帧类型、是不是可变帧率。\n\n"
            "选项：\n"
            "  --json        输出 JSON（不加就输出给人看的文本）\n"
            "  --no-decode   不解码，只统计包（快，但没有帧类型和解码出错的统计）\n"
            "  -h, --help    显示这段说明\n");
}

static int cmp_i64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;

    return (x > y) - (x < y);
}

static void add_pts(StreamStats *st, int64_t ts)
{
    if (st->npts == st->cap_pts) {
        size_t n = st->cap_pts ? st->cap_pts * 2 : 1024;
        int64_t *p = av_realloc_array(st->pts, n, sizeof(*p));

        if (!p)
            return;         /* 内存不够就不判断可变帧率，其余统计照常 */
        st->pts = p;
        st->cap_pts = n;
    }
    st->pts[st->npts++] = ts;
}

/* 数解码器吐出来的帧。corrupt 标志是解码器自己发现码流有错时打的。 */
static void count_frames(AVCodecContext *dec, AVFrame *frame, StreamStats *st)
{
    int ret;

    while ((ret = avcodec_receive_frame(dec, frame)) >= 0) {
        st->frames++;
        switch (frame->pict_type) {
        case AV_PICTURE_TYPE_I: st->frame_types[0]++; break;
        case AV_PICTURE_TYPE_P: st->frame_types[1]++; break;
        case AV_PICTURE_TYPE_B: st->frame_types[2]++; break;
        default:                st->frame_types[3]++; break;
        }
        if ((frame->flags & AV_FRAME_FLAG_CORRUPT) || frame->decode_error_flags)
            st->corrupt_frames++;
        av_frame_unref(frame);
    }
    if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
        st->decode_errors++;
}

/* 视频旋转角度：显示矩阵里的值，和 ffprobe 显示的一样。没有矩阵返回 0。 */
static int stream_rotation(const AVStream *s)
{
    const AVPacketSideData *sd = av_packet_side_data_get(s->codecpar->coded_side_data,
                                                         s->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);

    if (!sd || sd->size < 9 * sizeof(int32_t))
        return 0;
    return (int)lrint(av_display_rotation_get((const int32_t *)sd->data));
}

static void out_rational(Out *o, const char *key, AVRational r)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "%d/%d", r.num, r.den);
    out_str(o, key, buf);
}

static void out_dict(Out *o, const char *key, const AVDictionary *d)
{
    const AVDictionaryEntry *e = NULL;

    if (!av_dict_count(d))
        return;
    out_obj_begin(o, key);
    while ((e = av_dict_iterate(d, e)))
        out_str(o, e->key, e->value);
    out_obj_end(o);
}

static void out_stream(Out *o, const AVStream *s, StreamStats *st, int decoded)
{
    const AVCodecParameters *par = s->codecpar;
    const char *profile = avcodec_profile_name(par->codec_id, par->profile);
    char buf[128];

    out_obj_begin(o, NULL);
    out_int(o, "index", s->index);
    out_str(o, "type", av_get_media_type_string(par->codec_type) ? av_get_media_type_string(par->codec_type) : "unknown");
    out_str(o, "codec", avcodec_get_name(par->codec_id));
    if (profile)
        out_str(o, "profile", profile);
    out_rational(o, "time_base", s->time_base);
    if (s->start_time != AV_NOPTS_VALUE)
        out_dbl(o, "start_time", s->start_time * av_q2d(s->time_base), 3);
    if (s->duration != AV_NOPTS_VALUE)
        out_dbl(o, "duration", s->duration * av_q2d(s->time_base), 3);
    if (par->bit_rate > 0)
        out_int(o, "bit_rate", par->bit_rate);
    if (s->nb_frames > 0)
        out_int(o, "nb_frames", s->nb_frames);

    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        const char *pix = av_get_pix_fmt_name(par->format);
        const int rot = stream_rotation(s);

        out_int(o, "width", par->width);
        out_int(o, "height", par->height);
        if (pix)
            out_str(o, "pix_fmt", pix);
        if (par->sample_aspect_ratio.num)
            out_rational(o, "sample_aspect_ratio", par->sample_aspect_ratio);
        if (s->avg_frame_rate.den && s->avg_frame_rate.num)
            out_dbl(o, "avg_frame_rate", av_q2d(s->avg_frame_rate), 3);
        if (s->r_frame_rate.den && s->r_frame_rate.num)
            out_dbl(o, "r_frame_rate", av_q2d(s->r_frame_rate), 3);
        out_int(o, "rotation", rot);
        if (par->color_range != AVCOL_RANGE_UNSPECIFIED)
            out_str(o, "color_range", av_color_range_name(par->color_range));
        if (par->video_delay)
            out_int(o, "reorder_delay", par->video_delay);
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        const char *fmt = av_get_sample_fmt_name(par->format);

        out_int(o, "sample_rate", par->sample_rate);
        out_int(o, "channels", par->ch_layout.nb_channels);
        if (av_channel_layout_describe(&par->ch_layout, buf, sizeof(buf)) > 0)
            out_str(o, "channel_layout", buf);
        if (fmt)
            out_str(o, "sample_fmt", fmt);
    }
    out_dict(o, "tags", s->metadata);

    out_obj_begin(o, "stats");
    out_int(o, "packets", st->packets);
    out_int(o, "bytes", st->bytes);
    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        out_int(o, "keyframes", st->keyframes);
        if (st->gops > 0) {
            out_obj_begin(o, "gop");
            out_int(o, "count", st->gops);
            out_int(o, "min", st->gop_min);
            out_dbl(o, "avg", (double)st->gop_sum / st->gops, 1);
            out_int(o, "max", st->gop_max);
            out_obj_end(o);
        }
        if (st->npts > 2) {
            int64_t mn = INT64_MAX, mx = 0, sum = 0;
            double tick;

            qsort(st->pts, st->npts, sizeof(*st->pts), cmp_i64);
            for (size_t i = 1; i < st->npts; i++) {
                const int64_t d = st->pts[i] - st->pts[i - 1];

                if (d <= 0)
                    continue;
                mn = FFMIN(mn, d);
                mx = FFMAX(mx, d);
                sum += d;
            }
            if (mx > 0) {
                const double mean = (double)sum / (st->npts - 1);

                tick = av_q2d(s->time_base);
                out_bool(o, "variable_frame_rate", (mx - mn) > FFMAX(2.0, 0.05 * mean));
                out_obj_begin(o, "frame_interval");
                out_dbl(o, "min", mn * tick, 4);
                out_dbl(o, "max", mx * tick, 4);
                out_obj_end(o);
            }
        }
        if (decoded) {
            out_int(o, "decoded_frames", st->frames);
            out_obj_begin(o, "frame_types");
            out_int(o, "I", st->frame_types[0]);
            out_int(o, "P", st->frame_types[1]);
            out_int(o, "B", st->frame_types[2]);
            if (st->frame_types[3])
                out_int(o, "other", st->frame_types[3]);
            out_obj_end(o);
            out_int(o, "decode_errors", st->decode_errors);
            out_int(o, "corrupt_frames", st->corrupt_frames);
        }
    }
    out_obj_end(o);     /* stats */
    out_obj_end(o);     /* 这一路流 */
}

int cmd_info(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "json",      no_argument, NULL, 'j' },
        { "no-decode", no_argument, NULL, 'n' },
        { "help",      no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int json = 0, decode = 1, opt, ret = 0;
    AVFormatContext *ic = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    StreamStats *stats = NULL;
    const char *path;
    Out out;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'j': json = 1; break;
        case 'n': decode = 0; break;
        case 'h': usage(stdout); return MT_OK;
        default:  return mt_usage_err("info", "不认识的选项");
        }
    }
    if (argc - optind != 1)
        return mt_usage_err("info", "需要且只需要一个输入文件");
    path = argv[optind];

    if (mt_open_input(&ic, path) < 0)
        return MT_FAIL;
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    stats = av_calloc(ic->nb_streams, sizeof(*stats));
    if (!pkt || !frame || !stats) {
        mt_err("内存不够");
        ret = MT_FAIL;
        goto end;
    }

    /* 视频流各开一个解码器。没有解码器就只统计包。 */
    for (unsigned i = 0; decode && i < ic->nb_streams; i++) {
        const AVStream *s = ic->streams[i];
        const AVCodec *codec;

        if (s->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || (s->disposition & AV_DISPOSITION_ATTACHED_PIC))
            continue;
        codec = avcodec_find_decoder(s->codecpar->codec_id);
        if (!codec) {
            mt_warn("没有 %s 的解码器，这一路不统计帧类型", avcodec_get_name(s->codecpar->codec_id));
            continue;
        }
        stats[i].dec = avcodec_alloc_context3(codec);
        if (!stats[i].dec || avcodec_parameters_to_context(stats[i].dec, s->codecpar) < 0 ||
            avcodec_open2(stats[i].dec, codec, NULL) < 0) {
            mt_warn("打不开 %s 的解码器，这一路不统计帧类型", codec->name);
            avcodec_free_context(&stats[i].dec);
        }
    }

    /* 把整个文件读一遍。读包出错（文件被截断或损坏）不算失败：报告读到的部分，并在最后说明。 */
    while ((ret = av_read_frame(ic, pkt)) >= 0) {
        StreamStats *st = &stats[pkt->stream_index];
        const AVStream *s = ic->streams[pkt->stream_index];

        st->packets++;
        st->bytes += pkt->size;
        if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (pkt->flags & AV_PKT_FLAG_KEY) {
                st->keyframes++;
                if (st->started) {
                    st->gops++;
                    st->gop_sum += st->since_key;
                    st->gop_min = st->gops == 1 ? st->since_key : FFMIN(st->gop_min, st->since_key);
                    st->gop_max = FFMAX(st->gop_max, st->since_key);
                }
                st->started   = 1;
                st->since_key = 0;
            }
            if (st->started)
                st->since_key++;
            if (pkt->pts != AV_NOPTS_VALUE)
                add_pts(st, pkt->pts);
            else if (pkt->dts != AV_NOPTS_VALUE)
                add_pts(st, pkt->dts);
            if (st->dec) {
                const int r = avcodec_send_packet(st->dec, pkt);

                if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_EOF)
                    st->decode_errors++;
                count_frames(st->dec, frame, st);
            }
        }
        av_packet_unref(pkt);
    }
    if (ret != AVERROR_EOF)
        mt_warn("读包到一半出错了：%s。下面的统计只包含出错之前读到的部分", av_err2str(ret));

    /* 最后一个 GOP 没有“下一个关键帧”来结束它，按到文件尾的包数算。 */
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        StreamStats *st = &stats[i];

        if (st->started) {
            st->gops++;
            st->gop_sum += st->since_key;
            st->gop_min = st->gops == 1 ? st->since_key : FFMIN(st->gop_min, st->since_key);
            st->gop_max = FFMAX(st->gop_max, st->since_key);
        }
        if (st->dec) {
            avcodec_send_packet(st->dec, NULL);     /* 冲刷：把解码器里还没吐出来的帧取出来（L5） */
            count_frames(st->dec, frame, st);
        }
    }

    /* ---- 输出 ---- */
    out_init(&out, stdout, json);
    out_obj_begin(&out, NULL);
    out_obj_begin(&out, "format");
    out_str(&out, "filename", path);
    out_str(&out, "format_name", ic->iformat->name);
    out_str(&out, "format_long_name", ic->iformat->long_name ? ic->iformat->long_name : "");
    if (ic->duration != AV_NOPTS_VALUE)
        out_dbl(&out, "duration", ic->duration / (double)AV_TIME_BASE, 3);
    if (ic->start_time != AV_NOPTS_VALUE)
        out_dbl(&out, "start_time", ic->start_time / (double)AV_TIME_BASE, 3);
    if (ic->pb && avio_size(ic->pb) >= 0)
        out_int(&out, "size", avio_size(ic->pb));
    if (ic->bit_rate > 0)
        out_int(&out, "bit_rate", ic->bit_rate);
    out_int(&out, "nb_streams", ic->nb_streams);
    out_int(&out, "probe_score", ic->probe_score);
    out_dict(&out, "tags", ic->metadata);
    out_obj_end(&out);
    out_arr_begin(&out, "streams");
    for (unsigned i = 0; i < ic->nb_streams; i++)
        out_stream(&out, ic->streams[i], &stats[i], stats[i].dec != NULL);
    out_arr_end(&out);
    out_obj_end(&out);
    ret = (fflush(stdout) == 0) ? MT_OK : MT_FAIL;

end:
    if (stats) {
        for (unsigned i = 0; i < ic->nb_streams; i++) {
            avcodec_free_context(&stats[i].dec);
            av_freep(&stats[i].pts);
        }
    }
    av_freep(&stats);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avformat_close_input(&ic);
    return ret == MT_OK ? MT_OK : MT_FAIL;
}
