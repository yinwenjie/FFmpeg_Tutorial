/*
 * L04 时间基与时间戳：按读取顺序打印每个包的时间信息。
 *
 * 用法：packet_info <输入文件或 URL> [最多打印多少个包，默认 20，0 表示全部]
 *
 * 每一列的名字都是 AVPacket 的成员名，可以和下面这条命令的结果逐项核对：
 *   ffprobe -v error -show_entries packet=stream_index,pts,dts,duration,size,pos,flags \
 *           -of csv=p=0 <输入>
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

/* 每一路流的统计。时间都以这路流自己的 time_base 为单位。 */
typedef struct StreamStats {
    int64_t packets;      /* 包的个数 */
    int64_t keyframes;    /* 带关键帧标志的包 */
    int64_t reordered;    /* pts 和 dts 不相等的包 */
    int64_t bytes;        /* 数据总量 */
    int64_t min_pts, max_pts;
    int64_t first_dts, last_dts;
    int     dts_in_order; /* dts 是否一直递增 */
} StreamStats;

/* 打印一个时间戳：先是整数，再是换算成秒的值。AV_NOPTS_VALUE 表示“没有”。 */
static void print_ts(int64_t ts, AVRational tb)
{
    if (ts == AV_NOPTS_VALUE)
        printf(" %8s %9s", "N/A", "N/A");
    else
        printf(" %8" PRId64 " %9.4f", ts, ts * av_q2d(tb));
}

static void print_seconds(const char *name, int64_t ts, AVRational tb)
{
    if (ts == AV_NOPTS_VALUE)
        printf("  %s N/A", name);
    else
        printf("  %s %.4f 秒", name, ts * av_q2d(tb));
}

static void update_stats(StreamStats *s, const AVPacket *pkt)
{
    s->packets++;
    s->bytes += pkt->size;
    if (pkt->flags & AV_PKT_FLAG_KEY)
        s->keyframes++;
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts != pkt->dts)
        s->reordered++;

    if (pkt->pts != AV_NOPTS_VALUE) {
        if (s->min_pts == AV_NOPTS_VALUE || pkt->pts < s->min_pts)
            s->min_pts = pkt->pts;
        if (s->max_pts == AV_NOPTS_VALUE || pkt->pts > s->max_pts)
            s->max_pts = pkt->pts;
    }
    if (pkt->dts != AV_NOPTS_VALUE) {
        if (s->first_dts == AV_NOPTS_VALUE)
            s->first_dts = pkt->dts;
        else if (pkt->dts <= s->last_dts)
            s->dts_in_order = 0;
        s->last_dts = pkt->dts;
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt_ctx = NULL;
    AVPacket *pkt = NULL;
    StreamStats *stats = NULL;
    unsigned nb_stats = 0;
    int64_t limit = 20, n = 0;
    int ret;

    if (argc < 2 || argc > 3) {
        fprintf(stderr, "用法：%s <输入文件或 URL> [最多打印多少个包，默认 20，0 表示全部]\n", argv[0]);
        return 1;
    }
    if (argc == 3)
        limit = atoll(argv[2]);

    ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        return 1;
    }
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不到流信息：%s\n", av_err2str(ret));
        goto end;
    }

    nb_stats = fmt_ctx->nb_streams;
    stats = av_calloc(nb_stats, sizeof(*stats));
    /* AVPacket 不能直接定义成局部变量，必须用 av_packet_alloc() 分配。 */
    pkt = av_packet_alloc();
    if (!stats || !pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* 包上的时间戳是整数，单位是它所属的流的 time_base，所以先把每路流的时间基列出来。 */
    for (unsigned i = 0; i < nb_stats; i++) {
        const AVStream *st = fmt_ctx->streams[i];

        printf("流 #%u %-5s time_base = %d/%d\n", i,
               av_get_media_type_string(st->codecpar->codec_type),
               st->time_base.num, st->time_base.den);
        stats[i].min_pts = stats[i].max_pts = AV_NOPTS_VALUE;
        stats[i].first_dts = stats[i].last_dts = AV_NOPTS_VALUE;
        stats[i].dts_in_order = 1;
    }

    printf("\n%4s %2s %-5s %8s %9s %8s %9s %8s %7s %8s %s\n",
           "n", "st", "type", "pts", "pts_time", "dts", "dts_time",
           "duration", "size", "pos", "flags");

    /*
     * av_read_frame() 每次读出一个包。函数名里有 frame，读出来的是压缩数据包：
     * 视频是一帧的压缩数据，音频是一个或几个音频帧的压缩数据。
     * 返回 0 表示成功，读到文件末尾时返回 AVERROR_EOF。
     */
    while ((ret = av_read_frame(fmt_ctx, pkt)) >= 0) {
        const AVStream *st = fmt_ctx->streams[pkt->stream_index];
        const AVRational tb = st->time_base;

        if (limit == 0 || n < limit) {
            printf("%4" PRId64 " %2d %-5s", n, pkt->stream_index,
                   av_get_media_type_string(st->codecpar->codec_type));
            print_ts(pkt->pts, tb);
            print_ts(pkt->dts, tb);
            printf(" %8" PRId64 " %7d", pkt->duration, pkt->size);
            /* pos 是这个包在文件里的字节位置，-1 表示不知道。 */
            if (pkt->pos < 0)
                printf(" %8s", "N/A");
            else
                printf(" %8" PRId64, pkt->pos);
            printf(" %s%s\n",
                   pkt->flags & AV_PKT_FLAG_KEY     ? "K" : "_",
                   pkt->flags & AV_PKT_FLAG_DISCARD ? "D" : "_");
        }

        /* 有的容器在读数据的过程中才发现新的流，统计数组是按一开始的流数分配的。 */
        if ((unsigned)pkt->stream_index < nb_stats)
            update_stats(&stats[pkt->stream_index], pkt);
        n++;

        /*
         * 包里的数据是引用计数的。用完之后必须 av_packet_unref()，
         * 否则每读一个包就泄漏一块内存。unref 之后这个 AVPacket 可以接着用。
         */
        av_packet_unref(pkt);
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读包出错：%s\n", av_err2str(ret));
        goto end;
    }
    ret = 0;

    if (limit != 0 && n > limit)
        printf("  ……（共 %" PRId64 " 个包，只打印了前 %" PRId64 " 个）\n", n, limit);

    printf("\n== 统计 ==\n");
    for (unsigned i = 0; i < nb_stats; i++) {
        const AVStream *st = fmt_ctx->streams[i];
        const StreamStats *s = &stats[i];

        printf("流 #%u %-5s  包 %" PRId64 "  关键帧 %" PRId64
               "  pts≠dts %" PRId64 "  数据 %" PRId64 " 字节\n",
               i, av_get_media_type_string(st->codecpar->codec_type),
               s->packets, s->keyframes, s->reordered, s->bytes);
        printf("         ");
        print_seconds("pts 最小", s->min_pts, st->time_base);
        print_seconds("pts 最大", s->max_pts, st->time_base);
        print_seconds("首个 dts", s->first_dts, st->time_base);
        print_seconds("末个 dts", s->last_dts, st->time_base);
        printf("  dts %s\n", s->dts_in_order ? "一直递增" : "出现过回退");
    }

end:
    av_packet_free(&pkt);
    av_freep(&stats);
    avformat_close_input(&fmt_ctx);
    return ret < 0 ? 1 : 0;
}
