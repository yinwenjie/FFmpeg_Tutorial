/*
 * Copyright (c) 2013 Stefano Sabatini
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
 * @file libavformat/libavcodec demuxing and muxing API usage example
 * @example remux.c
 *
 * Remux streams from one container format to another. Data is copied from the
 * input to the output without transcoding.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/remux.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它把输入文件里的压缩数据包原样搬进另一种容器，不解码也不编码。
 * 读它要抓住两件事：输出一侧的五个步骤，以及包的时间戳怎样从输入流的时间基换算到输出流的时间基。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0010、0012 到 0015），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 remux_cut.c。
 */

#include <libavutil/mem.h>
#include <libavutil/timestamp.h>
#include <libavformat/avformat.h>

/*
 * 课程注释：打印一个包的时间信息。av_ts2str() 把时间戳整数转成字符串（AV_NOPTS_VALUE 显示为 NOPTS），
 * av_ts2timestr() 再乘上时间基，得到以秒为单位的字符串。时间基取自这个包所属的流。
 */
static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt, const char *tag)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("%s: pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           tag,
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

int main(int argc, char **argv)
{
    const AVOutputFormat *ofmt = NULL;
    AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
    AVPacket *pkt = NULL;
    const char *in_filename, *out_filename;
    int ret, i;
    int stream_index = 0;
    int *stream_mapping = NULL;
    int stream_mapping_size = 0;

    if (argc < 3) {
        printf("usage: %s input output\n"
               "API example program to remux a media file with libavformat and libavcodec.\n"
               "The output format is guessed according to the file extension.\n"
               "\n", argv[0]);
        return 1;
    }

    in_filename  = argv[1];
    out_filename = argv[2];

    /* 课程注释：AVPacket 必须用 av_packet_alloc() 分配。整个程序只用这一个，循环里反复使用。 */
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        return 1;
    }

    /* 课程注释：输入一侧和 L03 相同：open，然后 find_stream_info。 */
    if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
        fprintf(stderr, "Could not open input file '%s'", in_filename);
        goto end;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
        fprintf(stderr, "Failed to retrieve input stream information");
        goto end;
    }

    av_dump_format(ifmt_ctx, 0, in_filename, 0);

    /*
     * 课程注释：输出第 1 步，创建输出用的 AVFormatContext。
     * 第二、第三个参数都是 NULL 时，按输出文件名的扩展名选择容器格式（复用器）。
     * 这个函数有返回值，示例没有接收，失败时一律报 AVERROR_UNKNOWN（见 0014 号 patch）。
     */
    avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, out_filename);
    if (!ofmt_ctx) {
        fprintf(stderr, "Could not create output context\n");
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /*
     * 课程注释：stream_mapping[输入流序号] = 输出流序号，-1 表示这路流不要。
     * 输入的流不一定都搬过去，所以输入和输出的序号可能不同，需要一张对照表。
     */
    stream_mapping_size = ifmt_ctx->nb_streams;
    stream_mapping = av_calloc(stream_mapping_size, sizeof(*stream_mapping));
    if (!stream_mapping) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ofmt = ofmt_ctx->oformat;

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *out_stream;
        AVStream *in_stream = ifmt_ctx->streams[i];
        AVCodecParameters *in_codecpar = in_stream->codecpar;

        /* 课程注释：只搬音频、视频、字幕三种流，数据流和附件流跳过。 */
        if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO &&
            in_codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) {
            stream_mapping[i] = -1;
            continue;
        }

        stream_mapping[i] = stream_index++;

        /* 课程注释：输出第 2 步，在输出里新建一路流。 */
        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            fprintf(stderr, "Failed allocating output stream\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        /*
         * 课程注释：把输入流的编码参数整个拷给输出流。L03 说过，AVCodecParameters
         * 是各个环节之间传递流参数的统一格式，这里是第一次用到。
         * 示例只拷了 codecpar，流的元数据（比如语言）和 disposition 没有拷，
         * 容器的元数据也没有拷，输出文件会丢掉这些信息（见 0013 号 patch）。
         */
        ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters\n");
            goto end;
        }
        /*
         * 课程注释：codec_tag 是容器里标识编码的四字符代码，各种容器的写法不同
         * （L03 里看到 MP4 用 avc1）。清零表示让复用器按自己的规则重新选。
         */
        out_stream->codecpar->codec_tag = 0;
    }
    av_dump_format(ofmt_ctx, 0, out_filename, 1);

    /*
     * 课程注释：输出第 3 步，打开输出文件，得到负责写字节的 AVIOContext。
     * 输入一侧这一步由 avformat_open_input() 代劳，输出一侧要自己做。
     * 少数格式不写文件（带 AVFMT_NOFILE 标志），所以先判断。
     */
    if (!(ofmt->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open output file '%s'", out_filename);
            goto end;
        }
    }

    /*
     * 课程注释：输出第 4 步，写文件头。
     * 注意：复用器会在这个函数里确定每路输出流的 time_base，可能和之前的值不同。
     * 所以换算时间戳必须在它之后，并且每次都读 out_stream->time_base，不能自己假设。
     */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file\n");
        goto end;
    }

    while (1) {
        AVStream *in_stream, *out_stream;

        /*
         * 课程注释：从输入读一个包。名字里有 frame，读出来的其实是压缩数据包。
         * 包的 pts、dts、duration 以它所属输入流的 time_base 为单位。
         * 返回负值表示读完了（AVERROR_EOF）或出错。
         */
        ret = av_read_frame(ifmt_ctx, pkt);
        if (ret < 0)
            break;

        in_stream  = ifmt_ctx->streams[pkt->stream_index];
        /* 课程注释：不要的流，把包里的数据释放掉，读下一个。 */
        if (pkt->stream_index >= stream_mapping_size ||
            stream_mapping[pkt->stream_index] < 0) {
            av_packet_unref(pkt);
            continue;
        }

        /*
         * 课程注释：把包上的流序号改成输出流的序号。
         * 下面第三行的 log_packet(ifmt_ctx, pkt, "in") 用的已经是改过的序号，
         * 有流被跳过时会查错流的时间基（见 0012 号 patch）。
         */
        pkt->stream_index = stream_mapping[pkt->stream_index];
        out_stream = ofmt_ctx->streams[pkt->stream_index];
        log_packet(ifmt_ctx, pkt, "in");

        /* copy packet */
        /*
         * 课程注释：本讲的核心。把包的 pts、dts、duration 从输入流的时间基换算到输出流的时间基。
         * 数据一个字节都不动，只改这三个数。
         * 输入的包如果没有 dts（MKV 里带 B 帧的视频，开头几个包就是这样），这里原样传下去，
         * 复用器会打印一条弃用警告（见 0015 号 patch）。
         */
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
        /* 课程注释：pos 是这个包在输入文件里的字节位置，对输出文件没有意义，设为 -1 表示未知。 */
        pkt->pos = -1;
        log_packet(ofmt_ctx, pkt, "out");

        /*
         * 课程注释：输出第 5 步，写包。带 interleaved 的这个函数会把包暂存起来，
         * 按 dts 的先后把各路流的包交错着写出去，调用者不用操心音视频包的先后顺序。
         * 它会接管包里的数据并把 pkt 清空，所以循环里不需要再调用 av_packet_unref()。
         */
        ret = av_interleaved_write_frame(ofmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error muxing packet\n");
            break;
        }
    }

    /*
     * 课程注释：写文件尾。暂存的包在这里全部写出；MP4 的 moov 也是这时才写的，
     * 所以 L03 看到 moov 在文件末尾。不调用它，输出的 MP4 无法播放。
     */
    av_write_trailer(ofmt_ctx);
end:
    av_packet_free(&pkt);

    /* 课程注释：输入用 avformat_close_input() 关闭；输出要先关文件，再用 avformat_free_context() 释放。 */
    avformat_close_input(&ifmt_ctx);

    /* close output */
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    av_freep(&stream_mapping);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}
