/*
 * mtool cut：不转码截取一段，可以同时换容器。
 *
 *   mtool cut [-s 起点] [-e 终点 | -d 时长] [--video-only | --audio-only] [-f 格式] [-y] <输入> <输出>
 *
 * 来自 L04（时间基、时间戳、remux_cut）和 L15（复用：写文件头、时间基由复用器决定、写文件尾）。
 * 只搬运压缩好的包，不解码不编码，所以：
 *   - 起点只能落在关键帧上：实际起点是 -s 之前最近的一个关键帧（TS 没有索引，可能是之后的第一个）；
 *   - 终点按 dts 判断，留下的每个包都能解码；
 *   - 输出容器装不下输入的编码时（例如 FLV 装不下 MPEG-4 视频）直接报错，不会写出一个坏文件。
 * 要精确到帧，用 mtool transcode。
 */
#include <getopt.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

#include "mtool.h"

typedef struct CutStream {
    int     out_index;      /* 对应的输出流序号，-1：这路流不要 */
    int     finished;       /* 已经读到终点之后的包 */
    int64_t next_dts;       /* 下一个包应有的 dts，给没有 dts 的包补值（输入流时间基） */
    int64_t packets;
} CutStream;

static void usage(FILE *f)
{
    fprintf(f,
            "用法：mtool cut [选项] <输入> <输出>\n\n"
            "不解码、不编码，把输入的一段原样装进输出，输出的容器由扩展名决定。\n\n"
            "选项：\n"
            "  -s, --start 时间     起点（秒，也可以写 1:30 或 00:01:30.5），默认从头开始\n"
            "  -e, --end 时间       终点，从文件开头算起（和 -d 只能给一个）\n"
            "  -d, --duration 时间  时长，从 -s 算起（终点 = 起点 + 时长）\n"
            "      --video-only     只保留视频\n"
            "      --audio-only     只保留音频\n"
            "  -f, --format 名字    输出格式，扩展名认不出来时用（例如 mp4、matroska、mpegts）\n"
            "      --faststart      MP4 把 moov 放到文件开头（边下边播）\n"
            "  -y                   输出文件已经存在时覆盖它\n"
            "  -h, --help           显示这段说明\n\n"
            "起点只能落在关键帧上：实际起点是 -s 之前最近的关键帧，所以输出可能比要求的略长。\n");
}

/* MKV 等容器的开头几个包没有 dts，复用器要求每个包都有，这里补上（L04）。 */
static void fill_missing_dts(AVPacket *pkt, const AVStream *st, CutStream *cs)
{
    if (pkt->dts == AV_NOPTS_VALUE && pkt->duration > 0) {
        if (cs->next_dts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
            cs->next_dts = pkt->pts - st->codecpar->video_delay * pkt->duration;
        pkt->dts = cs->next_dts;
    }
    if (pkt->dts != AV_NOPTS_VALUE)
        cs->next_dts = pkt->dts + pkt->duration;
}

int cmd_cut(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "start",      required_argument, NULL, 's' },
        { "end",        required_argument, NULL, 'e' },
        { "duration",   required_argument, NULL, 'd' },
        { "video-only", no_argument,       NULL, 'V' },
        { "audio-only", no_argument,       NULL, 'A' },
        { "format",     required_argument, NULL, 'f' },
        { "faststart",  no_argument,       NULL, 'F' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    AVFormatContext *ic = NULL, *oc = NULL;
    AVPacket *pkt = NULL;
    AVDictionary *mux_opts = NULL;
    CutStream *cs = NULL;
    const char *in_path, *out_path, *fmt_name = NULL;
    double start = -1, end = -1, dur = -1;
    int video_only = 0, audio_only = 0, faststart = 0, overwrite = 0;
    int header_written = 0, created = 0, ref_stream = -1, opt, ret = 0, rc = MT_FAIL;
    int64_t file_start = 0, end_ts = AV_NOPTS_VALUE, origin = AV_NOPTS_VALUE, written = 0;

    optind = 1;
    while ((opt = getopt_long(argc, argv, "s:e:d:f:yh", longopts, NULL)) != -1) {
        switch (opt) {
        case 's':
            if (mt_parse_time(optarg, &start) < 0)
                return mt_usage_err("cut", "起点 %s 不是时间", optarg);
            break;
        case 'e':
            if (mt_parse_time(optarg, &end) < 0)
                return mt_usage_err("cut", "终点 %s 不是时间", optarg);
            break;
        case 'd':
            if (mt_parse_time(optarg, &dur) < 0 || dur <= 0)
                return mt_usage_err("cut", "时长 %s 不是正的时间", optarg);
            break;
        case 'V': video_only = 1; break;
        case 'A': audio_only = 1; break;
        case 'f': fmt_name = optarg; break;
        case 'F': faststart = 1; break;
        case 'y': overwrite = 1; break;
        case 'h': usage(stdout); return MT_OK;
        default:  return mt_usage_err("cut", "不认识的选项");
        }
    }
    if (argc - optind != 2)
        return mt_usage_err("cut", "需要一个输入和一个输出");
    in_path  = argv[optind];
    out_path = argv[optind + 1];
    if (video_only && audio_only)
        return mt_usage_err("cut", "--video-only 和 --audio-only 不能同时用");
    if (end >= 0 && dur >= 0)
        return mt_usage_err("cut", "-e 和 -d 只能给一个");
    if (dur >= 0)
        end = (start >= 0 ? start : 0) + dur;
    if (end >= 0 && end <= (start >= 0 ? start : 0))
        return mt_usage_err("cut", "终点必须在起点之后");
    if (!strcmp(in_path, out_path)) {
        mt_err("输入和输出是同一个文件，会把输入写坏：%s", in_path);
        return MT_FAIL;
    }
    if (!overwrite && access(out_path, F_OK) == 0) {
        mt_err("输出文件 %s 已经存在，加 -y 覆盖它", out_path);
        return MT_FAIL;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        mt_err("内存不够");
        return MT_FAIL;
    }
    if (mt_open_input(&ic, in_path) < 0)
        goto end;
    if (ic->start_time != AV_NOPTS_VALUE)
        file_start = ic->start_time;            /* TS 的时间不从 0 开始，用户给的时间从文件开头算（L03） */
    if (end >= 0)
        end_ts = file_start + (int64_t)(end * AV_TIME_BASE);

    /* ---- 输出：创建上下文，每路要保留的流新建一路输出流，检查容器装不装得下 ---- */
    ret = avformat_alloc_output_context2(&oc, NULL, fmt_name, out_path);
    if (ret < 0) {
        mt_averr(ret, "不知道 %s 该用什么格式（扩展名认不出来，用 -f 指定）", out_path);
        goto end;
    }
    av_dict_copy(&oc->metadata, ic->metadata, 0);

    cs = av_calloc(ic->nb_streams, sizeof(*cs));
    if (!cs) {
        mt_err("内存不够");
        goto end;
    }
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        const AVStream *is = ic->streams[i];
        const enum AVMediaType type = is->codecpar->codec_type;
        AVStream *os;

        cs[i].out_index = -1;
        cs[i].next_dts  = AV_NOPTS_VALUE;
        if (!((type == AVMEDIA_TYPE_VIDEO && !audio_only && !(is->disposition & AV_DISPOSITION_ATTACHED_PIC)) ||
              (type == AVMEDIA_TYPE_AUDIO && !video_only) ||
              (type == AVMEDIA_TYPE_SUBTITLE && !video_only && !audio_only)))
            continue;
        if (avformat_query_codec(oc->oformat, is->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0) {
            mt_err("输出格式 %s 装不下第 %u 路流的编码 %s。流拷贝不能改编码：换一种容器（-f），或者用 mtool transcode",
                   oc->oformat->name, i, avcodec_get_name(is->codecpar->codec_id));
            goto end;
        }
        os = avformat_new_stream(oc, NULL);
        if (!os) {
            mt_err("内存不够");
            goto end;
        }
        cs[i].out_index = os->index;
        if ((ret = avcodec_parameters_copy(os->codecpar, is->codecpar)) < 0)
            goto end_av;
        os->codecpar->codec_tag = 0;            /* 让复用器按自己的容器重新选 */
        av_dict_copy(&os->metadata, is->metadata, 0);
        os->disposition = is->disposition;
        os->time_base   = is->time_base;        /* 只是建议，写完文件头以后以复用器给的为准 */
    }
    if (!oc->nb_streams) {
        mt_err("%s 里没有可以保留的流（视频、音频、字幕）", in_path);
        goto end;
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&oc->pb, out_path, AVIO_FLAG_WRITE)) < 0) {
            mt_averr(ret, "打不开输出 %s", out_path);
            goto end;
        }
        created = 1;                            /* 这个文件现在是我们写的（原来有的内容已经被覆盖） */
    }
    if (faststart)
        av_dict_set(&mux_opts, "movflags", "+faststart", 0);
    if ((ret = avformat_write_header(oc, &mux_opts)) < 0) {
        mt_averr(ret, "写文件头失败（输出格式 %s）", oc->oformat->name);
        goto end;
    }
    header_written = 1;

    /* ---- 定位到起点：关键帧在起点之前最近的位置（AVSEEK_FLAG_BACKWARD） ---- */
    if (start > 0 && ic->duration != AV_NOPTS_VALUE && start >= ic->duration / (double)AV_TIME_BASE) {
        /* 起点在文件尾之后：不检查的话 av_seek_frame 会落在最后一个关键帧，悄悄输出最后一小段 */
        mt_err("起点 %.3f 秒超过了文件的长度 %.3f 秒", start, ic->duration / (double)AV_TIME_BASE);
        goto end;
    }
    if (start > 0) {
        const int64_t start_ts = file_start + (int64_t)(start * AV_TIME_BASE);

        if ((ret = av_seek_frame(ic, -1, start_ts, AVSEEK_FLAG_BACKWARD)) < 0) {
            mt_averr(ret, "定位到 %.3f 秒失败", start);
            goto end;
        }
        ref_stream = av_find_best_stream(ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (ref_stream < 0 || cs[ref_stream].out_index < 0)
            for (unsigned i = 0; i < ic->nb_streams; i++)
                if (cs[i].out_index >= 0) {
                    ref_stream = i;
                    break;
                }
    } else {
        origin = 0;                             /* 从头开始，时间戳不平移 */
    }

    /* ---- 读包、换算时间戳、写包 ---- */
    while ((ret = av_read_frame(ic, pkt)) >= 0) {
        const AVStream *is;
        const AVStream *os;
        CutStream *s;

        if ((unsigned)pkt->stream_index >= ic->nb_streams || cs[pkt->stream_index].out_index < 0) {
            av_packet_unref(pkt);
            continue;
        }
        s  = &cs[pkt->stream_index];
        is = ic->streams[pkt->stream_index];
        os = oc->streams[s->out_index];
        fill_missing_dts(pkt, is, s);

        /* 终点按 dts 判断：包按解码顺序排，从某个包开始整段去掉，留下的一定都能解码 */
        if (end_ts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE &&
            av_compare_ts(pkt->dts, is->time_base, end_ts, AV_TIME_BASE_Q) >= 0)
            s->finished = 1;
        if (s->finished) {
            int all = 1;

            av_packet_unref(pkt);
            for (unsigned i = 0; i < ic->nb_streams; i++)
                if (cs[i].out_index >= 0 && !cs[i].finished)
                    all = 0;
            if (all) {
                ret = AVERROR_EOF;
                break;
            }
            continue;
        }

        /* 起点：等参考流的第一个关键帧，用它的 pts 作为输出的时间零点 */
        if (origin == AV_NOPTS_VALUE) {
            if (pkt->stream_index != ref_stream || !(pkt->flags & AV_PKT_FLAG_KEY) || pkt->pts == AV_NOPTS_VALUE) {
                av_packet_unref(pkt);
                continue;
            }
            origin = av_rescale_q(pkt->pts, is->time_base, AV_TIME_BASE_Q);
        }
        if (start > 0 && pkt->pts != AV_NOPTS_VALUE &&
            av_compare_ts(pkt->pts, is->time_base, origin, AV_TIME_BASE_Q) < 0) {
            av_packet_unref(pkt);               /* 显示时间早于零点的包不要 */
            continue;
        }
        if (origin != 0) {
            const int64_t off = av_rescale_q(origin, AV_TIME_BASE_Q, is->time_base);

            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts -= off;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts -= off;
        }
        av_packet_rescale_ts(pkt, is->time_base, os->time_base);
        pkt->stream_index = s->out_index;
        pkt->pos = -1;
        if ((ret = av_interleaved_write_frame(oc, pkt)) < 0) {
            mt_averr(ret, "写包失败");
            goto end;
        }
        s->packets++;
        written++;
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        mt_averr(ret, "读包出错（%s 可能被截断或损坏）", in_path);
        goto end;
    }
    if (origin == AV_NOPTS_VALUE || written == 0) {
        mt_err("起点 %.3f 秒之后没有关键帧，没有东西可以截取（文件总长 %.3f 秒）", start,
               ic->duration != AV_NOPTS_VALUE ? ic->duration / (double)AV_TIME_BASE : 0.0);
        goto end;
    }
    if ((ret = av_write_trailer(oc)) < 0) {
        header_written = 0;
        mt_averr(ret, "写文件尾失败");
        goto end;
    }
    header_written = 0;

    printf("截取完成：%s → %s（%s）\n", in_path, out_path, oc->oformat->name);
    if (start > 0)
        printf("  请求的起点 %.3f 秒，实际从 %.3f 秒的关键帧开始\n", start, (origin - file_start) / (double)AV_TIME_BASE);
    for (unsigned i = 0; i < ic->nb_streams; i++)
        if (cs[i].out_index >= 0)
            printf("  %s 流 #%u → #%d：%" PRId64 " 个包\n",
                   av_get_media_type_string(ic->streams[i]->codecpar->codec_type), i, cs[i].out_index, cs[i].packets);
    rc = MT_OK;
    goto end;

end_av:
    mt_averr(ret, "处理流参数失败");
end:
    if (header_written)
        av_write_trailer(oc);
    if (oc && !(oc->oformat->flags & AVFMT_NOFILE) && oc->pb)
        avio_closep(&oc->pb);
    if (rc != MT_OK && created)
        unlink(out_path);                       /* 失败时不留下一个看起来像成功的坏文件 */
    avformat_free_context(oc);
    avformat_close_input(&ic);
    av_dict_free(&mux_opts);
    av_freep(&cs);
    av_packet_free(&pkt);
    return rc;
}
