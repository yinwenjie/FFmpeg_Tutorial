/*
 * L08 帧类型、GOP 与 side data：取出指定时刻的那一帧画面，存成一张灰度图。
 *
 * 用法：grab_frame <输入文件> <秒> <输出.pgm> [key]
 *
 * 例子：
 *   grab_frame samples/small_360p25.mp4 2.5 out/at_2.5.pgm
 *   grab_frame samples/small_360p25.mp4 2.5 out/key_2.5.pgm key
 *
 * 一帧不一定能独立解码，所以“取 2.5 秒的画面”要分三步：
 * 定位到 2.5 秒之前最近的关键帧；从那里开始解码；把目标之前的帧解出来丢掉，直到目标帧。
 * 第四个参数写 key 时省掉第三步，直接输出定位到的那个关键帧：快，但时刻不准。
 *
 * 输出是 PGM 格式的灰度图，只有亮度（Y 平面）。彩色要做像素格式转换，L09 讲。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>

typedef struct Grabber {
    AVRational  time_base;      /* 视频流的时间基 */
    int64_t     target;         /* 目标时刻，单位是流的时间基 */
    int         key_only;       /* 1 表示直接输出定位到的第一帧 */
    const char *filename;
    int         nb_decoded;     /* 定位之后一共解出了多少帧 */
    int         overshoot;      /* 1 表示定位落在了目标之后，要往前重新定位 */
    int         done;           /* 1 表示已经取到并写出 */
} Grabber;

/*
 * 把亮度平面写成 PGM 文件。PGM 是最简单的图像格式之一：
 * 一行文本头 “P5 宽 高 255”，后面是 宽×高 个字节，每个字节一个像素的亮度。
 * 写的时候一行一行地写，跳过 linesize 里的填充（L05）。
 */
static int write_pgm(const AVFrame *frame, const char *filename)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);
    FILE *f;
    int ret = 0;

    /* 只处理第一个平面是 8 位亮度的格式：yuv420p、yuv422p、yuv444p、nv12、gray 等。 */
    if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL)) ||
        desc->comp[0].plane != 0 || desc->comp[0].depth != 8 || desc->comp[0].step != 1) {
        fprintf(stderr, "本程序不处理像素格式 %s，像素格式转换在 L09 讲\n",
                desc ? desc->name : "未知格式");
        return AVERROR(ENOSYS);
    }

    f = fopen(filename, "wb");
    if (!f) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开 %s：%s\n", filename, av_err2str(ret));
        return ret;
    }
    fprintf(f, "P5\n%d %d\n255\n", frame->width, frame->height);
    for (int y = 0; y < frame->height; y++)
        fwrite(frame->data[0] + y * frame->linesize[0], 1, frame->width, f);
    if (ferror(f))
        ret = AVERROR(EIO);
    if (fclose(f))
        ret = AVERROR(EIO);
    if (ret < 0)
        fprintf(stderr, "写 %s 出错\n", filename);
    return ret;
}

/* 处理定位之后解出来的一帧。目标之前的帧丢掉，目标帧写成图片。 */
static int process_frame(Grabber *g, const AVFrame *frame)
{
    const int64_t pts = frame->best_effort_timestamp;
    /*
     * 一帧从 pts 开始显示，持续 duration。目标时刻落在 [pts, pts + duration) 里的那一帧，
     * 就是那个时刻屏幕上显示的画面。有的文件不记录 duration（值为 0），这时退一步，取第一个 pts 不小于目标的帧。
     */
    const int reached = frame->duration > 0 ? pts + frame->duration > g->target
                                            : pts >= g->target;

    g->nb_decoded++;
    if (g->nb_decoded == 1)
        printf("定位之后解出的第一帧：pts %" PRId64 " = %.3f 秒，%c 帧%s\n", pts,
               pts * av_q2d(g->time_base), av_get_picture_type_char(frame->pict_type),
               frame->flags & AV_FRAME_FLAG_KEY ? "，关键帧" : "");

    /*
     * 定位之后的第一帧就已经过了目标：定位落在了目标后面。
     * 没有索引的容器（MPEG-TS）只能估计位置，会出现这种情况。告诉 main() 往前一些重新定位。
     */
    if (g->nb_decoded == 1 && !g->key_only && pts > g->target) {
        g->overshoot = 1;
        return 0;
    }

    if (!g->key_only && !reached)
        return 0;   /* 还没到目标，这一帧只是为了解后面的帧，丢掉 */

    printf("输出的帧：pts %" PRId64 " = %.3f 秒，%c 帧，是定位之后解出的第 %d 帧\n", pts,
           pts * av_q2d(g->time_base), av_get_picture_type_char(frame->pict_type),
           g->nb_decoded);
    g->done = 1;
    return write_pgm(frame, g->filename);
}

/* 送一个包（NULL 表示冲刷），取帧。取到目标帧之后不再继续。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Grabber *g)
{
    int ret = avcodec_send_packet(dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包出错：%s\n", av_err2str(ret));
        return ret;
    }
    while (!g->done && !g->overshoot) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
            return ret;
        }
        ret = process_frame(g, frame);
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }
    return 0;
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVStream *stream;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    Grabber g = { 0 };
    char *end_ptr;
    double seconds;
    int64_t seek_ts, first_ts;
    int ret, index, nb_packets = 0, from_start = 0;

    if (argc < 4 || argc > 5 || (argc == 5 && strcmp(argv[4], "key"))) {
        fprintf(stderr, "用法：%s <输入文件> <秒> <输出.pgm> [key]\n", argv[0]);
        return 1;
    }
    seconds = strtod(argv[2], &end_ptr);
    if (end_ptr == argv[2] || *end_ptr || seconds < 0) {
        fprintf(stderr, "时刻要写成不小于 0 的秒数，例如 2.5\n");
        return 1;
    }
    g.filename = argv[3];
    g.key_only = argc == 5;

    ret = avformat_open_input(&fmt, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不出流信息：%s\n", av_err2str(ret));
        goto end;
    }

    /* 建立视频解码器，步骤和 L07 相同。 */
    index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0) {
        ret = index;
        fprintf(stderr, "%s 里没有能解码的视频流\n", argv[1]);
        goto end;
    }
    stream = fmt->streams[index];
    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avcodec_parameters_to_context(dec, stream->codecpar);
    if (ret < 0)
        goto end;
    dec->pkt_timebase = stream->time_base;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器 %s：%s\n", codec->name, av_err2str(ret));
        goto end;
    }

    /*
     * 把“从文件开头算起的秒数”换算成这路流的时间戳。
     * 流的时间戳不一定从 0 开始（L04），所以要加上 start_time。
     */
    g.time_base = stream->time_base;
    g.target    = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, stream->time_base);
    first_ts = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
    g.target += first_ts;

    /*
     * 第 1 步：定位。AVSEEK_FLAG_BACKWARD 表示定位到目标之前最近的关键帧（L04）。
     * 这里解码器刚打开，还没有送过包，定位之后可以直接解码。
     */
    seek_ts = g.target;
    ret = av_seek_frame(fmt, index, seek_ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(stderr, "定位到 %.3f 秒失败：%s\n", seconds, av_err2str(ret));
        goto end;
    }
    printf("目标：%.3f 秒，时间戳 %" PRId64 "（时间基 %d/%d）\n", seconds, g.target,
           g.time_base.num, g.time_base.den);

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* 第 2、3 步：从关键帧开始解码，直到取到目标帧。定位不理想时往前重新定位。 */
    for (;;) {
        while (!g.done && !g.overshoot && (ret = av_read_frame(fmt, pkt)) >= 0) {
            if (pkt->stream_index == index) {
                nb_packets++;
                ret = decode(dec, pkt, frame, &g);
            }
            av_packet_unref(pkt);
            if (ret < 0)
                goto end;
        }
        if (!g.done && !g.overshoot) {
            if (ret != AVERROR_EOF) {
                fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
                goto end;
            }
            /* 文件读完了还没取到：目标帧可能还留在解码器里，冲刷一次。 */
            ret = decode(dec, NULL, frame, &g);
            if (ret < 0)
                goto end;
        }
        if (g.done)
            break;

        if (!g.overshoot && g.nb_decoded > 0) {
            /* 从目标之前开始解，一直解到文件末尾都没有到目标。 */
            fprintf(stderr, "%.3f 秒超出了视频的长度\n", seconds);
            ret = AVERROR(EINVAL);
            goto end;
        }

        /*
         * 走到这里有两种情况：定位落在了目标之后；或者落在了最后一个关键帧之后，一帧也解不出来。
         * 没有索引的容器（MPEG-TS）只能估计位置，这两种情况都会出现。
         * 办法是往前挪 2 秒再定位，已经挪到头了就干脆回到文件开头。
         */
        if (from_start) {
            fprintf(stderr, "从文件开头解码也取不到 %.3f 秒的帧\n", seconds);
            ret = AVERROR(EINVAL);
            goto end;
        }
        if (seek_ts <= first_ts) {
            from_start = 1;
            printf("定位不到目标之前的关键帧，改为从文件开头解码\n");
            /* AVSEEK_FLAG_BYTE：按字节位置定位，0 就是文件开头。 */
            ret = av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BYTE);
        } else {
            seek_ts = FFMAX(first_ts, seek_ts - av_rescale_q(2, (AVRational){ 1, 1 }, g.time_base));
            printf("定位不到目标之前的关键帧，改为定位到时间戳 %" PRId64 " 再试\n", seek_ts);
            ret = av_seek_frame(fmt, index, seek_ts, AVSEEK_FLAG_BACKWARD);
        }
        if (ret < 0) {
            fprintf(stderr, "定位失败：%s\n", av_err2str(ret));
            goto end;
        }
        /*
         * 解码器已经收过别处的包，里面留着那里的参考帧和还没输出的帧。
         * 换了位置之后必须清空，否则新位置的帧会参考到旧位置的画面。
         */
        avcodec_flush_buffers(dec);
        g.overshoot  = 0;
        g.nb_decoded = 0;
        nb_packets   = 0;
    }
    printf("送了 %d 个包，解出 %d 帧，其中 %d 帧只是为了解出目标帧\n", nb_packets, g.nb_decoded,
           g.nb_decoded - 1);
    ret = 0;

end:
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
