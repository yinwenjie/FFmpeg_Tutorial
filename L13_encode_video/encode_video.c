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
 * @file libavcodec encoding video API usage example
 * @example encode_video.c
 *
 * Generate synthetic video data and encode it to an output file.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/encode_video.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它用程序算出 25 帧 352x288 的渐变画面，交给指定名字的编码器编码，把得到的包直接写进文件。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0016、0053 到 0055），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 encode_yuv.c。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>

#include <libavutil/opt.h>
#include <libavutil/imgutils.h>

/*
 * 课程注释：编码的核心，和 L05 的 decode() 正好是反过来的：
 * 解码是“送包（avcodec_send_packet）、取帧（avcodec_receive_frame）”，
 * 编码是“送帧（avcodec_send_frame）、取包（avcodec_receive_packet）”。
 * frame 为 NULL 表示输入结束，让编码器把留在内部的帧都编完交出来（冲刷）。
 */
static void encode(AVCodecContext *enc_ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *outfile)
{
    int ret;

    /* send the frame to the encoder */
    if (frame)
        printf("Send frame %3"PRId64"\n", frame->pts);

    ret = avcodec_send_frame(enc_ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame for encoding\n");
        exit(1);
    }

    /*
     * 课程注释：送一帧之后要循环取包，直到编码器说“没有了”。
     * 送一帧不一定马上得到一个包：编码器要先看到后面的帧才能决定前面的帧怎么编（B 帧、码率控制的预读），
     * 所以开头会连续几次一个包都取不到，返回 AVERROR(EAGAIN)；冲刷时则一次送帧（NULL）取出很多个包。
     */
    while (ret >= 0) {
        ret = avcodec_receive_packet(enc_ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error during encoding\n");
            exit(1);
        }

        /*
         * 课程注释：这里打印的是包的 pts。有 B 帧时包的输出顺序（解码顺序）和 pts 的顺序（显示顺序）不同，
         * 所以会看到 0、2、1、4、3 这样的序列，这就是 L08 讲的 dts 和 pts 的区别，现在是从编码器这一头看到的。
         */
        printf("Write packet %3"PRId64" (size=%5d)\n", pkt->pts, pkt->size);
        /*
         * 课程注释：把包的数据原样写进文件，没有检查 fwrite() 的返回值（patch 0053）。
         * 这样写出来的是“裸码流”，没有容器。MPEG-1/2 和 H.264（Annex B）的裸码流播放器能直接认，很多别的格式不行。
         */
        fwrite(pkt->data, 1, pkt->size, outfile);
        av_packet_unref(pkt);
    }
}

int main(int argc, char **argv)
{
    const char *filename, *codec_name;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    int i, ret, x, y;
    FILE *f;
    AVFrame *frame;
    AVPacket *pkt;
    uint8_t endcode[] = { 0, 0, 1, 0xb7 };

    /* 课程注释：原版在这里参数不够时退出码是 0（patch 0016 改成了 1）。 */
    if (argc <= 2) {
        fprintf(stderr, "Usage: %s <output file> <codec name>\n", argv[0]);
        exit(0);
    }
    filename = argv[1];
    codec_name = argv[2];

    /*
     * 课程注释：这句英文注释已经过时（patch 0055），实际是按命令行给的名字找编码器。
     * 编码器要按名字找：同一种格式可以有多个编码器，例如 H.264 有 libx264、h264_vaapi、h264_nvenc。
     */
    /* find the mpeg1video encoder */
    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        fprintf(stderr, "Codec '%s' not found\n", codec_name);
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    pkt = av_packet_alloc();
    if (!pkt)
        exit(1);

    /*
     * 课程注释：下面是编码参数。解码时这些信息在码流里，编码时必须由程序给出。
     * bit_rate 是目标码率，单位是比特每秒。400000 就是 400 kb/s。
     */
    /* put sample parameters */
    c->bit_rate = 400000;
    /* resolution must be a multiple of two */
    c->width = 352;
    c->height = 288;
    /* frames per second */
    /*
     * 课程注释：时间基是必须设置的：它是帧的 pts 的单位。设成 1/25，每秒 25 帧时第 n 帧的 pts 正好是 n。
     * framerate 是告诉编码器的帧率，供码率控制等使用。
     */
    c->time_base = (AVRational){1, 25};
    c->framerate = (AVRational){25, 1};

    /* emit one intra frame every ten frames
     * check frame pict_type before passing frame
     * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
     * then gop_size is ignored and the output of encoder
     * will always be I frame irrespective to gop_size
     */
    /*
     * 课程注释：gop_size 是关键帧间隔：最多每 10 帧出一个 I 帧。
     * max_b_frames 是两个非 B 帧之间最多放几个 B 帧。
     * pix_fmt 是送进去的帧的像素格式，必须是这个编码器支持的格式（本目录的 encoder_info 可以查）。
     */
    c->gop_size = 10;
    c->max_b_frames = 1;
    c->pix_fmt = AV_PIX_FMT_YUV420P;

    /*
     * 课程注释：preset 是编码器的“私有选项”（libx264 自己的选项，AVCodecContext 里没有对应的字段），
     * 所以要设置到 c->priv_data 上。slow 表示用更多的时间换更高的压缩率。
     * 这里没有检查返回值：不是所有 H.264 编码器都有 preset 这个选项，没有时设置被悄悄忽略（patch 0054）。
     * encode_yuv.c 用选项字典传参数，通用选项和私有选项写法相同，写错了也能发现。
     */
    if (codec->id == AV_CODEC_ID_H264)
        av_opt_set(c->priv_data, "preset", "slow", 0);

    /* open it */
    /*
     * 课程注释：打开编码器。参数不合法（像素格式不支持、尺寸不合要求等）会在这里失败。
     * 第三个参数是选项字典，这里没有用。
     */
    ret = avcodec_open2(c, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open codec: %s\n", av_err2str(ret));
        exit(1);
    }

    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
    frame->format = c->pix_fmt;
    frame->width  = c->width;
    frame->height = c->height;

    /* 课程注释：按上面填的格式和尺寸，为帧分配图像缓冲区。 */
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate the video frame data\n");
        exit(1);
    }

    /* encode 1 second of video */
    for (i = 0; i < 25; i++) {
        fflush(stdout);

        /* Make sure the frame data is writable.
           On the first round, the frame is fresh from av_frame_get_buffer()
           and therefore we know it is writable.
           But on the next rounds, encode() will have called
           avcodec_send_frame(), and the codec may have kept a reference to
           the frame in its internal structures, that makes the frame
           unwritable.
           av_frame_make_writable() checks that and allocates a new buffer
           for the frame only if necessary.
         */
        /*
         * 课程注释：上面的英文注释解释了为什么需要这一步：编码器可能还留着这一帧的引用，
         * 直接改写缓冲区会破坏编码器正在用的参考帧。这是“同一个 AVFrame 反复使用”时必须有的一步。
         */
        ret = av_frame_make_writable(frame);
        if (ret < 0)
            exit(1);

        /* Prepare a dummy image.
           In real code, this is where you would have your own logic for
           filling the frame. FFmpeg does not care what you put in the
           frame.
         */
        /* Y */
        for (y = 0; y < c->height; y++) {
            for (x = 0; x < c->width; x++) {
                frame->data[0][y * frame->linesize[0] + x] = x + y + i * 3;
            }
        }

        /* Cb and Cr */
        for (y = 0; y < c->height/2; y++) {
            for (x = 0; x < c->width/2; x++) {
                frame->data[1][y * frame->linesize[1] + x] = 128 + y + i * 2;
                frame->data[2][y * frame->linesize[2] + x] = 64 + x + i * 5;
            }
        }

        /* 课程注释：每一帧都要有 pts，单位是编码器的 time_base，并且要递增。 */
        frame->pts = i;

        /* encode the image */
        encode(c, frame, pkt, f);
    }

    /*
     * 课程注释：冲刷编码器。少了这一步，编码器里留着的帧就丢了，
     * 留多少取决于编码器和参数：mpeg1video 留 1 帧，libx264 可能留几十帧。
     */
    /* flush the encoder */
    encode(c, NULL, pkt, f);

    /* Add sequence end code to have a real MPEG file.
       It makes only sense because this tiny examples writes packets
       directly. This is called "elementary stream" and only works for some
       codecs. To create a valid file, you usually need to write packets
       into a proper file format or protocol; see mux.c.
     */
    /*
     * 课程注释：MPEG-1/2 的码流结尾有一个 4 字节的结束码，容器的复用器会负责写，这里没有容器，所以手工写。
     * 这里的 fwrite() 和下面的 fclose() 的结果也没有检查（patch 0053）。
     */
    if (codec->id == AV_CODEC_ID_MPEG1VIDEO || codec->id == AV_CODEC_ID_MPEG2VIDEO)
        fwrite(endcode, 1, sizeof(endcode), f);
    fclose(f);

    avcodec_free_context(&c);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    return 0;
}
