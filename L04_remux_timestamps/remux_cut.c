/*
 * L04 时间基与时间戳：能裁剪的 remux。
 *
 * 用法：remux_cut <输入> <输出> [-ss 起点秒] [-to 终点秒] [-video-only]
 *   什么选项都不给：整个文件原样换容器，和官方示例 remux.c 做的事相同。
 *   -ss, -to      只保留这一段。时间从文件开头算起，含义与 ffmpeg 的同名选项相同。
 *   -video-only   只保留视频流。
 *
 * 不解码、不编码，只搬运压缩数据包，所以：
 *   起点只能落在关键帧上。MP4、MKV 这类有索引的容器，实际起点是 -ss 之前最近的一个关键帧；
 *   TS 没有索引，实际起点可能是 -ss 之后的第一个关键帧。
 *   终点按 dts 判断，保证留下的每个包都能解码。
 *
 * 和官方示例相比，这个程序还做了几件工程上需要的事：
 *   拷贝元数据和 disposition；把输入流的时间基作为建议值交给复用器；
 *   给没有 dts 的包补上 dts；检查每一个返回值并报告真实的错误码。
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>

/* 每一路输入流的状态。 */
typedef struct StreamState {
    int     out_index;  /* 对应的输出流序号，-1 表示这路流不要 */
    int     finished;   /* 已经读到终点之后的包 */
    int64_t next_dts;   /* 下一个包应有的 dts，用来给没有 dts 的包补值（输入流时间基） */
    int64_t packets;    /* 写出的包数 */
    AVRational tb_hint; /* 交给复用器的建议时间基，只用于打印对比 */
} StreamState;

static void usage(const char *prog)
{
    fprintf(stderr,
            "用法：%s <输入> <输出> [-ss 起点秒] [-to 终点秒] [-video-only]\n", prog);
}

/*
 * MKV 这类容器每个包只存一个时间戳。视频有 B 帧时，解封装器对开头的几个包给不出 dts。
 * 复用器要求每个包都有 dts，所以这里补上：
 * 第一个包取 pts 减去重排延迟（video_delay 个包的时长），之后按“上一个 dts + 时长”往下推。
 */
static void fill_missing_dts(AVPacket *pkt, const AVStream *st, StreamState *state)
{
    if (pkt->dts == AV_NOPTS_VALUE && pkt->duration > 0) {
        if (state->next_dts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
            state->next_dts = pkt->pts - st->codecpar->video_delay * pkt->duration;
        pkt->dts = state->next_dts;
    }
    if (pkt->dts != AV_NOPTS_VALUE)
        state->next_dts = pkt->dts + pkt->duration;
}

int main(int argc, char **argv)
{
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket *pkt = NULL;
    StreamState *states = NULL;
    unsigned nb_states = 0;
    const char *in_filename, *out_filename;
    double start_sec = -1, end_sec = -1;
    int video_only = 0;
    int header_written = 0;
    int ref_stream = -1;                   /* 用哪一路流的关键帧确定起点 */
    int64_t file_start = 0;                /* 输入文件的起始时间，微秒 */
    int64_t end_ts = AV_NOPTS_VALUE;       /* 终点，微秒，已加上 file_start */
    int64_t origin = AV_NOPTS_VALUE;       /* 输出的时间零点，微秒 */
    int ret;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }
    in_filename  = argv[1];
    out_filename = argv[2];
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-ss") && i + 1 < argc) {
            start_sec = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-to") && i + 1 < argc) {
            end_sec = atof(argv[++i]);
        } else if (!strcmp(argv[i], "-video-only")) {
            video_only = 1;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (end_sec >= 0 && end_sec <= start_sec) {
        fprintf(stderr, "终点必须在起点之后\n");
        return 1;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "分配 AVPacket 失败\n");
        return 1;
    }

    /* ---------- 输入 ---------- */
    ret = avformat_open_input(&ifmt_ctx, in_filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开输入 %s：%s\n", in_filename, av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(ifmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不到流信息：%s\n", av_err2str(ret));
        goto end;
    }

    /* L03 见过：TS 文件的时间不从 0 开始。用户给的时间从文件开头算起，要加上这个起始值。 */
    if (ifmt_ctx->start_time != AV_NOPTS_VALUE)
        file_start = ifmt_ctx->start_time;
    if (end_sec >= 0)
        end_ts = file_start + (int64_t)(end_sec * AV_TIME_BASE);

    /* ---------- 输出第 1 步：创建输出上下文，容器格式由扩展名决定 ---------- */
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (ret < 0) {
        fprintf(stderr, "无法为 %s 创建输出：%s\n", out_filename, av_err2str(ret));
        goto end;
    }
    ret = av_dict_copy(&ofmt_ctx->metadata, ifmt_ctx->metadata, 0);
    if (ret < 0)
        goto end;

    /* ---------- 输出第 2 步：为要保留的每一路流新建输出流 ---------- */
    nb_states = ifmt_ctx->nb_streams;
    states = av_calloc(nb_states, sizeof(*states));
    if (!states) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    for (unsigned i = 0; i < nb_states; i++) {
        const AVStream *in_stream = ifmt_ctx->streams[i];
        const enum AVMediaType type = in_stream->codecpar->codec_type;
        AVStream *out_stream;

        states[i].out_index = -1;
        states[i].next_dts  = AV_NOPTS_VALUE;

        if (type != AVMEDIA_TYPE_VIDEO &&
            (video_only || (type != AVMEDIA_TYPE_AUDIO && type != AVMEDIA_TYPE_SUBTITLE)))
            continue;

        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        states[i].out_index = out_stream->index;

        /* 编码参数整个拷过去；codec_tag 清零，让复用器按自己的容器重新选。 */
        ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
        if (ret < 0)
            goto end;
        out_stream->codecpar->codec_tag = 0;

        /* 语言等流元数据、默认轨等标志也要带过去，官方示例漏了这一步。 */
        ret = av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
        if (ret < 0)
            goto end;
        out_stream->disposition = in_stream->disposition;

        /*
         * 在写文件头之前设置的 time_base 只是建议。复用器可以采纳，也可以改成自己的值。
         * 用输入流的时间基做建议值，采纳的话时间戳不需要舍入。
         */
        out_stream->time_base = in_stream->time_base;
        states[i].tb_hint     = in_stream->time_base;
    }
    if (ofmt_ctx->nb_streams == 0) {
        fprintf(stderr, "没有可以保留的流\n");
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* ---------- 输出第 3 步：打开输出文件 ---------- */
    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "打不开输出 %s：%s\n", out_filename, av_err2str(ret));
            goto end;
        }
    }

    /* ---------- 输出第 4 步：写文件头。复用器在这里确定每路输出流的 time_base ---------- */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "写文件头失败：%s\n", av_err2str(ret));
        goto end;
    }
    header_written = 1;

    printf("输出格式：%s\n", ofmt_ctx->oformat->name);
    for (unsigned i = 0; i < nb_states; i++) {
        const AVStream *out_stream;

        if (states[i].out_index < 0)
            continue;
        out_stream = ofmt_ctx->streams[states[i].out_index];
        printf("  输入流 #%u -> 输出流 #%d %-5s  time_base 建议 %d/%d，实际 %d/%d\n",
               i, states[i].out_index,
               av_get_media_type_string(out_stream->codecpar->codec_type),
               states[i].tb_hint.num, states[i].tb_hint.den,
               out_stream->time_base.num, out_stream->time_base.den);
    }

    /* ---------- 定位到起点 ---------- */
    if (start_sec >= 0) {
        const int64_t start_ts = file_start + (int64_t)(start_sec * AV_TIME_BASE);

        /*
         * 流序号传 -1 时，时间以 AV_TIME_BASE（微秒）为单位，由 FFmpeg 选一路流（一般是视频）来定位。
         * AVSEEK_FLAG_BACKWARD 表示定位到目标时间之前最近的关键帧。
         * 不从关键帧开始的话，开头的帧找不到它依赖的参考帧，解不出来。
         * 定位依赖容器的索引。TS 没有索引，只能按时间戳找到一个大概的位置，
         * 之后读到的第一个关键帧可能在目标时间之后。
         */
        ret = av_seek_frame(ifmt_ctx, -1, start_ts, AVSEEK_FLAG_BACKWARD);
        if (ret < 0) {
            fprintf(stderr, "定位到 %.3f 秒失败：%s\n", start_sec, av_err2str(ret));
            goto end;
        }

        /* 起点由哪一路流说了算：有视频就用视频，没有就用第一路保留的流。 */
        ref_stream = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
        if (ref_stream < 0 || states[ref_stream].out_index < 0) {
            for (unsigned i = 0; i < nb_states; i++) {
                if (states[i].out_index >= 0) {
                    ref_stream = i;
                    break;
                }
            }
        }
    } else {
        /* 没有 -ss：不平移时间戳。 */
        origin = 0;
    }

    /* ---------- 输出第 5 步：逐个读包、换算时间戳、写包 ---------- */
    while (1) {
        const AVStream *in_stream;
        const AVStream *out_stream;
        StreamState *state;

        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0)
            break;

        if ((unsigned)pkt->stream_index >= nb_states ||
            states[pkt->stream_index].out_index < 0) {
            av_packet_unref(pkt);
            continue;
        }
        state      = &states[pkt->stream_index];
        in_stream  = ifmt_ctx->streams[pkt->stream_index];
        out_stream = ofmt_ctx->streams[state->out_index];

        fill_missing_dts(pkt, in_stream, state);

        /*
         * 终点按 dts 判断。包是按解码顺序排列的，从某个包开始整段去掉，
         * 留下的包一定都能解码。按 pts 判断的话，可能丢掉一个 P 帧，
         * 却留下了排在它后面、依赖它的 B 帧。
         * av_compare_ts() 直接比较两个时间基不同的时间戳。
         */
        if (end_ts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE &&
            av_compare_ts(pkt->dts, in_stream->time_base, end_ts, AV_TIME_BASE_Q) >= 0)
            state->finished = 1;
        if (state->finished) {
            int all_finished = 1;

            av_packet_unref(pkt);
            for (unsigned i = 0; i < nb_states; i++)
                if (states[i].out_index >= 0 && !states[i].finished)
                    all_finished = 0;
            if (all_finished)
                break;
            continue;
        }

        /* 起点：等到参考流的第一个关键帧，用它的 pts 作为输出的时间零点。 */
        if (origin == AV_NOPTS_VALUE) {
            if (pkt->stream_index != ref_stream || !(pkt->flags & AV_PKT_FLAG_KEY) ||
                pkt->pts == AV_NOPTS_VALUE) {
                av_packet_unref(pkt);
                continue;
            }
            origin = av_rescale_q(pkt->pts, in_stream->time_base, AV_TIME_BASE_Q);
        }
        /* 显示时间早于零点的包不要，例如排在关键帧之前的那一小段音频。 */
        if (start_sec >= 0 && pkt->pts != AV_NOPTS_VALUE &&
            av_compare_ts(pkt->pts, in_stream->time_base, origin, AV_TIME_BASE_Q) < 0) {
            av_packet_unref(pkt);
            continue;
        }

        /* 平移：减去零点。零点是微秒，先换算到这路输入流的时间基。 */
        if (origin != 0) {
            const int64_t offset = av_rescale_q(origin, AV_TIME_BASE_Q, in_stream->time_base);

            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts -= offset;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts -= offset;
        }

        /* 换算：从输入流的时间基到输出流的时间基。必须用写完文件头之后的 out_stream->time_base。 */
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        pkt->stream_index = state->out_index;
        pkt->pos          = -1;

        /* 交错写入。函数会接管包里的数据并清空 pkt，所以这里不需要 av_packet_unref()。 */
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "写包失败：%s\n", av_err2str(ret));
            goto end;
        }
        state->packets++;
    }
    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "读包出错：%s\n", av_err2str(ret));
        goto end;
    }

    /* 写文件尾：把还暂存着的包写完，再写索引（MP4 的 moov 就是这时写的）。 */
    ret = av_write_trailer(ofmt_ctx);
    header_written = 0;
    if (ret < 0) {
        fprintf(stderr, "写文件尾失败：%s\n", av_err2str(ret));
        goto end;
    }

    if (start_sec >= 0) {
        if (origin == AV_NOPTS_VALUE)
            printf("请求的起点 %.3f 秒之后没有关键帧，输出是空的\n", start_sec);
        else
            printf("请求的起点 %.3f 秒，实际从 %.3f 秒处的关键帧开始\n",
                   start_sec, (origin - file_start) / (double)AV_TIME_BASE);
    }
    for (unsigned i = 0; i < nb_states; i++) {
        if (states[i].out_index >= 0)
            printf("  输出流 #%d 写出 %" PRId64 " 个包\n", states[i].out_index, states[i].packets);
    }

end:
    /* 文件头已经写了但中途出错，也要写文件尾，让复用器释放内部资源。 */
    if (header_written)
        av_write_trailer(ofmt_ctx);
    av_packet_free(&pkt);
    avformat_close_input(&ifmt_ctx);
    /* 输出一侧：先关文件，再释放上下文。 */
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);
    av_freep(&states);

    if (ret < 0 && ret != AVERROR_EOF)
        return 1;
    return 0;
}
