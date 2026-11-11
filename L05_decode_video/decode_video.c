/*
 * Copyright (c) 2001 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file libavcodec video decoding API usage example
 * @example decode_video.c *
 *
 * Read from an MPEG1 video file, decode frames, and generate PGM images as
 * output.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/decode_video.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它不用 libavformat：直接用 fread 读一个 MPEG-1 视频裸流，用 parser 把字节流切成一帧一帧的包，
 * 再交给解码器，把解码出来的每一帧的亮度平面存成一个 PGM 灰度图。
 * 读它要抓住三件事：parser 怎么切包，send/receive 这一对函数怎么配合，读完之后为什么还要冲刷一次。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0016 到 0018），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 decode_trace.c 和 decode_yuv.c。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

/*
 * 课程注释：每次从文件读 4096 字节。这个数字和一帧有多大没有关系，
 * 一帧压缩数据通常比它大，要读好几次才凑齐。凑齐的工作由 parser 完成。
 */
#define INBUF_SIZE 4096

/*
 * 课程注释：把一个 8 位的灰度平面存成 PGM 文件。PGM 是最简单的图片格式：
 * 一个文本头（P5、宽、高、最大值 255），后面直接跟每个像素一个字节。
 * 参数 wrap 是每一行在内存里占的字节数（就是 linesize），xsize 是这一行里有效的像素数。
 * 注意循环的写法：每行从 buf + i * wrap 开始，只写 xsize 个字节。
 * linesize 可以比宽度大，多出来的是对齐用的填充，不能写进文件。本讲第 4 步专门演示写错的后果。
 *
 * 课程注释：这里打不开输出文件时只打印一条信息就返回，程序接着解码，最后退出码还是 0
 * （见 upstream_patches 0017）。
 */
static void pgm_save(unsigned char *buf, int wrap, int xsize, int ysize,
                     char *filename)
{
    FILE *f;
    int i;

    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        return;
    }
    fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);
    for (i = 0; i < ysize; i++)
        fwrite(buf + i * wrap, 1, xsize, f);
    fclose(f);
}

/*
 * 课程注释：送一个包进解码器，再把解码器此刻能给出的帧全部取走。
 * pkt 为 NULL 表示“没有更多数据了”，用于最后的冲刷。
 */
static void decode(AVCodecContext *dec_ctx, AVFrame *frame, AVPacket *pkt,
                   const char *filename)
{
    char buf[1024];
    int ret;

    /*
     * 课程注释：avcodec_send_packet() 把压缩数据交给解码器，返回 0 表示收下了。
     * 这个函数不返回解码结果，结果要用 avcodec_receive_frame() 去取。
     */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error sending a packet for decoding\n");
        exit(1);
    }

    /*
     * 课程注释：送一个包，可能取到 0 帧、1 帧或多帧，所以取帧要用循环，一直取到取不出来为止。
     * 两个返回值不是错误：
     *   AVERROR(EAGAIN)  解码器现在给不出帧，需要再送数据；
     *   AVERROR_EOF      解码器已经冲刷完，以后不会再有帧了。
     * 其余的负数才是真正的错误。
     */
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }

        /* 课程注释：frame_num 是解码器到目前为止输出的帧数，从 1 开始，这里拿它当文件名的序号。 */
        printf("saving frame %3"PRId64"\n", dec_ctx->frame_num);
        fflush(stdout);

        /*
         * 课程注释：frame 里的图像内存是解码器分配的，带引用计数。
         * 下一次 avcodec_receive_frame() 会先释放 frame 里原有的引用，所以循环里不需要手工 unref。
         * data[0] 是亮度平面的首地址，linesize[0] 是亮度平面每行占的字节数。
         * 这个示例只存了亮度，所以得到的是黑白图片；色度平面在 data[1] 和 data[2]。
         */
        /* the picture is allocated by the decoder. no need to
           free it */
        snprintf(buf, sizeof(buf), "%s-%"PRId64, filename, dec_ctx->frame_num);
        pgm_save(frame->data[0], frame->linesize[0],
                 frame->width, frame->height, buf);
    }
}

int main(int argc, char **argv)
{
    const char *filename, *outfilename;
    const AVCodec *codec;
    AVCodecParserContext *parser;
    AVCodecContext *c= NULL;
    FILE *f;
    AVFrame *frame;
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data;
    size_t   data_size;
    int ret;
    int eof;
    AVPacket *pkt;

    /*
     * 课程注释：两个参数都容易误解（见 upstream_patches 0018）。
     * 第一个必须是 MPEG-1 视频裸流，放在 MP4、AVI 或 MPEG-PS 容器里的不行；
     * 第二个是输出文件名的前缀，每一帧存成“前缀-序号”。
     * 另外参数不够时退出码是 0，脚本无从判断程序没有干活（见 upstream_patches 0016）。
     */
    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n"
                "And check your input file is encoded by mpeg1video please.\n", argv[0]);
        exit(0);
    }
    filename    = argv[1];
    outfilename = argv[2];

    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    /*
     * 课程注释：交给 parser 和解码器的缓冲区，末尾必须多留 AV_INPUT_BUFFER_PADDING_SIZE 个字节并清零。
     * 解码器为了速度会一次读好几个字节，数据损坏时可能读过头，这段填充保证读过头也不会越界。
     * 上面定义 inbuf 时已经多留了这么多，这里把它清零。
     */
    /* set end of buffer to 0 (this ensures that no overreading happens for damaged MPEG streams) */
    memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

    /*
     * 课程注释：解码器写死成 MPEG-1。裸流里没有“我是什么编码”的可靠标记，
     * 没有容器帮忙，就只能由调用者指定。L07 改用解封装器之后，编码格式从流参数里取。
     */
    /* find the MPEG-1 video decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_MPEG1VIDEO);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    /* 课程注释：parser 按编码格式创建，每种编码格式切帧的规则不同。 */
    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "parser not found\n");
        exit(1);
    }

    /* 课程注释：AVCodecContext 是一个解码器实例的全部状态，先分配，设置好参数之后再打开。 */
    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    /*
     * 课程注释：MPEG-1 的宽高、帧率都写在码流自己的序列头里，所以这里什么参数都不用设。
     * 有些格式（比如 MP4 里的 H.264）解码所需的参数存在容器里，要在打开之前拷进 AVCodecContext，L07 讲。
     */
    /* For some codecs, such as msmpeg4 and mpeg4, width and height
       MUST be initialized there because this information is not
       available in the bitstream. */

    /* open it */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /*
     * 课程注释：主循环有两层。外层每次读一块文件数据；内层把这一块交给 parser，
     * parser 可能用掉一部分或全部，凑够一帧时通过 pkt->data 和 pkt->size 把这一帧交出来。
     */
    do {
        /* read raw data from the input file */
        data_size = fread(inbuf, 1, INBUF_SIZE, f);
        /* 课程注释：读出错时直接跳出循环，没有任何提示，退出码也是 0（见 upstream_patches 0017）。 */
        if (ferror(f))
            break;
        /* 课程注释：读到 0 字节说明文件结束。这时还要再调一次 parser，让它把攒着的最后一帧交出来。 */
        eof = !data_size;

        /* use the parser to split the data into frames */
        data = inbuf;
        while (data_size > 0 || eof) {
            /*
             * 课程注释：av_parser_parse2() 的返回值是它这次用掉了多少字节的输入。
             * 输出在 pkt->data 和 pkt->size 里：pkt->size 为 0 表示还没凑够一帧，继续喂数据；
             * 不为 0 表示切出了完整的一帧。两个 AV_NOPTS_VALUE 是这块数据的 pts 和 dts，裸流没有时间戳。
             * 切出来的数据有时在 parser 自己的缓冲区里，有时就指向 inbuf，
             * 下一次调用 parser 或者重新读文件之后就失效，所以要马上送去解码。
             */
            ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                fprintf(stderr, "Error while parsing\n");
                exit(1);
            }
            data      += ret;
            data_size -= ret;

            if (pkt->size)
                decode(c, frame, pkt, outfilename);
            else if (eof)
                break;
        }
    } while (!eof);

    /*
     * 课程注释：冲刷。解码器内部会留着一些帧不马上输出，送 NULL 告诉它“没有数据了”，
     * 它才把剩下的帧交出来。漏掉这一步，输出就会少最后几帧，本讲第 3 步有演示。
     */
    /* flush the decoder */
    decode(c, frame, NULL, outfilename);

    fclose(f);

    av_parser_close(parser);
    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}
