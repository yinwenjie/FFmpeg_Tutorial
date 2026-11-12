/*
 * Copyright (c) 2012 Stefano Sabatini
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
 * @file audio resampling API usage example
 * @example resample_audio.c
 *
 * Generate a synthetic audio signal, and Use libswresample API to perform audio
 * resampling. The output is written to a raw audio file to be played with
 * ffplay.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/resample_audio.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它自己生成 10 秒的 440 Hz 正弦波（48000 Hz、双声道、dbl 格式），
 * 用 libswresample 转成 44100 Hz、3.0 声道（左、右、中）、s16 格式，写进一个裸 PCM 文件。
 * 一次转换同时改了三样：采样率、声道布局、采样格式。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0038 到 0041），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 resample_file.c。
 */

#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

/*
 * 课程注释：这个函数和重采样无关，只是为了最后提示 ffplay 命令：把采样格式换成 ffplay 的 -f 参数认识的名字，
 * 例如 AV_SAMPLE_FMT_S16 在小端机器上是 s16le。L06 的 decode_audio.c 里有同样的函数。
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
            "Sample format %s not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return AVERROR(EINVAL);
}

/**
 * Fill dst buffer with nb_samples, generated starting from t.
 */
/*
 * 课程注释：生成测试信号。dbl 是 packed 格式，各声道交错存放，所以 dstp 每次前进 nb_channels 个采样。
 * t 是当前的时间（秒），每生成一个采样前进 1 / sample_rate 秒，跨多次调用连续累加，这样波形在帧与帧之间是接上的。
 */
static void fill_samples(double *dst, int nb_samples, int nb_channels, int sample_rate, double *t)
{
    int i, j;
    double tincr = 1.0 / sample_rate, *dstp = dst;
    const double c = 2 * M_PI * 440.0;

    /* generate sin tone with 440Hz frequency and duplicated channels */
    for (i = 0; i < nb_samples; i++) {
        *dstp = sin(c * *t);
        for (j = 1; j < nb_channels; j++)
            dstp[j] = dstp[0];
        dstp += nb_channels;
        *t += tincr;
    }
}

int main(int argc, char **argv)
{
    AVChannelLayout src_ch_layout = AV_CHANNEL_LAYOUT_STEREO, dst_ch_layout = AV_CHANNEL_LAYOUT_SURROUND;
    int src_rate = 48000, dst_rate = 44100;
    uint8_t **src_data = NULL, **dst_data = NULL;
    int src_nb_channels = 0, dst_nb_channels = 0;
    int src_linesize, dst_linesize;
    int src_nb_samples = 1024, dst_nb_samples, max_dst_nb_samples;
    enum AVSampleFormat src_sample_fmt = AV_SAMPLE_FMT_DBL, dst_sample_fmt = AV_SAMPLE_FMT_S16;
    const char *dst_filename = NULL;
    FILE *dst_file;
    int dst_bufsize;
    const char *fmt;
    struct SwrContext *swr_ctx;
    char buf[64];
    double t;
    int ret;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s output_file\n"
                "API example program to show how to resample an audio stream with libswresample.\n"
                "This program generates a series of audio frames, resamples them to a specified "
                "output format and rate and saves them to an output file named output_file.\n",
            argv[0]);
        exit(1);
    }
    dst_filename = argv[1];

    dst_file = fopen(dst_filename, "wb");
    if (!dst_file) {
        fprintf(stderr, "Could not open destination file %s\n", dst_filename);
        exit(1);
    }

    /* create resampler context */
    /* 课程注释：第 1 步，分配重采样上下文 SwrContext。这时它还是空的，不知道要做什么转换。 */
    swr_ctx = swr_alloc();
    if (!swr_ctx) {
        fprintf(stderr, "Could not allocate resampler context\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* set options */
    /*
     * 课程注释：第 2 步，告诉它输入是什么、输出要什么。各有三项：声道布局、采样率、采样格式，
     * 用 av_opt_set_* 按名字设置（这是 FFmpeg 通用的选项机制，L13 讲）。
     * 另一种写法是 swr_alloc_set_opts2()，一个函数把分配和这六项参数都办了。
     */
    av_opt_set_chlayout(swr_ctx, "in_chlayout",    &src_ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate",       src_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", src_sample_fmt, 0);

    av_opt_set_chlayout(swr_ctx, "out_chlayout",    &dst_ch_layout, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate",       dst_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", dst_sample_fmt, 0);

    /* initialize the resampling context */
    /*
     * 课程注释：第 3 步，初始化。参数设完必须调用 swr_init()，它按参数准备好内部的滤波器和混音矩阵。
     * 初始化之后再改参数不会生效，要改就得重新 swr_init()。
     */
    if ((ret = swr_init(swr_ctx)) < 0) {
        fprintf(stderr, "Failed to initialize the resampling context\n");
        goto end;
    }

    /* allocate source and destination samples buffers */

    src_nb_channels = src_ch_layout.nb_channels;
    /*
     * 课程注释：分配输入缓冲区。这个示例不用 AVFrame，音频数据是“每个平面一个指针”的指针数组，和 AVFrame.extended_data 是同一种结构。
     * av_samples_alloc_array_and_samples() 先分配指针数组，再分配采样数据。packed 格式只有一个平面，所以只用到 src_data[0]。
     */
    ret = av_samples_alloc_array_and_samples(&src_data, &src_linesize, src_nb_channels,
                                             src_nb_samples, src_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate source samples\n");
        goto end;
    }

    /* compute the number of converted samples: buffering is avoided
     * ensuring that the output buffer will contain at least all the
     * converted input samples */
    /*
     * 课程注释：输出缓冲区要多大？1024 个 48000 Hz 的采样，换成 44100 Hz 是 1024 × 44100 / 48000 = 940.8 个，
     * 向上取整是 941。av_rescale_rnd(a, b, c, AV_ROUND_UP) 算的就是 a × b / c 向上取整，中间结果不会溢出。
     */
    max_dst_nb_samples = dst_nb_samples =
        av_rescale_rnd(src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);

    /* buffer is going to be directly written to a rawaudio file, no alignment */
    dst_nb_channels = dst_ch_layout.nb_channels;
    ret = av_samples_alloc_array_and_samples(&dst_data, &dst_linesize, dst_nb_channels,
                                             dst_nb_samples, dst_sample_fmt, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not allocate destination samples\n");
        goto end;
    }

    t = 0;
    do {
        /* generate synthetic audio */
        fill_samples((double *)src_data[0], src_nb_samples, src_nb_channels, src_rate, &t);

        /* compute destination number of samples */
        /*
         * 课程注释：每次转换前重新估算这一次最多能输出多少。重采样器内部会留着一些输入采样（改采样率的滤波器要用到前后的采样），
         * swr_get_delay() 返回留着的数量（按 src_rate 计），加上这次新给的输入，再换算到输出采样率。
         * 估出来比现有缓冲区大，就重新分配。重新分配失败时这里用的是 break，跳出去之后程序会报告成功（patch 0039）。
         */
        dst_nb_samples = av_rescale_rnd(swr_get_delay(swr_ctx, src_rate) +
                                        src_nb_samples, dst_rate, src_rate, AV_ROUND_UP);
        if (dst_nb_samples > max_dst_nb_samples) {
            av_freep(&dst_data[0]);
            ret = av_samples_alloc(dst_data, &dst_linesize, dst_nb_channels,
                                   dst_nb_samples, dst_sample_fmt, 1);
            if (ret < 0)
                break;
            max_dst_nb_samples = dst_nb_samples;
        }

        /* convert to destination format */
        /*
         * 课程注释：第 4 步，转换。参数是：输出缓冲区和它最多能装的采样数，输入缓冲区和输入的采样数。
         * 返回值是这次实际输出的采样数（每个声道的），不一定等于估算值：看程序打印的 out，有时是 940，有时是 941。
         * 输出缓冲区装不下的部分会留在重采样器里，下次再给。
         */
        ret = swr_convert(swr_ctx, dst_data, dst_nb_samples, (const uint8_t **)src_data, src_nb_samples);
        if (ret < 0) {
            fprintf(stderr, "Error while converting\n");
            goto end;
        }
        dst_bufsize = av_samples_get_buffer_size(&dst_linesize, dst_nb_channels,
                                                 ret, dst_sample_fmt, 1);
        if (dst_bufsize < 0) {
            fprintf(stderr, "Could not get sample buffer size\n");
            goto end;
        }
        printf("t:%f in:%d out:%d\n", t, src_nb_samples, ret);
        /* 课程注释：没有检查是否写成功（patch 0040）。 */
        fwrite(dst_data[0], 1, dst_bufsize, dst_file);
    /*
     * 课程注释：循环到这里就结束了，没有冲刷。重采样器里还留着最后一小段采样，应该再调用
     * swr_convert(swr_ctx, dst_data, max_dst_nb_samples, NULL, 0) 把它们取出来，直到返回 0。
     * 少了这一步，输出比应有的少 17 个采样（patch 0041）。
     */
    } while (t < 10);

    if ((ret = get_format_from_sample_fmt(&fmt, dst_sample_fmt)) < 0)
        goto end;
    av_channel_layout_describe(&dst_ch_layout, buf, sizeof(buf));
    fprintf(stderr, "Resampling succeeded. Play the output file with the command:\n"
            /*
             * 课程注释：这条提示的命令现在不能用了：ffplay 已经没有 -channel_layout 和 -channels 这两个选项，
             * 要写成 -ch_layout 3.0（patch 0038）。
             */
            "ffplay -f %s -channel_layout %s -channels %d -ar %d %s\n",
            fmt, buf, dst_nb_channels, dst_rate, dst_filename);

end:
    fclose(dst_file);

    if (src_data)
        av_freep(&src_data[0]);
    av_freep(&src_data);

    if (dst_data)
        av_freep(&dst_data[0]);
    av_freep(&dst_data);

    /* 课程注释：释放重采样上下文。 */
    swr_free(&swr_ctx);
    return ret < 0;
}
