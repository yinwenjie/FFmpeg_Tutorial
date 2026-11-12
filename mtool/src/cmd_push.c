/*
 * mtool push：把一个文件按播放的速度推到流媒体地址（RTMP、UDP、TCP、SRT……）。
 *
 *   mtool push [-f 格式] [--loop N] [-d 时长] [--no-pace] [-o 键=值]... <输入> <地址>
 *
 * 来自 L19（协议层）和 L15（复用）。不解码不编码，只把压缩好的包原样发出去，所以：
 *   - 输入的编码必须是输出格式装得下的：RTMP 用 FLV，只装 H.264、AAC 这一类；装不下就在连接之前报错，
 *     不会连上服务器以后才发现（要改编码先用 mtool transcode）；
 *   - 按包的 dts 对着墙上的时钟发：1 秒的内容用 1 秒发出去，和 ffmpeg -re 一样。--no-pace 取消等待，用来测试或者推给不看时间的接收端；
 *   - Ctrl-C 或者 SIGTERM 让它写完文件尾再退出，退出码 0（直播推流本来就是靠 Ctrl-C 停的）；
 *   - --loop 播完以后重新打开输入再播，时间戳往后挪一遍的长度，每一路流的时间戳始终单调递增；
 *   - 对方断开（Broken pipe、Connection reset）不会让进程被信号杀掉：SIGPIPE 被忽略，变成一个普通的写错误，退出码 1。
 * 地址里的密钥（RTMP 的流名、userinfo、查询串）不会出现在输出里。
 */
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mathematics.h>
#include <libavutil/time.h>

#include "mtool.h"

typedef struct PushStream {
    int     out_index;      /* 对应的输出流序号，-1：这路流不推 */
    int     finished;       /* 已经读到 -d 的终点之后 */
    int64_t next_dts;       /* 下一个包应有的 dts（输入流时间基），给没有 dts 的包补值 */
    int64_t first_us, end_us;   /* 这一遍里第一个包的 dts、最后一个包的结束时间（微秒，已经加上循环的偏移） */
    int64_t packets, bytes;
} PushStream;

static volatile sig_atomic_t stop_requested;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

/* 给 libavformat 的阻塞操作（连接、握手、写）用：返回非 0 就中止（L19） */
static int interrupted(void *opaque)
{
    (void)opaque;
    return stop_requested;
}

static void usage(FILE *f)
{
    fprintf(f,
            "用法：mtool push [选项] <输入> <地址>\n\n"
            "把输入文件按播放的速度推到流媒体地址，不转码。地址例如：\n"
            "  rtmp://主机/应用/流名      RTMP（封装成 FLV；视频 H.264，音频 AAC）\n"
            "  udp://主机:端口            裸 UDP（封装成 MPEG-TS）\n"
            "  tcp://主机:端口            裸 TCP（封装成 MPEG-TS；接收端要先监听）\n"
            "  srt://主机:端口            SRT（要这份 FFmpeg 带 libsrt）\n\n"
            "选项：\n"
            "  -f, --format 名字   封装格式，默认按协议选（rtmp 用 flv，rtsp 用 rtsp，udp、tcp、srt 用 mpegts）\n"
            "      --loop N        输入播完以后再从头播 N 遍；-1 一直播（靠 Ctrl-C 停），默认 0\n"
            "  -d, --duration 时间 一共推多久（循环也算在内），默认播完输入\n"
            "      --no-pace       不按播放速度等待，能发多快发多快\n"
            "  -o, --opt 键=值     交给协议或者封装器的选项，可以重复给（例如 -o rtsp_transport=tcp）\n"
            "  -h, --help          显示这段说明\n\n"
            "输入里只推第一路视频和第一路音频。Ctrl-C 正常结束（退出码 0）；连不上、对方断开、编码装不下是失败（退出码 1）。\n");
}

/* 地址里能泄露的部分去掉再打印：userinfo、查询串，RTMP 的最后一段是推流密钥（L19） */
static void redact_url(const char *url, char *out, size_t size)
{
    const char *scheme_end = strstr(url, "://"), *host, *at;
    char tmp[1024], *p, *first, *last;

    if (!scheme_end) {
        snprintf(out, size, "%s", url);
        return;
    }
    host = scheme_end + 3;
    p = strchr(host, '/');
    at = memchr(host, '@', p ? (size_t)(p - host) : strlen(host));
    if (at)
        host = at + 1;                          /* 去掉 用户名:密码@ */
    snprintf(tmp, sizeof(tmp), "%.*s%s", (int)(scheme_end + 3 - url), url, host);
    if ((p = strchr(tmp, '?')))
        *p = '\0';                              /* 查询串常常带 token */
    p = strstr(tmp, "://") + 3;
    first = strchr(p, '/');
    last  = strrchr(p, '/');
    if (!strncmp(url, "rtmp", 4) && first && last > first)
        strcpy(last + 1, "***");                /* rtmp://主机/应用/流名：流名是密钥 */
    snprintf(out, size, "%s", tmp);
}

/* 取地址的协议名（“://” 前面的部分，小写）。不是“协议名://……”的写法返回 -1 */
static int url_scheme(const char *url, char *out, size_t size)
{
    const char *sep = strstr(url, "://");
    size_t n = sep ? (size_t)(sep - url) : 0;

    if (!sep || n == 0 || n >= size)
        return -1;
    for (size_t i = 0; i < n; i++) {
        const char c = url[i];

        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
            return -1;
        out[i] = c >= 'A' && c <= 'Z' ? c + 32 : c;
    }
    out[n] = '\0';
    return 0;
}

/* 这份 FFmpeg 能不能往这个协议写？（编译时没有带的协议，连接之前就要说清楚，而不是丢一句“Protocol not found”） */
static int protocol_writable(const char *name)
{
    void *opaque = NULL;
    const char *p;

    while ((p = avio_enum_protocols(&opaque, 1)))
        if (!strcmp(p, name))
            return 1;
    return 0;
}

/* 默认的封装格式：按协议选；NULL 表示让 libavformat 按地址的扩展名猜 */
static const char *default_format(const char *proto)
{
    if (!strncmp(proto, "rtmp", 4))
        return "flv";
    if (!strncmp(proto, "rtsp", 4))
        return "rtsp";
    if (!strcmp(proto, "udp") || !strcmp(proto, "tcp") || !strcmp(proto, "srt") || !strcmp(proto, "unix"))
        return "mpegts";
    return NULL;
}

/* MKV 等容器的开头几个包没有 dts，复用器要求每个包都有（同 cmd_cut.c，L04） */
static void fill_missing_dts(AVPacket *pkt, const AVStream *st, PushStream *ps)
{
    if (pkt->dts == AV_NOPTS_VALUE && pkt->duration > 0) {
        if (ps->next_dts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
            ps->next_dts = pkt->pts - st->codecpar->video_delay * pkt->duration;
        pkt->dts = ps->next_dts;
    }
    if (pkt->dts != AV_NOPTS_VALUE)
        ps->next_dts = pkt->dts + pkt->duration;
}

/* 睡到墙上时钟到 until_us（相对 t0）。切成小段，好让 Ctrl-C 马上生效 */
static void wait_until(int64_t t0, int64_t until_us)
{
    int64_t left;

    while (!stop_requested && (left = until_us - (av_gettime_relative() - t0)) > 0)
        av_usleep(FFMIN(left, 50000));
}

int cmd_push(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "format",   required_argument, NULL, 'f' },
        { "loop",     required_argument, NULL, 'L' },
        { "duration", required_argument, NULL, 'd' },
        { "no-pace",  no_argument,       NULL, 'N' },
        { "opt",      required_argument, NULL, 'o' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    AVFormatContext *ic = NULL, *oc = NULL;
    AVPacket *pkt = NULL;
    AVDictionary *opts = NULL;
    PushStream *ps = NULL;
    const char *in_path, *url, *fmt_name = NULL;
    char proto[32];
    char shown[1100], *eq, *kv;
    double dur = -1, sent_s = 0;
    long loop = 0, round = 0;
    unsigned nb_in = 0;
    int pace = 1, header_written = 0, opened = 0, opt, ret = 0, rc = MT_FAIL, stopped = 0, limit_hit = 0;
    int64_t dur_us = AV_NOPTS_VALUE, start_us = 0, loop_off_us = 0, t0 = 0, wall0, wall_us;
    int64_t bytes = 0, packets = 0;
    Progress prog;
    struct sigaction sa = { 0 };

    optind = 1;
    while ((opt = getopt_long(argc, argv, "f:d:o:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 'f': fmt_name = optarg; break;
        case 'L': {
            char *end;

            loop = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end || loop < -1)
                return mt_usage_err("push", "--loop %s 不是次数（0 或者正整数，-1 表示一直播）", optarg);
            break;
        }
        case 'd':
            if (mt_parse_time(optarg, &dur) < 0 || dur <= 0)
                return mt_usage_err("push", "时长 %s 不是正的时间", optarg);
            break;
        case 'N': pace = 0; break;
        case 'o':
            kv = av_strdup(optarg);
            eq = kv ? strchr(kv, '=') : NULL;
            if (!eq || eq == kv) {
                av_free(kv);
                av_dict_free(&opts);
                return mt_usage_err("push", "-o %s 要写成 键=值", optarg);
            }
            *eq = '\0';
            av_dict_set(&opts, kv, eq + 1, 0);
            av_free(kv);
            break;
        case 'h': usage(stdout); return MT_OK;
        default:  return mt_usage_err("push", "不认识的选项");
        }
    }
    if (argc - optind != 2) {
        av_dict_free(&opts);
        return mt_usage_err("push", "需要一个输入和一个地址");
    }
    in_path = argv[optind];
    url     = argv[optind + 1];
    if (dur > 0)
        dur_us = (int64_t)(dur * AV_TIME_BASE);

    /* ---- 地址：必须是这份 FFmpeg 能写的网络协议。普通文件路径不是推流，别在这里写文件 ---- */
    redact_url(url, shown, sizeof(shown));
    if (url_scheme(url, proto, sizeof(proto)) < 0 || !strcmp(proto, "file") || !strcmp(proto, "pipe") || !strcmp(proto, "fd")) {
        mt_err("%s 不是流媒体地址（要写成 rtmp://、udp://、tcp:// 这样的）；写文件请用 mtool cut 或者 mtool transcode", shown);
        goto end_noinput;
    }
    if (!protocol_writable(proto)) {
        if (!strcmp(proto, "srt"))
            mt_err("这份 FFmpeg 没有 srt 协议（编译时要加 --enable-libsrt）：%s", shown);
        else
            mt_err("这份 FFmpeg 不能往 %s 协议写（没编译进去，或者它只能读）：%s", proto, shown);
        goto end_noinput;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        mt_err("内存不够");
        goto end_noinput;
    }
    if (mt_open_input(&ic, in_path) < 0)
        goto end;
    nb_in = ic->nb_streams;
    if (ic->start_time != AV_NOPTS_VALUE)
        start_us = ic->start_time;              /* TS 的时间不从 0 开始：整体平移，推出去的时间戳从 0 开始 */

    /* ---- 输出：每种要推的流（第一路视频、第一路音频）新建一路输出流，检查输出格式装不装得下 ---- */
    if (!fmt_name)
        fmt_name = default_format(proto);
    ret = avformat_alloc_output_context2(&oc, NULL, fmt_name, url);
    if (ret < 0) {
        mt_averr(ret, "不知道 %s 该用什么封装格式（用 -f 指定）", shown);
        goto end;
    }
    oc->interrupt_callback.callback = interrupted;
    ps = av_calloc(ic->nb_streams, sizeof(*ps));
    if (!ps) {
        mt_err("内存不够");
        goto end;
    }
    for (unsigned i = 0; i < ic->nb_streams; i++)
        ps[i].out_index = -1;
    for (unsigned i = 0; i < ic->nb_streams; i++)
        ps[i].first_us = ps[i].end_us = AV_NOPTS_VALUE;
    for (int type = AVMEDIA_TYPE_VIDEO; type <= AVMEDIA_TYPE_AUDIO; type++) {
        const int i = av_find_best_stream(ic, type, -1, -1, NULL, 0);
        const AVStream *is;
        AVStream *os;

        if (i < 0)
            continue;
        is = ic->streams[i];
        if (avformat_query_codec(oc->oformat, is->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 0) {
            mt_err("%s 装不下%s编码 %s：流拷贝不能改编码，先用 mtool transcode 转成它装得下的（RTMP 用 --vcodec libx264 --acodec aac）",
                   oc->oformat->name, type == AVMEDIA_TYPE_VIDEO ? "视频" : "音频", avcodec_get_name(is->codecpar->codec_id));
            goto end;
        }
        os = avformat_new_stream(oc, NULL);
        if (!os) {
            mt_err("内存不够");
            goto end;
        }
        ps[i].out_index = os->index;
        ps[i].next_dts  = AV_NOPTS_VALUE;
        if ((ret = avcodec_parameters_copy(os->codecpar, is->codecpar)) < 0) {
            mt_averr(ret, "处理流参数失败");
            goto end;
        }
        os->codecpar->codec_tag = 0;
        os->disposition = is->disposition;
        os->time_base   = is->time_base;
        os->avg_frame_rate = is->avg_frame_rate;
    }
    if (!oc->nb_streams) {
        mt_err("%s 里没有可以推的流（视频、音频）", in_path);
        goto end;
    }

    /* ---- 信号：Ctrl-C 和 SIGTERM 让循环收尾；SIGPIPE 忽略，对方断开时写操作返回错误而不是杀掉进程 ---- */
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);                   /* 不设 SA_RESTART：被打断的阻塞调用返回 */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    /* ---- 连接。-o 给的选项先交给协议（avio_open2 取走它认识的），剩下的交给封装器 ---- */
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&oc->pb, url, AVIO_FLAG_WRITE, &oc->interrupt_callback, &opts);
        if (ret < 0) {
            if (stop_requested)
                goto interrupted_early;
            mt_averr(ret, "连不上 %s", shown);
            goto end;
        }
        opened = 1;
    }
    /* FLV 开头的元数据里有时长和文件大小，写完文件尾要回去改；往网络上推改不了，会打印两条警告。
     * 直播本来就没有时长：让封装器不写这两项（和 ffmpeg 命令行推 RTMP 时建议的 -flvflags no_duration_filesize 一样） */
    if (!strcmp(oc->oformat->name, "flv") && !av_dict_get(opts, "flvflags", NULL, 0))
        av_dict_set(&opts, "flvflags", "no_duration_filesize", 0);
    ret = avformat_write_header(oc, &opts);
    if (ret < 0) {
        if (stop_requested)
            goto interrupted_early;
        mt_averr(ret, "写文件头失败（%s，输出格式 %s）", shown, oc->oformat->name);
        goto end;
    }
    header_written = 1;
    if (opts) {
        const AVDictionaryEntry *e = av_dict_iterate(opts, NULL);

        mt_err("协议和封装器 %s 都不认识选项 %s", oc->oformat->name, e->key);
        goto end;
    }

    mt_progress_start(&prog, "推流", dur > 0 ? dur : (loop >= 0 && ic->duration != AV_NOPTS_VALUE ?
                                                      ic->duration / (double)AV_TIME_BASE * (loop + 1) : 0));
    wall0 = t0 = av_gettime_relative();         /* 时钟从连接成功以后开始：握手花的时间不算在节目里 */

    /* ---- 读包、对着时钟等、写包 ---- */
    while (!stop_requested && !limit_hit) {
        ret = av_read_frame(ic, pkt);
        if (ret == AVERROR_EOF && (loop < 0 || round < loop)) {
            /* 播完了：回到开头再来一遍，时间戳整体往后挪“这一遍里最长的一路流的长度”（结束时间减第一个包的 dts，
             * 不能直接用结束时间：MP4 的音频第一个包的 dts 是负的，那样下一遍的第一个音频包会和这一遍的最后一个撞车）。
             * 这样每一路流的时间戳在整个推流里都单调递增 */
            int64_t len = 0;

            for (unsigned i = 0; i < ic->nb_streams; i++) {
                if (ps[i].out_index >= 0 && ps[i].first_us != AV_NOPTS_VALUE)
                    len = FFMAX(len, ps[i].end_us - ps[i].first_us);
                ps[i].first_us = ps[i].end_us = AV_NOPTS_VALUE;
            }
            if (len <= 0) {
                mt_err("%s 里的包没有时间戳，没法循环", in_path);
                goto end;
            }
            round++;
            loop_off_us += len;
            /* 回到开头用“重新打开”，不用 seek：TS 这类没有索引的容器 seek 不一定落在第一个包上，
             * 落在一个 P 帧上的话接收端从第二遍起会花屏；重新打开读到的永远和第一遍一样 */
            avformat_close_input(&ic);
            if (mt_open_input(&ic, in_path) < 0)
                goto end;
            if (ic->nb_streams != nb_in) {
                mt_err("%s 第 %ld 次打开时流的数量变了（%u → %u），没法循环", in_path, round + 1, nb_in, ic->nb_streams);
                goto end;
            }
            for (unsigned i = 0; i < ic->nb_streams; i++)
                ps[i].next_dts = AV_NOPTS_VALUE;
            continue;
        }
        if (ret < 0)
            break;
        if ((unsigned)pkt->stream_index >= ic->nb_streams || ps[pkt->stream_index].out_index < 0) {
            av_packet_unref(pkt);
            continue;
        }
        {
            PushStream *s = &ps[pkt->stream_index];
            const AVStream *is = ic->streams[pkt->stream_index];
            const AVStream *os = oc->streams[s->out_index];
            int64_t ts_us, end_us;
            const int64_t off = av_rescale_q(loop_off_us - start_us, AV_TIME_BASE_Q, is->time_base);

            fill_missing_dts(pkt, is, s);
            if (pkt->pts != AV_NOPTS_VALUE)
                pkt->pts += off;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts += off;
            ts_us = av_rescale_q(pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts, is->time_base, AV_TIME_BASE_Q);
            if (pkt->dts == AV_NOPTS_VALUE && pkt->pts == AV_NOPTS_VALUE)
                ts_us = av_gettime_relative() - t0;     /* 没有时间戳的包（裸流）：当作现在 */

            if (dur_us != AV_NOPTS_VALUE && ts_us >= dur_us) {
                int all = 1;

                s->finished = 1;
                av_packet_unref(pkt);
                for (unsigned i = 0; i < ic->nb_streams; i++)
                    if (ps[i].out_index >= 0 && !ps[i].finished)
                        all = 0;
                limit_hit = all;
                continue;
            }
            if (pkt->dts != AV_NOPTS_VALUE) {
                end_us = av_rescale_q(pkt->dts + FFMAX(pkt->duration, 0), is->time_base, AV_TIME_BASE_Q);
                if (s->first_us == AV_NOPTS_VALUE)
                    s->first_us = ts_us;
                if (s->end_us == AV_NOPTS_VALUE || end_us > s->end_us)
                    s->end_us = end_us;
            }

            if (pace)
                wait_until(t0, ts_us);
            if (stop_requested) {
                av_packet_unref(pkt);
                break;
            }
            av_packet_rescale_ts(pkt, is->time_base, os->time_base);
            pkt->stream_index = s->out_index;
            pkt->pos = -1;
            s->packets++;
            s->bytes += pkt->size;
            packets++;
            bytes += pkt->size;
            sent_s = FFMAX(sent_s, ts_us / (double)AV_TIME_BASE);
            ret = av_interleaved_write_frame(oc, pkt);   /* 不管成败，包都已经交出去了 */
            if (ret < 0) {
                if (stop_requested)
                    break;
                mt_averr(ret, "往 %s 发数据失败（对方断开了？）", shown);
                goto end;
            }
            mt_progress_update(&prog, sent_s, 0);
        }
    }
    if (ret < 0 && ret != AVERROR_EOF && !stop_requested) {
        mt_averr(ret, "读包出错（%s 可能被截断或损坏）", in_path);
        goto end;
    }
    stopped = stop_requested;
    wall_us = av_gettime_relative() - wall0;
    mt_progress_end(&prog, sent_s, 0);
    ret = av_write_trailer(oc);
    header_written = 0;
    if (ret < 0 && !stopped) {
        mt_averr(ret, "写文件尾失败（%s）", shown);
        goto end;
    }

    if (stopped)
        printf("收到中断信号，已停止推流：%s → %s（%s）\n", in_path, shown, oc->oformat->name);
    else
        printf("推流完成：%s → %s（%s）\n", in_path, shown, oc->oformat->name);
    for (unsigned i = 0; i < ic->nb_streams; i++)
        if (ps[i].out_index >= 0)
            printf("  %s：%" PRId64 " 个包，%" PRId64 " 字节\n",
                   av_get_media_type_string(ic->streams[i]->codecpar->codec_type), ps[i].packets, ps[i].bytes);
    printf("  共 %" PRId64 " 个包、%" PRId64 " 字节，推了 %.3f 秒的内容，用时 %.3f 秒%s，循环 %ld 遍\n",
           packets, bytes, sent_s, wall_us / (double)AV_TIME_BASE, pace ? "" : "（没有按播放速度等待）", round);
    rc = MT_OK;
    goto end;

interrupted_early:
    printf("收到中断信号，连接还没建好就停了：%s\n", shown);
    rc = MT_OK;
end:
    if (header_written)
        av_write_trailer(oc);
    if (oc && opened && oc->pb)
        avio_closep(&oc->pb);
    avformat_free_context(oc);
    avformat_close_input(&ic);
    av_freep(&ps);
    av_packet_free(&pkt);
end_noinput:
    av_dict_free(&opts);
    return rc;
}
