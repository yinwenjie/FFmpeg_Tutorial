/*
 * L05 视频解码：把 parser 和解码器的每一次调用及其返回值打印出来。
 *
 * 用法：decode_trace <解码器名> <裸流文件> [线程数，默认 1] [最多打印多少个包，默认 10，0 表示全部]
 *
 * 例子：
 *   decode_trace mpeg1video samples/video_360p25.m1v
 *   decode_trace h264 out/small_360p25.h264 4
 *
 * 输入必须是裸流（没有容器）。程序不保存图像，只观察三件事：
 * parser 切出来的包、avcodec_send_packet() 的返回值、avcodec_receive_frame() 的返回值。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

#define INBUF_SIZE 4096

/* 把返回值变成容易读的名字。EAGAIN 和 EOF 是正常的状态，不是错误。 */
static const char *ret_name(int ret)
{
    /*
     * 注意这里不能写 return av_err2str(ret)。av_err2str() 是一个宏，
     * 它把字符串放在当前函数的栈上，函数一返回这块内存就失效了。
     * 要把错误信息带出函数，得自己准备缓冲区，再调用 av_strerror()。
     */
    static char buf[AV_ERROR_MAX_STRING_SIZE];

    if (ret == 0)
        return "0";
    if (ret == AVERROR(EAGAIN))
        return "EAGAIN";
    if (ret == AVERROR_EOF)
        return "EOF";
    av_strerror(ret, buf, sizeof(buf));
    return buf;
}

/*
 * 把解码器此刻能给出的帧全部取走。
 * 返回 0 表示取完了（遇到 EAGAIN 或 EOF），返回负数表示真正的错误。
 */
static int receive_frames(AVCodecContext *dec, AVFrame *frame, int verbose, int64_t *nb_frames)
{
    for (;;) {
        int ret = avcodec_receive_frame(dec, frame);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            if (verbose)
                printf("   receive → %s\n", ret_name(ret));
            return 0;
        }
        if (ret < 0) {
            if (verbose)
                printf("   receive → %s\n", ret_name(ret));
            return ret;
        }

        (*nb_frames)++;
        if (verbose)
            printf("   receive → 帧 %" PRId64 "(%c)", *nb_frames,
                   av_get_picture_type_char(frame->pict_type));
        /*
         * 这里本该使用 frame 里的图像。用完之后不必手工 av_frame_unref()：
         * 下一次 avcodec_receive_frame() 会先把 frame 里原有的内容释放掉。
         */
    }
}

int main(int argc, char **argv)
{
    const AVCodec *codec;
    AVCodecParserContext *parser = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    FILE *in = NULL;
    /* 缓冲区末尾多留 AV_INPUT_BUFFER_PADDING_SIZE 个字节并清零，解码器读过头时不会越界。 */
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    int64_t limit = 10, nb_reads = 0, nb_bytes = 0, nb_packets = 0;
    int64_t nb_frames = 0, nb_before_flush;
    int threads = 1, eof = 0, ret = 0;

    if (argc < 3 || argc > 5) {
        fprintf(stderr, "用法：%s <解码器名> <裸流文件> [线程数，默认 1] "
                "[最多打印多少个包，默认 10，0 表示全部]\n", argv[0]);
        return 1;
    }
    if (argc >= 4)
        threads = atoi(argv[3]);
    if (argc == 5)
        limit = atoll(argv[4]);

    /* 裸流不会告诉我们它是什么编码，所以解码器由命令行指定。 */
    codec = avcodec_find_decoder_by_name(argv[1]);
    if (!codec) {
        fprintf(stderr, "找不到名为 %s 的解码器\n", argv[1]);
        return 1;
    }
    /* parser 负责把连续的字节流切成一帧一帧的包。不是每种编码格式都有 parser。 */
    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "编码格式 %s 没有 parser\n", avcodec_get_name(codec->id));
        return 1;
    }

    dec   = avcodec_alloc_context3(codec);
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!dec || !pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 线程数要在 avcodec_open2() 之前设置。默认是 1；设成 0 表示让 FFmpeg 按 CPU 核数自己决定。
     * 多线程解码更快，代价是解码器要多攒几个包才开始输出。
     */
    dec->thread_count = threads;

    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器：%s\n", av_err2str(ret));
        goto end;
    }
    printf("解码器 %s，线程数 %d\n\n", codec->name, dec->thread_count);

    in = fopen(argv[2], "rb");
    if (!in) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开 %s：%s\n", argv[2], av_err2str(ret));
        goto end;
    }

    while (!eof) {
        uint8_t *data = inbuf;
        size_t data_size = fread(inbuf, 1, INBUF_SIZE, in);

        if (ferror(in)) {
            fprintf(stderr, "读 %s 出错\n", argv[2]);
            ret = AVERROR(EIO);
            goto end;
        }
        /* 读到 0 字节就是文件结束。这时还要用空输入再调一次 parser，让它交出攒着的最后一帧。 */
        eof = !data_size;
        if (!eof) {
            nb_reads++;
            nb_bytes += data_size;
        }

        while (data_size > 0 || eof) {
            int verbose;

            /*
             * 返回值是 parser 这次用掉的输入字节数。
             * pkt->size 为 0：还没凑够一帧；不为 0：pkt->data 指向完整的一帧压缩数据。
             */
            ret = av_parser_parse2(parser, dec, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                fprintf(stderr, "parser 出错：%s\n", av_err2str(ret));
                goto end;
            }
            data      += ret;
            data_size -= ret;

            if (!pkt->size) {
                if (eof)
                    break;
                continue;
            }

            nb_packets++;
            verbose = limit == 0 || nb_packets <= limit;

            /* 送一个包。返回 0 表示解码器收下了，解码结果要另外去取。 */
            ret = avcodec_send_packet(dec, pkt);
            if (verbose)
                printf("包 %-4" PRId64 " %6d 字节 %c   send → %s", nb_packets, pkt->size,
                       av_get_picture_type_char(parser->pict_type), ret_name(ret));
            if (ret < 0) {
                if (verbose)
                    printf("\n");
                fprintf(stderr, "送包出错：%s\n", av_err2str(ret));
                goto end;
            }

            /* 每送一个包，就把能取的帧全部取走。 */
            ret = receive_frames(dec, frame, verbose, &nb_frames);
            if (ret < 0) {
                fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
                goto end;
            }
        }
    }
    if (limit != 0 && nb_packets > limit)
        printf("  ……（共 %" PRId64 " 个包，只打印了前 %" PRId64 " 个）\n", nb_packets, limit);

    nb_before_flush = nb_frames;

    /*
     * 冲刷：送一个 NULL，表示没有更多数据了。
     * 解码器把内部攒着的帧全部交出来，最后 avcodec_receive_frame() 返回 AVERROR_EOF。
     */
    ret = avcodec_send_packet(dec, NULL);
    printf("冲刷              send(NULL) → %s", ret_name(ret));
    if (ret < 0) {
        printf("\n");
        goto end;
    }
    ret = receive_frames(dec, frame, 1, &nb_frames);
    if (ret < 0) {
        fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
        goto end;
    }

    printf("\n== 统计 ==\n");
    printf("读文件 %" PRId64 " 次，共 %" PRId64 " 字节；parser 切出 %" PRId64 " 个包\n",
           nb_reads, nb_bytes, nb_packets);
    printf("冲刷之前取到 %" PRId64 " 帧，冲刷时又取到 %" PRId64 " 帧，共 %" PRId64 " 帧\n",
           nb_before_flush, nb_frames - nb_before_flush, nb_frames);

end:
    if (in)
        fclose(in);
    av_parser_close(parser);
    avcodec_free_context(&dec);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
