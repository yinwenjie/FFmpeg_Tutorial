/*
 * copyright (c) 2013 Andrew Kelley
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file libavfilter audio filtering API usage example
 * @example filter_audio.c
 *
 * This example will generate a sine wave audio, pass it through a simple filter
 * chain, and then compute the MD5 checksum of the output data.
 *
 * The filter chain it uses is:
 * (input) -> abuffer -> volume -> aformat -> abuffersink -> (output)
 *
 * abuffer: This provides the endpoint where you can feed the decoded samples.
 * volume: In this example we hardcode it to 0.90.
 * aformat: This converts the samples to the samplefreq, channel layout,
 *          and sample format required by the audio device.
 * abuffersink: This provides the endpoint where you can read the samples after
 *              they have passed through the filter chain.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/filter_audio.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它自己生成一段 5.0 声道、48000 Hz、fltp 格式的正弦波，送进一个手工搭起来的滤镜图：
 *   abuffer（入口）-> volume（音量乘 0.9）-> aformat（要求 s16、44100 Hz、立体声）-> abuffersink（出口）
 * 再把出口拿到的每一帧算出 MD5 打印出来。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0042 到 0047），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 volume_file.c。
 */

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavutil/channel_layout.h>
#include <libavutil/md5.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>

#define INPUT_SAMPLERATE     48000
#define INPUT_FORMAT         AV_SAMPLE_FMT_FLTP
#define INPUT_CHANNEL_LAYOUT (AVChannelLayout)AV_CHANNEL_LAYOUT_5POINT0

#define VOLUME_VAL 0.90

static int init_filter_graph(AVFilterGraph **graph, AVFilterContext **src,
                             AVFilterContext **sink)
{
    AVFilterGraph *filter_graph;
    AVFilterContext *abuffer_ctx;
    const AVFilter  *abuffer;
    AVFilterContext *volume_ctx;
    const AVFilter  *volume;
    AVFilterContext *aformat_ctx;
    const AVFilter  *aformat;
    AVFilterContext *abuffersink_ctx;
    const AVFilter  *abuffersink;

    AVDictionary *options_dict = NULL;
    /*
     * 课程注释：这两个缓冲区放的是字符串，应该用 char。用 -Wall 编译时这里引出 4 个 -Wpointer-sign 警告（patch 0042），
     * 本目录的 Makefile 为这个文件单独关掉了这个警告。
     */
    uint8_t options_str[1024];
    uint8_t ch_layout[64];

    int err;

    /* Create a new filtergraph, which will contain all the filters. */
    /* 课程注释：第 1 步，分配滤镜图 AVFilterGraph。图是容器：所有滤镜实例都建在图里，释放图的时候一起释放。 */
    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        fprintf(stderr, "Unable to create filter graph.\n");
        return AVERROR(ENOMEM);
    }

    /* Create the abuffer filter;
     * it will be used for feeding the data into the graph. */
    /*
     * 课程注释：第 2 步，建第一个滤镜 abuffer，它是图的入口：程序把帧交给它，它再往下游传。
     * 每个滤镜都分两个层次，和“编解码器 AVCodec / 编解码器上下文 AVCodecContext”的关系一样：
     *   AVFilter         滤镜的定义，全局只有一份，按名字查找
     *   AVFilterContext  滤镜的一个实例，带着自己的参数，属于某一张图
     * avfilter_get_by_name() 找定义，avfilter_graph_alloc_filter() 在图里建一个实例，第三个参数是实例的名字（自己起，调试时看得到）。
     * 从这里开始，所有出错的地方都直接 return，没有释放已经分配的图（patch 0046）。
     */
    abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
        fprintf(stderr, "Could not find the abuffer filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, "src");
    if (!abuffer_ctx) {
        fprintf(stderr, "Could not allocate the abuffer instance.\n");
        return AVERROR(ENOMEM);
    }

    /* Set the filter options through the AVOptions API. */
    /*
     * 课程注释：第 3 步，设置参数。abuffer 必须知道送进来的帧是什么样的：声道布局、采样格式、采样率、时间基。
     * 这是传参数的第一种方式：用 av_opt_set_* 逐个设置。
     * AV_OPT_SEARCH_CHILDREN 是必须的：选项不在 AVFilterContext 本身，而在它里面这个滤镜的私有数据上，要让查找深入一层。
     */
    av_channel_layout_describe(&INPUT_CHANNEL_LAYOUT, ch_layout, sizeof(ch_layout));
    av_opt_set    (abuffer_ctx, "channel_layout", ch_layout,                            AV_OPT_SEARCH_CHILDREN);
    av_opt_set    (abuffer_ctx, "sample_fmt",     av_get_sample_fmt_name(INPUT_FORMAT), AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q  (abuffer_ctx, "time_base",      (AVRational){ 1, INPUT_SAMPLERATE },  AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(abuffer_ctx, "sample_rate",    INPUT_SAMPLERATE,                     AV_OPT_SEARCH_CHILDREN);

    /* Now initialize the filter; we pass NULL options, since we have already
     * set all the options above. */
    /*
     * 课程注释：第 4 步，初始化这个实例。参数已经在上面设好了，所以这里的选项字符串传 NULL。
     * 每个滤镜实例都要经过“建实例、给参数、初始化”三件事，下面三个滤镜重复同样的过程。
     */
    err = avfilter_init_str(abuffer_ctx, NULL);
    if (err < 0) {
        fprintf(stderr, "Could not initialize the abuffer filter.\n");
        return err;
    }

    /* Create volume filter. */
    volume = avfilter_get_by_name("volume");
    if (!volume) {
        fprintf(stderr, "Could not find the volume filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    volume_ctx = avfilter_graph_alloc_filter(filter_graph, volume, "volume");
    if (!volume_ctx) {
        fprintf(stderr, "Could not allocate the volume instance.\n");
        return AVERROR(ENOMEM);
    }

    /* A different way of passing the options is as key/value pairs in a
     * dictionary. */
    /*
     * 课程注释：传参数的第二种方式：放进 AVDictionary（键值对），初始化时一起传进去。
     * AV_STRINGIFY(VOLUME_VAL) 把宏的值变成字符串 "0.90"。
     */
    av_dict_set(&options_dict, "volume", AV_STRINGIFY(VOLUME_VAL), 0);
    err = avfilter_init_dict(volume_ctx, &options_dict);
    av_dict_free(&options_dict);
    if (err < 0) {
        fprintf(stderr, "Could not initialize the volume filter.\n");
        return err;
    }

    /* Create the aformat filter;
     * it ensures that the output is of the format we want. */
    aformat = avfilter_get_by_name("aformat");
    if (!aformat) {
        fprintf(stderr, "Could not find the aformat filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    aformat_ctx = avfilter_graph_alloc_filter(filter_graph, aformat, "aformat");
    if (!aformat_ctx) {
        fprintf(stderr, "Could not allocate the aformat instance.\n");
        return AVERROR(ENOMEM);
    }

    /* A third way of passing the options is in a string of the form
     * key1=value1:key2=value2.... */
    /*
     * 课程注释：传参数的第三种方式，也是最常用的：一个 "键=值:键=值" 形式的字符串，和 ffmpeg 命令行里滤镜的写法相同。
     * aformat 自己不改数据，它只声明“从我这里出去的必须是这些格式”。输入是 fltp、48000 Hz、5.0 声道，
     * 和这里要求的三样都不同，真正的转换由 libavfilter 自动插入的重采样滤镜完成（见下面 avfilter_graph_config 处的注释）。
     */
    snprintf(options_str, sizeof(options_str),
             "sample_fmts=%s:sample_rates=%d:channel_layouts=stereo",
             av_get_sample_fmt_name(AV_SAMPLE_FMT_S16), 44100);
    err = avfilter_init_str(aformat_ctx, options_str);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not initialize the aformat filter.\n");
        return err;
    }

    /* Finally create the abuffersink filter;
     * it will be used to get the filtered data out of the graph. */
    /* 课程注释：最后一个滤镜 abuffersink，它是图的出口：程序从它那里取处理好的帧。 */
    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        fprintf(stderr, "Could not find the abuffersink filter.\n");
        return AVERROR_FILTER_NOT_FOUND;
    }

    abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx) {
        fprintf(stderr, "Could not allocate the abuffersink instance.\n");
        return AVERROR(ENOMEM);
    }

    /* This filter takes no options. */
    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0) {
        fprintf(stderr, "Could not initialize the abuffersink instance.\n");
        return err;
    }

    /* Connect the filters;
     * in this simple case the filters just form a linear chain. */
    /*
     * 课程注释：第 5 步，连接。avfilter_link(上游滤镜, 上游的第几个输出, 下游滤镜, 下游的第几个输入)。
     * 滤镜的输入口、输出口叫 pad。这四个滤镜都只有一个输入或输出，所以序号都是 0。
     */
    err = avfilter_link(abuffer_ctx, 0, volume_ctx, 0);
    if (err >= 0)
        err = avfilter_link(volume_ctx, 0, aformat_ctx, 0);
    if (err >= 0)
        err = avfilter_link(aformat_ctx, 0, abuffersink_ctx, 0);
    if (err < 0) {
        fprintf(stderr, "Error connecting filters\n");
        return err;
    }

    /* Configure the graph. */
    /*
     * 课程注释：第 6 步，配置整张图。它检查所有 pad 都接上了，然后做“格式协商”：
     * 让每一条连接两端的滤镜就采样格式、采样率、声道布局达成一致。达不成一致的地方，自动插入转换滤镜
     * （音频是 aresample，也就是 L10 的 libswresample；视频是 scale，也就是 L09 的 libswscale）。
     * 这个示例里 volume 和 aformat 之间会被插入一个 aresample。配置之后图就不能再改结构了。
     */
    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
        return err;
    }

    *graph = filter_graph;
    *src   = abuffer_ctx;
    *sink  = abuffersink_ctx;

    return 0;
}

/* Do something useful with the filtered data: this simple
 * example just prints the MD5 checksum of each plane to stdout. */
/*
 * 课程注释：对输出帧算 MD5 并打印。出口给的是 s16（packed），所以只有一个平面。
 * 这里 av_md5_init() 之后用的是 av_md5_sum()，它并不使用传进来的 md5 上下文（patch 0044）。
 */
static int process_output(struct AVMD5 *md5, AVFrame *frame)
{
    int planar     = av_sample_fmt_is_planar(frame->format);
    int channels   = frame->ch_layout.nb_channels;
    int planes     = planar ? channels : 1;
    int bps        = av_get_bytes_per_sample(frame->format);
    int plane_size = bps * frame->nb_samples * (planar ? 1 : channels);
    int i, j;

    for (i = 0; i < planes; i++) {
        uint8_t checksum[16];

        av_md5_init(md5);
        av_md5_sum(checksum, frame->extended_data[i], plane_size);

        fprintf(stdout, "plane %d: 0x", i);
        for (j = 0; j < sizeof(checksum); j++)
            fprintf(stdout, "%02X", checksum[j]);
        fprintf(stdout, "\n");
    }
    fprintf(stdout, "\n");

    return 0;
}

/* Construct a frame of audio data to be filtered;
 * this simple example just synthesizes a sine wave. */
static int get_input(AVFrame *frame, int frame_num)
{
    int err, i, j;

#define FRAME_SIZE 1024

    /* Set up the frame properties and allocate the buffer for the data. */
    frame->sample_rate    = INPUT_SAMPLERATE;
    frame->format         = INPUT_FORMAT;
    av_channel_layout_copy(&frame->ch_layout, &INPUT_CHANNEL_LAYOUT);
    frame->nb_samples     = FRAME_SIZE;
    frame->pts            = frame_num * FRAME_SIZE;

    err = av_frame_get_buffer(frame, 0);
    if (err < 0)
        return err;

    /* Fill the data for each channel. */
    for (i = 0; i < 5; i++) {
        float *data = (float*)frame->extended_data[i];

        for (j = 0; j < frame->nb_samples; j++)
            /*
             * 课程注释：第 i 个声道的频率是第 0 个声道的 i + 1 倍。相位用了 frame_num + j，
             * 结果每到帧的边界波形会跳过一个采样（patch 0043），对演示滤镜的用法没有影响。
             */
            data[j] = sin(2 * M_PI * (frame_num + j) * (i + 1) / FRAME_SIZE);
    }

    return 0;
}

int main(int argc, char *argv[])
{
    struct AVMD5 *md5;
    AVFilterGraph *graph;
    AVFilterContext *src, *sink;
    AVFrame *frame;
    float duration;
    int err, nb_frames, i;

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <duration>\n", argv[0]);
        return 1;
    }

    duration  = atof(argv[1]);
    nb_frames = duration * INPUT_SAMPLERATE / FRAME_SIZE;
    if (nb_frames <= 0) {
        fprintf(stderr, "Invalid duration: %s\n", argv[1]);
        return 1;
    }

    /* Allocate the frame we will be using to store the data. */
    frame  = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Error allocating the frame\n");
        return 1;
    }

    md5 = av_md5_alloc();
    if (!md5) {
        av_frame_free(&frame);
        fprintf(stderr, "Error allocating the MD5 context\n");
        return 1;
    }

    /* Set up the filtergraph. */
    err = init_filter_graph(&graph, &src, &sink);
    if (err < 0) {
        av_frame_free(&frame);
        av_freep(&md5);
        /* 课程注释：这里直接 return 了，没有像其他出错路径那样跳到 fail 去打印错误原因和换行（patch 0045）。 */
        fprintf(stderr, "Unable to init filter graph:");
        return 1;
    }

    /* the main filtering loop */
    for (i = 0; i < nb_frames; i++) {
        /* get an input frame to be filtered */
        err = get_input(frame, i);
        if (err < 0) {
            fprintf(stderr, "Error generating input frame:");
            goto fail;
        }

        /* Send the frame to the input of the filtergraph. */
        /*
         * 课程注释：把一帧送进图的入口。av_buffersrc_add_frame() 会把帧里的数据“拿走”（frame 被清空），
         * 所以下面可以用同一个 frame 去接输出。如果送进去之后自己还要用这一帧，改用 av_buffersrc_add_frame_flags() 加 AV_BUFFERSRC_FLAG_KEEP_REF。
         */
        err = av_buffersrc_add_frame(src, frame);
        if (err < 0) {
            av_frame_unref(frame);
            fprintf(stderr, "Error submitting the frame to the filtergraph:");
            goto fail;
        }

        /* Get all the filtered output that is available. */
        /*
         * 课程注释：从出口取帧，直到取不出来。和解码器的 send/receive 是同一个模式：送一帧进去，可能出来 0 帧、1 帧或多帧。
         * 返回 AVERROR(EAGAIN) 表示要再送输入才有输出；返回 AVERROR_EOF 表示图已经结束，不会再有输出。
         * 取出来的帧用完要 av_frame_unref()。
         */
        while ((err = av_buffersink_get_frame(sink, frame)) >= 0) {
            /* now do something with our filtered frame */
            err = process_output(md5, frame);
            if (err < 0) {
                fprintf(stderr, "Error processing the filtered frame:");
                goto fail;
            }
            av_frame_unref(frame);
        }

        if (err == AVERROR(EAGAIN)) {
            /* Need to feed more frames in. */
            continue;
        } else if (err == AVERROR_EOF) {
            /* Nothing more to do, finish. */
            break;
        } else if (err < 0) {
            /* An error occurred. */
            fprintf(stderr, "Error filtering the data:");
            goto fail;
        }
    }

    /*
     * 课程注释：循环到这里就结束了，没有告诉图“输入结束了”。正确的做法是 av_buffersrc_add_frame(src, NULL)，
     * 然后继续从出口取帧，直到返回 AVERROR_EOF。少了这一步，留在自动插入的重采样滤镜里的最后 16 个采样没有输出（patch 0047）。
     * avfilter_graph_free() 释放图和图里所有的滤镜实例，不需要逐个释放滤镜。
     */
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    av_freep(&md5);

    return 0;

fail:
    avfilter_graph_free(&graph);
    av_frame_free(&frame);
    av_freep(&md5);

    fprintf(stderr, "%s\n", av_err2str(err));
    return 1;
}
