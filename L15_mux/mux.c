/*
 * Copyright (c) 2003 Fabrice Bellard
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
 * @file libavformat muxing API usage example
 * @example mux.c
 *
 * Generate a synthetic audio and video signal and mux them to a media file in
 * any supported libavformat format. The default codecs are used.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/mux.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它用程序算出一段画面（渐变的色块）和一段声音（频率不断升高的正弦波），各自编码，
 * 再用 libavformat 写进一个媒体文件。输出格式由文件扩展名决定，两路流都用这种格式的默认编码器。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0062 到 0070，L22 又补了一个 0094），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 mux_av.c。
 *
 * 用法：./mux 输出文件 [-flags 值] [-fflags 值]
 * 例子：./mux out/official.mp4
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <libavutil/avassert.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>

/* 课程注释：生成 10 秒的内容，每秒 25 帧。 */
#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

/* 课程注释：旧式的 libswscale 标志，和下面的 sws_getContext() 配套（patch 0068 改用新接口）。 */
#define SCALE_FLAGS SWS_BICUBIC

/*
 * 课程注释：输出文件里的每一路流（视频一路、音频一路）都用一个 OutputStream 管理：
 * 流（st）、编码器（enc）、下一帧的 pts、要重复使用的帧和包，以及各自的转换器。
 */
// a wrapper around a single output AVStream
typedef struct OutputStream {
    AVStream *st;
    AVCodecContext *enc;

    /* pts of the next frame that will be generated */
    int64_t next_pts;
    int samples_count;

    AVFrame *frame;
    AVFrame *tmp_frame;

    AVPacket *tmp_pkt;

    /* 课程注释：正弦波的相位和增量，用 float 累加 44 万次，误差越来越大（patch 0069 改成 double）。 */
    float t, tincr, tincr2;

    struct SwsContext *sws_ctx;
    struct SwrContext *swr_ctx;
} OutputStream;

/*
 * 课程注释：打印一个包的时间戳。注意它用的是“流”的时间基（文件里的），不是编码器的。
 * 同一个包在两个时间基下的数字不同，但表示同一个时刻。
 */
static void log_packet(const AVFormatContext *fmt_ctx, const AVPacket *pkt)
{
    AVRational *time_base = &fmt_ctx->streams[pkt->stream_index]->time_base;

    printf("pts:%s pts_time:%s dts:%s dts_time:%s duration:%s duration_time:%s stream_index:%d\n",
           av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, time_base),
           av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, time_base),
           av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, time_base),
           pkt->stream_index);
}

/*
 * 课程注释：编码一帧，把编码好的包写进文件。音频和视频共用这个函数。
 * 前半部分和 L13、L14 一样（send_frame、receive_packet），后半部分是这一讲的内容：
 * 换算时间戳，告诉复用器包属于哪一路流，交给复用器。
 * 出错时直接 exit(1)，示例这样写省事；自己的程序应该返回错误码并释放资源。
 */
static int write_frame(AVFormatContext *fmt_ctx, AVCodecContext *c,
                       AVStream *st, AVFrame *frame, AVPacket *pkt)
{
    int ret;

    // send the frame to the encoder
    ret = avcodec_send_frame(c, frame);
    if (ret < 0) {
        fprintf(stderr, "Error sending a frame to the encoder: %s\n",
                av_err2str(ret));
        exit(1);
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(c, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        else if (ret < 0) {
            fprintf(stderr, "Error encoding a frame: %s\n", av_err2str(ret));
            exit(1);
        }

        /* rescale output packet timestamp values from codec to stream timebase */
        /*
         * 课程注释：编码器输出的包，时间戳以编码器的时间基为单位（视频是 1/25）。
         * 文件里这一路流有自己的时间基（MP4 的视频是 1/12800，MKV 是 1/1000，TS 是 1/90000），
         * 而且要到 avformat_write_header() 之后才确定。所以每个包写之前都要换算一次：
         * pts、dts、duration 一起换算。漏掉这一步，时间戳的数字不变、单位变了，播放速度就会不对。
         */
        av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;

        /* Write the compressed frame to the media file. */
        log_packet(fmt_ctx, pkt);
        /*
         * 课程注释：把包交给复用器。有两个函数可选：
         * av_write_frame()：收到就写，包的顺序由调用者保证；
         * av_interleaved_write_frame()：复用器内部先缓存，按 dts 排好顺序再写，让同一时刻的画面和声音在文件里挨在一起。
         * 音频和视频一起写时用后者。它会接管包里的数据，返回以后 pkt 已经是空的。
         */
        ret = av_interleaved_write_frame(fmt_ctx, pkt);
        /* pkt is now blank (av_interleaved_write_frame() takes ownership of
         * its contents and resets pkt), so that no unreferencing is necessary.
         * This would be different if one used av_write_frame(). */
        if (ret < 0) {
            fprintf(stderr, "Error while writing output packet: %s\n", av_err2str(ret));
            exit(1);
        }
    }

    /* 课程注释：返回 1 表示编码器已经冲刷完，主循环据此知道这一路结束了。 */
    return ret == AVERROR_EOF ? 1 : 0;
}

/*
 * 课程注释：创建一路输出流。次序是：找编码器 → avformat_new_stream() 在文件里加一路流 →
 * 分配编码器上下文并设置参数。这时编码器还没有打开，要等所有参数设好了再 avcodec_open2()（见 open_video、open_audio）。
 */
/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,
                       const AVCodec **codec,
                       enum AVCodecID codec_id)
{
    AVCodecContext *c;
    int ret;

    /* find the encoder */
    *codec = avcodec_find_encoder(codec_id);
    if (!(*codec)) {
        fprintf(stderr, "Could not find encoder for '%s'\n",
                avcodec_get_name(codec_id));
        exit(1);
    }

    ost->tmp_pkt = av_packet_alloc();
    if (!ost->tmp_pkt) {
        fprintf(stderr, "Could not allocate AVPacket\n");
        exit(1);
    }

    ost->st = avformat_new_stream(oc, NULL);
    if (!ost->st) {
        fprintf(stderr, "Could not allocate stream\n");
        exit(1);
    }
    ost->st->id = oc->nb_streams-1;
    c = avcodec_alloc_context3(*codec);
    if (!c) {
        fprintf(stderr, "Could not alloc an encoding context\n");
        exit(1);
    }
    ost->enc = c;

    switch ((*codec)->type) {
    case AVMEDIA_TYPE_AUDIO: {
        const void *codec_config;
        /* 课程注释：音频的参数：码率、采样格式、采样率、声道布局，和 L14 一样。 */
        c->bit_rate    = 64000;
        ret = avcodec_get_supported_config(c, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                           0, &codec_config, NULL);
        if (ret < 0) {
            fprintf(stderr, "Failed to get supported sample formats\n");
            exit(1);
        }
        c->sample_fmt  = codec_config ? *(const enum AVSampleFormat*)codec_config
                                      : AV_SAMPLE_FMT_FLTP;
        ret = avcodec_get_supported_config(c, NULL, AV_CODEC_CONFIG_SAMPLE_RATE,
                                           0, &codec_config, NULL);
        if (ret < 0) {
            fprintf(stderr, "Failed to get supported sample rates\n");
            exit(1);
        }
        if (codec_config) {
            const int *supported_samplerates = codec_config;
            c->sample_rate = supported_samplerates[0];
            for (; *supported_samplerates; supported_samplerates++) {
                if (*supported_samplerates == 44100)
                    c->sample_rate = 44100;
            }
        } else {
            c->sample_rate = 44100;
        }
        av_channel_layout_copy(&c->ch_layout, &(AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO);
        /*
         * 课程注释：给流设置时间基。这只是向复用器“建议”，复用器可以改：
         * avformat_write_header() 返回以后要以 st->time_base 当前的值为准。
         */
        ost->st->time_base = (AVRational){ 1, c->sample_rate };
        break;
    }

    case AVMEDIA_TYPE_VIDEO:
        c->codec_id = codec_id;

        c->bit_rate = 400000;
        /* Resolution must be a multiple of two. */
        c->width    = 352;
        c->height   = 288;
        /* timebase: This is the fundamental unit of time (in seconds) in terms
         * of which frame timestamps are represented. For fixed-fps content,
         * timebase should be 1/framerate and timestamp increments should be
         * identical to 1. */
        ost->st->time_base = (AVRational){ 1, STREAM_FRAME_RATE };
        c->time_base       = ost->st->time_base;

        /*
         * 课程注释：视频帧在 get_video_frame() 里没有设置时长，也没有设 AV_CODEC_FLAG_FRAME_DURATION，
         * 编出来的包时长是 0，MP4 的最后一帧因此被丢掉（patch 0065）。
         */
        c->gop_size      = 12; /* emit one intra frame every twelve frames at most */
        /*
         * 课程注释：像素格式写死为 yuv420p，没有问编码器支持什么。gif、apng 这些格式的默认编码器不接受它，
         * 下面 get_video_frame() 里“转换像素格式”的分支也因此永远走不到（patch 0066）。
         */
        c->pix_fmt       = STREAM_PIX_FMT;
        if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO) {
            /* just for testing, we also add B-frames */
            c->max_b_frames = 2;
        }
        if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO) {
            /* Needed to avoid using macroblocks in which some coeffs overflow.
             * This does not happen with normal video, it just happens here as
             * the motion of the chroma plane does not match the luma plane. */
            /*
             * 课程注释：这几个 if 是针对个别编码器的参数。image2 复用器（文件名带 %d 输出图片序列）的默认编码器是 MJPEG，
             * 它只接受全范围的 YUV，这里没有处理，打不开编码器（patch 0067）。
             */
            c->mb_decision = 2;
        }
        break;

    default:
        break;
    }

    /*
     * 课程注释：全局头（global header）。有的格式（MP4、MKV）把解码需要的参数集（H.264 的 SPS、PPS，AAC 的配置）
     * 放在文件头里一份，包里不再重复；有的格式（MPEG-TS、裸 H.264）放在码流里，每个关键帧前面带一份。
     * 复用器用 AVFMT_GLOBALHEADER 说明自己属于哪一种。属于前一种时，必须在 avcodec_open2() 之前给编码器设
     * AV_CODEC_FLAG_GLOBAL_HEADER，编码器才会把参数集放进 extradata 而不是码流里。
     * 下面 avcodec_parameters_from_context() 会把 extradata 一起复制给流，复用器靠它写文件头。
     */
    /* Some formats want stream headers to be separate. */
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}

/**************************************************************/
/* audio output */

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt,
                                  const AVChannelLayout *channel_layout,
                                  int sample_rate, int nb_samples)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error allocating an audio frame\n");
        exit(1);
    }

    frame->format = sample_fmt;
    av_channel_layout_copy(&frame->ch_layout, channel_layout);
    frame->sample_rate = sample_rate;
    frame->nb_samples = nb_samples;

    if (nb_samples) {
        if (av_frame_get_buffer(frame, 0) < 0) {
            fprintf(stderr, "Error allocating an audio buffer\n");
            exit(1);
        }
    }

    return frame;
}

/* 课程注释：打开音频编码器，分配帧，建立重采样器。 */
static void open_audio(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg)
{
    AVCodecContext *c;
    int nb_samples;
    int ret;
    AVDictionary *opt = NULL;

    c = ost->enc;

    /* open it */
    av_dict_copy(&opt, opt_arg, 0);
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open audio codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* init signal generator */
    ost->t     = 0;
    /* 课程注释：信号发生器：频率从 110 Hz 起，每秒升高 110 Hz。 */
    ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
    /* increment frequency by 110 Hz per second */
    ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

    /*
     * 课程注释：每帧的采样数。有的编码器（如 PCM）不限制帧长，这里取 10000；其余的按 frame_size 来，
     * 这是 L14 的内容。示例的声音是自己算的，要多少算多少，所以不需要缓冲队列。
     */
    if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        nb_samples = 10000;
    else
        nb_samples = c->frame_size;

    ost->frame     = alloc_audio_frame(c->sample_fmt, &c->ch_layout,
                                       c->sample_rate, nb_samples);
    ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, &c->ch_layout,
                                       c->sample_rate, nb_samples);

    /* copy the stream parameters to the muxer */
    /*
     * 课程注释：把编码器的参数复制给文件里的流（codecpar）：编码格式、宽高、采样率、extradata 等。
     * 复用器写文件头时读的是 codecpar，不读编码器上下文。必须在编码器打开之后做，因为 extradata 是打开时才生成的。
     */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }

    /* create resampler context */
    ost->swr_ctx = swr_alloc();
    if (!ost->swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        exit(1);
    }

    /* set options */
    av_opt_set_chlayout  (ost->swr_ctx, "in_chlayout",       &c->ch_layout,      0);
    av_opt_set_int       (ost->swr_ctx, "in_sample_rate",     c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt",      AV_SAMPLE_FMT_S16, 0);
    av_opt_set_chlayout  (ost->swr_ctx, "out_chlayout",      &c->ch_layout,      0);
    av_opt_set_int       (ost->swr_ctx, "out_sample_rate",    c->sample_rate,    0);
    av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt",     c->sample_fmt,     0);

    /* initialize the resampling context */
    if ((ret = swr_init(ost->swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        exit(1);
    }
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
 * 'nb_channels' channels. */
static AVFrame *get_audio_frame(OutputStream *ost)
{
    AVFrame *frame = ost->tmp_frame;
    int j, i, v;
    int16_t *q = (int16_t*)frame->data[0];

    /* check if we want to generate more frames */
    if (av_compare_ts(ost->next_pts, ost->enc->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
        return NULL;

    for (j = 0; j <frame->nb_samples; j++) {
        v = (int)(sin(ost->t) * 10000);
        for (i = 0; i < ost->enc->ch_layout.nb_channels; i++)
            *q++ = v;
        ost->t     += ost->tincr;
        ost->tincr += ost->tincr2;
    }

    frame->pts = ost->next_pts;
    ost->next_pts  += frame->nb_samples;

    return frame;
}

/*
 * encode one audio frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
    AVCodecContext *c;
    AVFrame *frame;
    int ret;
    int dst_nb_samples;

    c = ost->enc;

    frame = get_audio_frame(ost);

    if (frame) {
        /* convert samples from native format to destination codec format, using the resampler */
        /* compute destination number of samples */
        /*
         * 课程注释：这里的重采样器输入和输出采样率相同，只转换采样格式（s16 → 编码器要的格式），
         * 所以输出的采样数正好等于输入的，下面 av_assert0() 用来确认这一点。
         */
        dst_nb_samples = swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples;
        av_assert0(dst_nb_samples == frame->nb_samples);

        /* when we pass a frame to the encoder, it may keep a reference to it
         * internally;
         * make sure we do not overwrite it here
         */
        ret = av_frame_make_writable(ost->frame);
        if (ret < 0)
            exit(1);

        /* convert to destination format */
        ret = swr_convert(ost->swr_ctx,
                          ost->frame->data, dst_nb_samples,
                          (const uint8_t **)frame->data, frame->nb_samples);
        if (ret < 0) {
            fprintf(stderr, "Error while converting\n");
            exit(1);
        }
        frame = ost->frame;

        /* 课程注释：音频帧的 pts：已经送进编码器的采样数，单位换算成编码器的时间基（L14）。 */
        frame->pts = av_rescale_q(ost->samples_count, (AVRational){1, c->sample_rate}, c->time_base);
        ost->samples_count += dst_nb_samples;
    }

    return write_frame(oc, c, ost->st, frame, ost->tmp_pkt);
}

/**************************************************************/
/* video output */

static AVFrame *alloc_frame(enum AVPixelFormat pix_fmt, int width, int height)
{
    AVFrame *frame;
    int ret;

    frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = pix_fmt;
    frame->width  = width;
    frame->height = height;

    /* allocate the buffers for the frame data */
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate frame data.\n");
        exit(1);
    }

    return frame;
}

static void open_video(AVFormatContext *oc, const AVCodec *codec,
                       OutputStream *ost, AVDictionary *opt_arg)
{
    int ret;
    AVCodecContext *c = ost->enc;
    AVDictionary *opt = NULL;

    av_dict_copy(&opt, opt_arg, 0);

    /* open the codec */
    ret = avcodec_open2(c, codec, &opt);
    av_dict_free(&opt);
    if (ret < 0) {
        fprintf(stderr, "Could not open video codec: %s\n", av_err2str(ret));
        exit(1);
    }

    /* allocate and init a reusable frame */
    ost->frame = alloc_frame(c->pix_fmt, c->width, c->height);
    if (!ost->frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }

    /* If the output format is not YUV420P, then a temporary YUV420P
     * picture is needed too. It is then converted to the required
     * output format. */
    ost->tmp_frame = NULL;
    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        ost->tmp_frame = alloc_frame(AV_PIX_FMT_YUV420P, c->width, c->height);
        if (!ost->tmp_frame) {
            fprintf(stderr, "Could not allocate temporary video frame\n");
            exit(1);
        }
    }

    /* copy the stream parameters to the muxer */
    ret = avcodec_parameters_from_context(ost->st->codecpar, c);
    if (ret < 0) {
        fprintf(stderr, "Could not copy the stream parameters\n");
        exit(1);
    }
}

/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    int x, y, i;

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->enc;

    /* check if we want to generate more frames */
    /*
     * 课程注释：判断是否到了 10 秒。时间戳等于 10 秒的那一帧也会通过 > 0 的判断，
     * 所以视频多生成了一帧：251 帧、10.04 秒（patch 0064 改成 >= 0）。
     */
    if (av_compare_ts(ost->next_pts, c->time_base,
                      STREAM_DURATION, (AVRational){ 1, 1 }) > 0)
        return NULL;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally; make sure we do not overwrite it here */
    if (av_frame_make_writable(ost->frame) < 0)
        exit(1);

    if (c->pix_fmt != AV_PIX_FMT_YUV420P) {
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        /* 课程注释：旧式的 sws_getContext() 加 sws_scale()，并且不检查转换结果（patch 0068）。 */
        if (!ost->sws_ctx) {
            ost->sws_ctx = sws_getContext(c->width, c->height,
                                          AV_PIX_FMT_YUV420P,
                                          c->width, c->height,
                                          c->pix_fmt,
                                          SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->sws_ctx) {
                fprintf(stderr,
                        "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->sws_ctx, (const uint8_t * const *) ost->tmp_frame->data,
                  ost->tmp_frame->linesize, 0, c->height, ost->frame->data,
                  ost->frame->linesize);
    } else {
        fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
    }

    /* 课程注释：视频帧的 pts 就是帧序号，因为编码器时间基是 1/25。 */
    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}

/*
 * encode one video frame and send it to the muxer
 * return 1 when encoding is finished, 0 otherwise
 */
static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
    return write_frame(oc, ost->enc, ost->st, get_video_frame(ost), ost->tmp_pkt);
}

/* 课程注释：释放一路流用到的东西。 */
static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
    avcodec_free_context(&ost->enc);
    av_frame_free(&ost->frame);
    av_frame_free(&ost->tmp_frame);
    av_packet_free(&ost->tmp_pkt);
    sws_freeContext(ost->sws_ctx);
    swr_free(&ost->swr_ctx);
}

/**************************************************************/
/* media file output */

/*
 * 课程注释：主流程。复用的固定套路，按次序是：
 *   1. avformat_alloc_output_context2()   创建输出上下文，格式由文件名猜
 *   2. avformat_new_stream() 等             加流，打开编码器，复制参数（add_stream、open_video、open_audio）
 *   3. avio_open()                          打开输出文件
 *   4. avformat_write_header()              写文件头，确定每路流的时间基
 *   5. 循环：编码 → av_interleaved_write_frame()
 *   6. av_write_trailer()                   写文件尾（MP4 在这里写索引）
 *   7. avio_closep()、avformat_free_context() 关闭、释放
 */
int main(int argc, char **argv)
{
    OutputStream video_st = { 0 }, audio_st = { 0 };
    const AVOutputFormat *fmt;
    const char *filename;
    AVFormatContext *oc;
    const AVCodec *audio_codec, *video_codec;
    int ret;
    int have_video = 0, have_audio = 0;
    int encode_video = 0, encode_audio = 0;
    AVDictionary *opt = NULL;
    int i;

    if (argc < 2) {
        /* 课程注释：用法说明里没有提 -flags 和 -fflags 两个参数（patch 0070）。 */
        printf("usage: %s output_file\n"
               "API example program to output a media file with libavformat.\n"
               "This program generates a synthetic audio and video stream, encodes and\n"
               "muxes them into a file named output_file.\n"
               "The output format is automatically guessed according to the file extension.\n"
               "Raw images can also be output by using '%%d' in the filename.\n"
               "\n", argv[0]);
        return 1;
    }

    filename = argv[1];
    for (i = 2; i+1 < argc; i+=2) {
        if (!strcmp(argv[i], "-flags") || !strcmp(argv[i], "-fflags"))
            /* 课程注释：把 -flags、-fflags 的值放进字典，后面传给编码器和复用器。这个字典用完没有释放（patch 0063）。 */
            av_dict_set(&opt, argv[i]+1, argv[i+1], 0);
    }

    /* allocate the output media context */
    /* 课程注释：按文件扩展名选输出格式。猜不出来时（返回 NULL）下面改用 MPEG。 */
    avformat_alloc_output_context2(&oc, NULL, NULL, filename);
    if (!oc) {
        printf("Could not deduce output format from file extension: using MPEG.\n");
        avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
    }
    if (!oc)
        return 1;

    /* 课程注释：oformat 描述这种输出格式，里面有它默认的视频、音频编码器和各种标志。 */
    fmt = oc->oformat;

    /* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    /* 课程注释：用这种格式默认的编码器。MP4 是 H.264 和 AAC，MKV 是 H.264 和 AC-3（或 Vorbis），TS 是 MPEG-2 和 MP2。 */
    if (fmt->video_codec != AV_CODEC_ID_NONE) {
        add_stream(&video_st, oc, &video_codec, fmt->video_codec);
        have_video = 1;
        encode_video = 1;
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE) {
        add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec);
        have_audio = 1;
        encode_audio = 1;
    }

    /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (have_video)
        open_video(oc, video_codec, &video_st, opt);

    if (have_audio)
        open_audio(oc, audio_codec, &audio_st, opt);

    /* 课程注释：打印输出文件的流信息，最后一个参数 1 表示这是输出。 */
    av_dump_format(oc, 0, filename, 1);

    /* open the output file, if needed */
    if (!(fmt->flags & AVFMT_NOFILE)) {
        /* 课程注释：打开输出文件。有的格式（例如图片序列）自己管文件，带 AVFMT_NOFILE 标志，不用调用这个。 */
        ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "Could not open '%s': %s\n", filename,
                    av_err2str(ret));
            /*
             * 课程注释：patch 0094。这里直接 return，格式上下文、两个编码器上下文、帧、缩放器、重采样器
             * 都没有释放（正常结束的路径是释放了的）。L22 的 ASan 扫描用不存在的输出目录就能复现：
             * ./mux /nonexistent/dir/out.mp4
             */
            return 1;
        }
    }

    /* Write the stream header, if any. */
    /*
     * 课程注释：写文件头。所有流都建好、参数都设好以后才能调用。
     * 它做三件事：检查每路流的参数是否完整，写文件头，确定每路流最终的时间基。
     * 复用器的选项（如 movflags）通过第二个参数传入；返回后字典里剩下的是复用器不认识的，示例没有检查。
     */
    ret = avformat_write_header(oc, &opt);
    if (ret < 0) {
        fprintf(stderr, "Error occurred when opening output file: %s\n",
                av_err2str(ret));
        /* 课程注释：patch 0094，同上，这条出错路径也泄漏所有资源。 */
        return 1;
    }

    /*
     * 课程注释：主循环：每次让“时间落后”的那一路往前走一步。
     * av_compare_ts() 可以比较两个不同时间基下的时间戳，这样两路的包大致按时间顺序产生。
     */
    while (encode_video || encode_audio) {
        /* select the stream to encode */
        if (encode_video &&
            (!encode_audio || av_compare_ts(video_st.next_pts, video_st.enc->time_base,
                                            audio_st.next_pts, audio_st.enc->time_base) <= 0)) {
            encode_video = !write_video_frame(oc, &video_st);
        } else {
            encode_audio = !write_audio_frame(oc, &audio_st);
        }
    }

    /*
     * 课程注释：写文件尾。MP4 的索引（moov）是在这里写的，记录每个包在文件里的位置和时间；
     * 没有它，MP4 是打不开的。返回值没有检查：磁盘满了这一步写不进去，程序照样返回 0（patch 0062）。
     */
    av_write_trailer(oc);

    /* Close each codec. */
    if (have_video)
        close_stream(oc, &video_st);
    if (have_audio)
        close_stream(oc, &audio_st);

    if (!(fmt->flags & AVFMT_NOFILE))
        /* Close the output file. */
        /* 课程注释：关闭输出文件，缓冲区里还没写出去的数据在这里写出。返回值同样没有检查（patch 0062）。 */
        avio_closep(&oc->pb);

    /* free the stream */
    avformat_free_context(oc);

    return 0;
}
