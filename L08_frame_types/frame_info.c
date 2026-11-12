/*
 * L08 帧类型、GOP 与 side data：逐帧打印视频帧上除了图像之外的信息。
 *
 * 用法：frame_info <输入文件> [mvs]
 *
 * 例子：
 *   frame_info samples/small_360p25.mp4 | head -30
 *   frame_info samples/small_360p25.mp4 mvs
 *
 * 每解出一帧打印一行：序号、帧类型、是否关键帧、时间戳、对应的包有多少字节、帧上带的 side data。
 * 最后打印统计：每种帧类型的数量和大小，每个 GOP 的长度，流一级的 side data。
 * 第二个参数写 mvs 时，要求解码器把运动矢量也导出来（作为一种 side data）。
 * 这个程序只解码、不写图像。
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/display.h>
#include <libavutil/frame.h>
#include <libavutil/motion_vector.h>

#define MAX_GOPS 64

typedef struct Stats {
    int64_t nb_frames;
    /* 按帧类型统计。下标是 enum AVPictureType 的值：1 是 I，2 是 P，3 是 B。 */
    int64_t count[8];
    int64_t bytes[8];
    /* GOP：从一个关键帧到下一个关键帧之前。记下前 MAX_GOPS 个 GOP 的帧数。 */
    int     gop_len[MAX_GOPS];
    int     nb_gops;
} Stats;

/* 打印一帧上的 side data：每一项的类型名和大小。运动矢量另外算出个数。 */
static void print_side_data(const AVFrame *frame)
{
    if (!frame->nb_side_data) {
        printf("  -");
        return;
    }
    for (int i = 0; i < frame->nb_side_data; i++) {
        const AVFrameSideData *sd = frame->side_data[i];

        printf("  [%s，%zu 字节", av_frame_side_data_name(sd->type), sd->size);
        if (sd->type == AV_FRAME_DATA_MOTION_VECTORS)
            printf("，%zu 个矢量", sd->size / sizeof(AVMotionVector));
        printf("]");
    }
}

/* 处理一帧：打印一行，更新统计。 */
static void process_frame(const AVFrame *frame, AVRational time_base, Stats *st)
{
    const int key  = !!(frame->flags & AV_FRAME_FLAG_KEY);
    const int type = frame->pict_type;
    /*
     * 这一帧对应的包有多少字节。AVFrame 里没有这个数，它是 main() 送包时
     * 放在 pkt->opaque 里、由解码器原样带到 frame->opaque 上的。
     */
    const int size = (int)(intptr_t)frame->opaque;

    printf("%4" PRId64 "  %c  %s  pts %7" PRId64 " = %6.3f 秒  %6d 字节", st->nb_frames,
           av_get_picture_type_char(type), key ? "关键帧" : "      ",
           frame->pts, frame->pts * av_q2d(time_base), size);
    print_side_data(frame);
    printf("\n");

    if (type >= 0 && type < 8) {
        st->count[type]++;
        st->bytes[type] += size;
    }
    /* 遇到关键帧就开始一个新的 GOP。 */
    if (key || !st->nb_gops) {
        if (st->nb_gops < MAX_GOPS)
            st->nb_gops++;
    }
    if (st->nb_gops <= MAX_GOPS)
        st->gop_len[st->nb_gops - 1]++;
    st->nb_frames++;
}

/* 送一个包（NULL 表示冲刷），取出所有能取的帧。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame,
                  AVRational time_base, Stats *st)
{
    int ret = avcodec_send_packet(dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包出错：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
            return ret;
        }
        process_frame(frame, time_base, st);
        av_frame_unref(frame);
    }
}

/* 打印流一级的 side data。它记在容器里，对整路流有效，不属于某一帧。 */
static void print_stream_side_data(const AVStream *stream)
{
    const AVCodecParameters *par = stream->codecpar;

    printf("流一级的 side data：%s\n", par->nb_coded_side_data ? "" : "无");
    for (int i = 0; i < par->nb_coded_side_data; i++) {
        const AVPacketSideData *sd = &par->coded_side_data[i];

        printf("  %s，%zu 字节", av_packet_side_data_name(sd->type), sd->size);
        /* 显示矩阵：播放时要把画面旋转多少度。手机竖着拍的视频靠它转正。 */
        if (sd->type == AV_PKT_DATA_DISPLAYMATRIX && sd->size >= 9 * sizeof(int32_t))
            printf("，旋转 %.0f 度", av_display_rotation_get((const int32_t *)sd->data));
        printf("\n");
    }
}

static void print_stats(const Stats *st)
{
    static const enum AVPictureType types[] = { AV_PICTURE_TYPE_I, AV_PICTURE_TYPE_P,
                                                AV_PICTURE_TYPE_B };
    int64_t total = 0;

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++)
        total += st->bytes[types[i]];

    printf("\n共 %" PRId64 " 帧\n", st->nb_frames);
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        const int t = types[i];

        if (!st->count[t])
            continue;
        printf("  %c 帧 %3" PRId64 " 个，共 %7" PRId64 " 字节，平均 %6" PRId64 " 字节，占总数据量的 %4.1f%%\n",
               av_get_picture_type_char(t), st->count[t], st->bytes[t],
               st->bytes[t] / st->count[t], total ? 100.0 * st->bytes[t] / total : 0.0);
    }
    printf("GOP 共 %d 个，长度（帧数）：", st->nb_gops);
    for (int i = 0; i < st->nb_gops; i++)
        printf("%s%d", i ? " " : "", st->gop_len[i]);
    printf("\n");
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVStream *stream;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    Stats st = { 0 };
    int ret, index;

    if (argc < 2 || argc > 3 || (argc == 3 && strcmp(argv[2], "mvs"))) {
        fprintf(stderr, "用法：%s <输入文件> [mvs]\n", argv[0]);
        return 1;
    }

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

    /*
     * 让解码器把 pkt->opaque 原样拷到对应的 frame->opaque 上。
     * 解码器输出帧的顺序和送包的顺序不同，还会延迟，程序自己很难把帧和包对上号，
     * 这个标志就是为“给每个包带一点自己的数据，在帧上取回来”准备的。
     */
    dec->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    /*
     * 运动矢量默认不导出，因为整理这些数据要花时间。需要时设置 export_side_data。
     * 官方示例 extract_mvs.c 用的是另一种等价的写法：打开解码器时传选项 flags2=+export_mvs。
     */
    if (argc == 3)
        dec->export_side_data |= AV_CODEC_EXPORT_DATA_MVS;

    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器 %s：%s\n", codec->name, av_err2str(ret));
        goto end;
    }

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    printf("序号 类型 关键帧  时间戳                  包的大小      帧上的 side data\n");
    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == index) {
            /* 把包的大小放进 opaque。opaque 是一个指针，这里只是借它存一个整数。 */
            pkt->opaque = (void *)(intptr_t)pkt->size;
            ret = decode(dec, pkt, frame, stream->time_base, &st);
        }
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    ret = decode(dec, NULL, frame, stream->time_base, &st);
    if (ret < 0)
        goto end;

    print_stats(&st);
    print_stream_side_data(stream);

end:
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
