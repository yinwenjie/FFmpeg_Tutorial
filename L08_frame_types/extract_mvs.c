/*
 * Copyright (c) 2012 Stefano Sabatini
 * Copyright (c) 2014 Clément Bœsch
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
 * @file libavcodec motion vectors extraction API usage example
 * @example extract_mvs.c
 *
 * Read from input file, decode video stream and print a motion vectors
 * representation to stdout.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/extract_mvs.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它解码一个文件里的视频，把解码器在每一帧上导出的运动矢量打印成 CSV 表格。
 * 打开文件、建立解码器、读包解码的部分和 L07 的 demux_decode.c 相同，只处理视频一路。
 * 新内容有两处：打开解码器时用一个选项要求它导出运动矢量；
 * 解出一帧之后用 av_frame_get_side_data() 把运动矢量取出来。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0031 到 0033），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 frame_info.c。
 */

#include <libavutil/motion_vector.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL;
static AVStream *video_stream = NULL;
static const char *src_filename = NULL;

static int video_stream_idx = -1;
static AVFrame *frame = NULL;
static int video_frame_count = 0;

static int decode_packet(const AVPacket *pkt)
{
    int ret = avcodec_send_packet(video_dec_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error while sending a packet to the decoder: %s\n", av_err2str(ret));
        return ret;
    }

    while (ret >= 0)  {
        ret = avcodec_receive_frame(video_dec_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            fprintf(stderr, "Error while receiving a frame from the decoder: %s\n", av_err2str(ret));
            return ret;
        }

        if (ret >= 0) {
            int i;
            AVFrameSideData *sd;

            video_frame_count++;
            /*
             * 课程注释：side data 是附在帧上的额外数据，一帧可以带很多种，每种有一个类型编号。
             * av_frame_get_side_data() 按类型去找，找到就返回一个 AVFrameSideData，里面是一块内存（data）和它的大小（size）；
             * 这一帧上没有这种数据时返回 NULL。I 帧不做帧间预测，没有运动矢量，返回的就是 NULL。
             */
            sd = av_frame_get_side_data(frame, AV_FRAME_DATA_MOTION_VECTORS);
            if (sd) {
                /*
                 * 课程注释：side data 里的内存是什么结构，由类型决定。运动矢量这一种是一个 AVMotionVector 数组，
                 * 定义在 libavutil/motion_vector.h。数组有多少个元素，用总大小除以一个元素的大小得到。
                 * 下面的 for 循环拿 int 和这个无符号的商比较，脱离源码树、带 -Wextra 编译时有一个符号比较的警告，不影响运行。
                 */
                const AVMotionVector *mvs = (const AVMotionVector *)sd->data;
                for (i = 0; i < sd->size / sizeof(*mvs); i++) {
                    const AVMotionVector *mv = &mvs[i];
                    /*
                     * 课程注释：每个运动矢量打印一行。各列的含义：
                     * framenum 第几帧（从 1 数起，按输出顺序）；source 参考帧的方向，负数是过去的帧，正数是将来的帧；
                     * blockw、blockh 这一块的宽和高；srcx、srcy 这一块的内容在参考帧里的位置（块的中心）；
                     * dstx、dsty 这一块在当前帧里的位置（块的中心）；flags 目前总是 0；
                     * motion_x、motion_y、motion_scale 精确的位移：src 等于 dst 加上 motion / motion_scale。
                     * H.264 的运动矢量精确到四分之一像素，motion_scale 是 4。
                     */
                    printf("%d,%2d,%2d,%2d,%4d,%4d,%4d,%4d,0x%"PRIx64",%4d,%4d,%4d\n",
                        video_frame_count, mv->source,
                        mv->w, mv->h, mv->src_x, mv->src_y,
                        mv->dst_x, mv->dst_y, mv->flags,
                        mv->motion_x, mv->motion_y, mv->motion_scale);
                }
            }
            av_frame_unref(frame);
        }
    }

    return 0;
}

static int open_codec_context(AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret;
    AVStream *st;
    AVCodecContext *dec_ctx = NULL;
    const AVCodec *dec = NULL;
    AVDictionary *opts = NULL;

    ret = av_find_best_stream(fmt_ctx, type, -1, -1, &dec, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        int stream_idx = ret;
        st = fmt_ctx->streams[stream_idx];

        dec_ctx = avcodec_alloc_context3(dec);
        if (!dec_ctx) {
            fprintf(stderr, "Failed to allocate codec\n");
            /* 课程注释：分配失败应该返回 AVERROR(ENOMEM)，这里写成了 EINVAL（patch 0032 顺带改了）。 */
            return AVERROR(EINVAL);
        }

        ret = avcodec_parameters_to_context(dec_ctx, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "Failed to copy codec parameters to codec context\n");
            avcodec_free_context(&dec_ctx);
            return ret;
        }

        /*
         * 课程注释：这一讲的第一个新东西。解码器默认不导出运动矢量，因为整理这些数据要花时间。
         * 这里用一个 AVDictionary 给解码器传选项 flags2=+export_mvs，在 avcodec_open2() 的第三个参数里交给它。
         * 选项名和命令行的 -flags2 +export_mvs 是同一个。用字典传选项的办法在 L13 讲编码参数时会大量用到。
         * 另一种等价的写法是直接设置 dec_ctx->export_side_data |= AV_CODEC_EXPORT_DATA_MVS，frame_info.c 用的是这一种。
         * 不是每个解码器都支持：MPEG-1、MPEG-2、MPEG-4、H.264 等软件解码器支持，不支持的解码器不报错，只是帧上没有这种数据。
         */
        /* Init the video decoder */
        av_dict_set(&opts, "flags2", "+export_mvs", 0);
        ret = avcodec_open2(dec_ctx, dec, &opts);
        av_dict_free(&opts);
        if (ret < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            /*
             * 课程注释：avcodec_open2() 失败时直接返回，没有释放 dec_ctx。
             * 上面拷参数失败时是释放了的。dec_ctx 这时还没有存进全局变量，main() 也释放不到它（patch 0032）。
             */
            return ret;
        }

        video_stream_idx = stream_idx;
        video_stream = fmt_ctx->streams[video_stream_idx];
        video_dec_ctx = dec_ctx;
    }

    return 0;
}

int main(int argc, char **argv)
{
    int ret = 0;
    AVPacket *pkt = NULL;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <video>\n", argv[0]);
        exit(1);
    }
    src_filename = argv[1];

    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }

    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }

    /* 课程注释：返回值没有用。成功与否靠下面检查 video_stream 是不是 NULL 来判断。 */
    open_codec_context(fmt_ctx, AVMEDIA_TYPE_VIDEO);

    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!video_stream) {
        fprintf(stderr, "Could not find video stream in the input, aborting\n");
        /*
         * 课程注释：和 demux_decode.c 同样的问题：main() 最后是 return ret < 0，这里把 ret 设成 1，
         * 所以输入里没有视频流时打印了错误信息，退出码却是 0（patch 0031）。
         */
        ret = 1;
        goto end;
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 课程注释：CSV 的表头。结果打印在标准输出上，使用时重定向到文件。
     * 程序从头到尾没有检查过输出有没有写成功，磁盘满了也照常结束（patch 0033）。
     */
    printf("framenum,source,blockw,blockh,srcx,srcy,dstx,dsty,flags,motion_x,motion_y,motion_scale\n");

    /* read frames from the file */
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx)
            ret = decode_packet(pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    /* 课程注释：冲刷。H.264 解码器里留着最后几帧，它们的运动矢量也要靠这一步才能拿到。 */
    /* flush cached frames */
    decode_packet(NULL);

end:
    avcodec_free_context(&video_dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0;
}
