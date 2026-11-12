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
 * @file libavcodec encoding audio API usage examples
 * @example encode_audio.c
 *
 * Generate a synthetic audio signal and encode it to an output MP2 file.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/encode_audio.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它用程序算出一段 440 Hz 的正弦波（200 帧，约 5.2 秒），用 MP2 编码器编码，把得到的包直接写进文件。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0010、0016、0056 到 0061），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 encode_wav.c。
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>

#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>

/* check that a given sample format is supported by the encoder */
/*
 * 课程注释：音频编码器对输入有三方面的要求：采样格式、采样率、声道布局。下面三个函数各查一样。
 * 查询用的都是 avcodec_get_supported_config()（L13 的 encoder_info.c 用它查过像素格式）：
 * 返回一个数组，数组为 NULL 表示编码器没有声明限制。
 */
static int check_sample_fmt(const AVCodec *codec, enum AVSampleFormat sample_fmt)
{
    const void *out_config;
    int ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                           0, &out_config, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error getting supported sample formats\n");
        exit(1);
    }
    const enum AVSampleFormat *p = out_config;

    if (!p)
        return 1;

    while (*p != AV_SAMPLE_FMT_NONE) {
        if (*p == sample_fmt)
            return 1;
        p++;
    }
    return 0;
}

/* 课程注释：这句英文注释已经过时（patch 0059）。下面的代码选的是“最接近 44100 Hz”的采样率，不是最高的。 */
/* just pick the highest supported samplerate */
static int select_sample_rate(const AVCodec *codec)
{
    const void *out_config;
    int ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_RATE,
                                           0, &out_config, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error getting supported sample rates\n");
        exit(1);
    }
    const int *p = out_config;
    int best_samplerate = 0;

    if (!p)
        return 44100;

    while (*p) {
        if (!best_samplerate || abs(44100 - *p) < abs(44100 - best_samplerate))
            best_samplerate = *p;
        p++;
    }
    return best_samplerate;
}

/* select layout with the highest channel count */
static int select_channel_layout(const AVCodec *codec, AVChannelLayout *dst)
{
    const void *out_config;
    int ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT,
                                           0, &out_config, NULL);
    if (ret < 0) {
        fprintf(stderr, "Error getting supported channel layouts\n");
        exit(1);
    }
    /*
     * 课程注释：best_ch_layout 没有初始值，只在下面的循环里赋值。
     * 如果编码器给出的是一个空列表，循环一次也不执行，后面就用到了未初始化的指针（patch 0056）。
     */
    const AVChannelLayout *p = out_config, *best_ch_layout;
    int best_nb_channels   = 0;

    if (!p)
        return av_channel_layout_copy(dst, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);

    while (p->nb_channels) {
        int nb_channels = p->nb_channels;

        if (nb_channels > best_nb_channels) {
            best_ch_layout   = p;
            best_nb_channels = nb_channels;
        }
        p++;
    }
    return av_channel_layout_copy(dst, best_ch_layout);
}

/*
 * 课程注释：和 L13 的视频编码是同一个结构：送一帧（avcodec_send_frame），循环取包（avcodec_receive_packet）。
 * 音频和视频在这一层没有区别。
 */
static void encode(AVCodecContext *ctx, AVFrame *frame, AVPacket *pkt,
                   FILE *output)
{
    int ret;

    /* send the frame for encoding */
    ret = avcodec_send_frame(ctx, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending the frame to the encoder\n");
        exit(1);
    }

    /* read all the available output packets (in general there may be any
     * number of them */
    while (ret >= 0) {
        ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding audio frame\n");
            exit(1);
        }

        /*
         * 课程注释：把包直接写进文件，没有检查 fwrite() 的返回值（patch 0060）。
         * MP2 的每个包自带帧头，连起来就是能播放的文件。AAC 的包不带帧头，不能这样写（见 encode_wav.c 的 ADTS 头）。
         */
        fwrite(pkt->data, 1, pkt->size, output);
        av_packet_unref(pkt);
    }
}

int main(int argc, char **argv)
{
    const char *filename;
    const AVCodec *codec;
    AVCodecContext *c= NULL;
    AVFrame *frame;
    AVPacket *pkt;
    int i, j, k, ret;
    FILE *f;
    /*
     * 课程注释：采样是有符号的 16 位整数，这里却用了无符号的 uint16_t（patch 0058 改成 int16_t）。
     * 下面的 t 是正弦波的相位，用 float 累加二十多万次，后半段精度不够，音高会漂（patch 0057 改成 double）。
     */
    uint16_t *samples;
    float t, tincr;

    if (argc <= 1) {
        fprintf(stderr, "Usage: %s <output file>\n", argv[0]);
        return 0;
    }
    filename = argv[1];

    /* find the MP2 encoder */
    /* 课程注释：这里按格式的 ID 找编码器，得到的是这种格式的默认编码器。L13 是按名字找的，两种方式都可以。 */
    codec = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!codec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    c = avcodec_alloc_context3(codec);
    if (!c) {
        fprintf(stderr, "Could not allocate audio codec context\n");
        exit(1);
    }

    /* put sample parameters */
    c->bit_rate = 64000;

    /* check that the encoder supports s16 pcm input */
    /*
     * 课程注释：采样格式。MP2 编码器正好接受 s16，所以这个示例不需要转换。
     * 换成 AAC 编码器（只接受 fltp）这里就会失败，要先用 L10 的 libswresample 转换，encode_wav.c 就是这样做的。
     */
    c->sample_fmt = AV_SAMPLE_FMT_S16;
    if (!check_sample_fmt(codec, c->sample_fmt)) {
        fprintf(stderr, "Encoder does not support sample format %s",
                av_get_sample_fmt_name(c->sample_fmt));
        exit(1);
    }

    /* select other audio parameters supported by the encoder */
    c->sample_rate    = select_sample_rate(codec);
    ret = select_channel_layout(codec, &c->ch_layout);
    if (ret < 0)
        exit(1);

    /* open it */
    /* 课程注释：没有设置 time_base。音频编码器不设时默认是 1/采样率。 */
    if (avcodec_open2(c, codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    f = fopen(filename, "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", filename);
        exit(1);
    }

    /* packet for holding encoded output */
    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "could not allocate the packet\n");
        exit(1);
    }

    /* frame containing input raw audio */
    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate audio frame\n");
        exit(1);
    }

    /*
     * 课程注释：frame_size 是打开编码器之后才有的值：这个编码器要求每一帧有多少个采样（MP2 是 1152）。
     * 除了最后一帧，送进去的每一帧都必须正好是这么多。
     * 这个示例的声音是自己算出来的，要多少算多少，所以直接按 frame_size 分配帧就行了。
     * 真实程序里，声音来自解码器或者重采样器，每次的数量不固定，中间要加一个缓冲队列（encode_wav.c）。
     */
    frame->nb_samples     = c->frame_size;
    frame->format         = c->sample_fmt;
    ret = av_channel_layout_copy(&frame->ch_layout, &c->ch_layout);
    if (ret < 0)
        exit(1);

    /* allocate the data buffers */
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate audio data buffers\n");
        exit(1);
    }

    /* encode a single tone sound */
    t = 0;
    tincr = 2 * M_PI * 440.0 / c->sample_rate;
    for (i = 0; i < 200; i++) {
        /* make sure the frame is writable -- makes a copy if the encoder
         * kept a reference internally */
        /* 课程注释：和 L13 一样，反复使用同一个 AVFrame 时必须先确认缓冲区可以写。 */
        ret = av_frame_make_writable(frame);
        if (ret < 0)
            exit(1);
        /*
         * 课程注释：s16 是交错格式，所有声道的采样都在 data[0] 里，按“左、右、左、右”排列（L06）。
         * 下面的两层循环给每个声道写同一个值。
         */
        samples = (uint16_t*)frame->data[0];

        for (j = 0; j < c->frame_size; j++) {
            samples[c->ch_layout.nb_channels*j] = (int)(sin(t) * 10000);

            for (k = 1; k < c->ch_layout.nb_channels; k++)
                samples[c->ch_layout.nb_channels*j + k] = samples[c->ch_layout.nb_channels*j];
            t += tincr;
        }
        /*
         * 课程注释：帧没有设置 pts（patch 0061）。编码器照样工作，但是返回的包也就没有时间戳。
         * 这里直接写裸流看不出问题，要把包交给复用器时就不行了。
         */
        encode(c, frame, pkt, f);
    }

    /* 课程注释：冲刷编码器。下面的 fclose() 没有检查结果（patch 0060）。 */
    /* flush the encoder */
    encode(c, NULL, pkt, f);

    fclose(f);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&c);

    return 0;
}
