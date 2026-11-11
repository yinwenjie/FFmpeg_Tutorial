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
 * @file libavcodec audio decoding API usage example
 * @example decode_audio.c
 *
 * Decode data from an MP2 input file and generate a raw audio file to
 * be played with ffplay.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/decode_audio.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它和 L05 的 decode_video.c 是一对：同样不用 libavformat，用 fread 读一个 MP2 音频裸流，
 * 用 parser 切成包，交给解码器，把解出来的采样写成一个没有文件头的 PCM 文件。
 * 读它要抓住两件事：音频帧里的采样是怎么摆放的（decode() 里的两层循环），
 * 以及它填输入缓冲区的办法和 decode_video.c 有什么不同。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录
 * （0006、0016、0019 到 0024），下面在出问题的位置都做了标注。
 * 自己写程序时参考本目录的 decode_wav.c。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/frame.h>
#include <libavutil/mem.h>

#include <libavcodec/avcodec.h>

/*
 * 课程注释：输入缓冲区 20480 字节。缓冲区里剩下的数据少于 4096 字节时就从文件补充。
 * 128 kb/s 的 MP2 一帧只有 417 或 418 字节，所以缓冲区里任何时候都放着很多帧。
 */
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

/*
 * 课程注释：把 FFmpeg 的采样格式换成 ffplay 的 -f 参数要用的名字，例如
 * AV_SAMPLE_FMT_S16 对应 s16le（le 表示小端字节序）。AV_NE(be, le) 在大端机器上取第一个，
 * 在小端机器上取第二个：PCM 文件里的字节序就是写它的那台机器的字节序。
 * 表里只有 packed 格式。planar 格式（s16p、fltp 等）只存在于内存里，没有对应的文件格式。
 *
 * 课程注释：脱离源码树、带 -Wextra 编译时，下面的 for 循环有一个符号比较的警告
 * （int 和 FF_ARRAY_ELEMS 的无符号结果比较），不影响运行。本目录的 Makefile 对这个文件关掉了这个警告。
 */
static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}

/*
 * 课程注释：送一个包，取出所有能取的帧，把采样写进文件。
 * send 和 receive 的配合与 L05 完全一样，音频和视频用的是同一套接口。
 */
static void decode(AVCodecContext *dec_ctx, AVPacket *pkt, AVFrame *frame,
                   FILE *outfile)
{
    int i, ch;
    int ret, data_size;

    /* send the packet with the compressed data to the decoder */
    ret = avcodec_send_packet(dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting the packet to the decoder\n");
        exit(1);
    }

    /* read all the output frames (in general there may be any number of them */
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during decoding\n");
            exit(1);
        }
        /*
         * 课程注释：av_get_bytes_per_sample() 返回一个采样占几个字节：s16 和 s16p 是 2，
         * flt 和 fltp 是 4。它不乘声道数。
         */
        data_size = av_get_bytes_per_sample(dec_ctx->sample_fmt);
        if (data_size < 0) {
            /* This should not occur, checking just for paranoia */
            fprintf(stderr, "Failed to calculate data size\n");
            exit(1);
        }
        /*
         * 课程注释：本讲最重要的几行。frame->nb_samples 是这一帧里每个声道的采样数，
         * MP2 固定是 1152。MP2 解码器输出的格式是 s16p，p 表示 planar：
         * 每个声道一个平面，data[0] 是左声道的 1152 个采样，data[1] 是右声道的 1152 个采样。
         * PCM 文件要的是交错（packed）排列：左、右、左、右……
         * 所以外层循环走采样，内层循环走声道，每次从对应的平面里取一个采样写出去。
         *
         * 课程注释：这种写法只对 planar 格式成立。解码器输出 packed 格式时只有 data[0] 一个平面，
         * data[1] 是空指针，程序会崩溃。另外 data[] 只有 8 个位置，声道更多时要用 extended_data
         * （见 upstream_patches 0021）。fwrite 的返回值也没有检查（0020）。
         */
        for (i = 0; i < frame->nb_samples; i++)
            for (ch = 0; ch < dec_ctx->ch_layout.nb_channels; ch++)
                fwrite(frame->data[ch] + data_size*i, 1, data_size, outfile);
    }
}

/*
 * 课程注释：main 的骨架和 decode_video.c 一样：找解码器、建 parser、建上下文、打开，
 * 然后循环“读文件、切包、解码”，最后冲刷。
 */
int main(int argc, char **argv)
{
    const char *outfilename, *filename;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    AVCodecParserContext *parser = NULL;
    int len, ret;
    FILE *f, *outfile;
    uint8_t inbuf[AUDIO_INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
    uint8_t *data;
    size_t   data_size;
    AVPacket *pkt;
    AVFrame *decoded_frame = NULL;
    enum AVSampleFormat sfmt;
    int n_channels = 0;
    const char *fmt;

    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        /*
         * 课程注释：用法说明没有说输入必须是 MP2 裸流（见 upstream_patches 0023），
         * 而且参数不够是用法错误，退出码应该是非 0（0016）。
         */
        exit(0);
    }
    filename    = argv[1];
    outfilename = argv[2];

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1); /* or proper cleanup and returning */
    }

    /*
     * 课程注释：解码器写死为 MP2。avcodec_find_decoder(AV_CODEC_ID_MP2) 找到的是名为 mp2 的定点解码器，
     * 它输出 s16p。同一种编码还有一个浮点解码器 mp2float，输出 fltp，要用名字去找。
     */
    /* find the MPEG audio decoder */
    codec = avcodec_find_decoder(AV_CODEC_ID_MP2);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "Parser not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

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
    outfile = fopen(outfilename, "wb");
    if (!outfile) {
        fprintf(stderr, "Could not open %s\n", outfilename);
        exit(1);
    }

    /*
     * 课程注释：下面的读循环和 decode_video.c 的写法不同。
     * decode_video.c 每次读一块，交给 parser，直到这一块被吃完再读下一块。
     * 这里维护一个“滑动”的缓冲区：data 指向还没处理的数据，data_size 是剩下的字节数。
     * 每处理完一个包，如果剩下的数据不足 4096 字节，就用 memmove 把剩下的挪到缓冲区开头，
     * 再从文件读一些接在后面。两种写法都对。对 parser 来说，一次给多少字节无所谓。
     *
     * 课程注释：三个问题。
     * 一，AVFrame 放在循环里面第一次用到时才分配。输入文件是空的时候循环体一次也不执行，
     *     后面冲刷解码器时传进去的是空指针，程序崩溃（见 upstream_patches 0019）。
     * 二，缓冲区的末尾需要 AV_INPUT_BUFFER_PADDING_SIZE 个值为 0 的字节，inbuf 是没有初始化的局部数组，
     *     这里也没有清零。decode_video.c 里有一句 memset，这里漏了（0024）。
     * 三，读循环结束后没有用长度 0 再调用一次 parser。MP2 的 parser 不攒数据，所以完整的文件没有影响，
     *     换成别的编码格式会丢掉结尾的几帧（0022）。fread 出错也没有检查（0020）。
     */
    /* decode until eof */
    data      = inbuf;
    data_size = fread(inbuf, 1, AUDIO_INBUF_SIZE, f);

    while (data_size > 0) {
        if (!decoded_frame) {
            if (!(decoded_frame = av_frame_alloc())) {
                fprintf(stderr, "Could not allocate audio frame\n");
                exit(1);
            }
        }

        ret = av_parser_parse2(parser, c, &pkt->data, &pkt->size,
                               data, data_size,
                               AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (ret < 0) {
            fprintf(stderr, "Error while parsing\n");
            exit(1);
        }
        data      += ret;
        data_size -= ret;

        if (pkt->size)
            decode(c, pkt, decoded_frame, outfile);

        if (data_size < AUDIO_REFILL_THRESH) {
            memmove(inbuf, data, data_size);
            data = inbuf;
            len = fread(data + data_size, 1,
                        AUDIO_INBUF_SIZE - data_size, f);
            if (len > 0)
                data_size += len;
        }
    }

    /*
     * 课程注释：冲刷解码器。这里的写法是把包的 data 置空、size 置 0 再送进去，
     * 效果和直接传 NULL 一样。音频解码器一般不攒帧，冲刷多半取不到东西，但这一步不能省。
     */
    /* flush the decoder */
    pkt->data = NULL;
    pkt->size = 0;
    decode(c, pkt, decoded_frame, outfile);

    /*
     * 课程注释：PCM 文件没有文件头，采样格式、声道数、采样率都要播放的人自己知道，
     * 所以程序在结束时把播放命令打印出来。
     *
     * 课程注释：下面的警告说“只输出第一个声道”，与实际不符，decode() 写的是全部声道。
     * 打印的 ffplay 命令用的 -ac 和 -ar 在新版 ffplay 里已经失效，要写成 -ch_layout 和 -sample_rate
     * （见 upstream_patches 0006，L02 已经遇到过）。
     */
    /* print output pcm infomations, because there have no metadata of pcm */
    sfmt = c->sample_fmt;

    if (av_sample_fmt_is_planar(sfmt)) {
        const char *packed = av_get_sample_fmt_name(sfmt);
        printf("Warning: the sample format the decoder produced is planar "
               "(%s). This example will output the first channel only.\n",
               packed ? packed : "?");
        sfmt = av_get_packed_sample_fmt(sfmt);
    }

    n_channels = c->ch_layout.nb_channels;
    if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
        goto end;

    printf("Play the output audio file with the command:\n"
           "ffplay -f %s -ac %d -ar %d %s\n",
           fmt, n_channels, c->sample_rate,
           outfilename);
    /*
     * 课程注释：到这里输出文件还没有关闭，写文件有没有出错也没有检查，
     * 磁盘满了照样提示“去播放输出文件”（见 upstream_patches 0020）。
     */
end:
    fclose(outfile);
    fclose(f);

    avcodec_free_context(&c);
    av_parser_close(parser);
    av_frame_free(&decoded_frame);
    av_packet_free(&pkt);

    return 0;
}
