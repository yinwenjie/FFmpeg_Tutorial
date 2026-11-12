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
 * @file libswscale API usage example
 * @example scale_video.c
 *
 * Generate a synthetic video signal and use libswscale to perform rescaling.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/scale_video.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它自己画 100 帧 320x240 的 YUV420P 图像，用 libswscale 转成 RGB24 并缩放到命令行给的尺寸，写进一个裸文件。
 *
 * 注意：这个示例用的是 sws_getContext() 加 sws_scale()，libswscale 的头文件把它们归在
 * “Legacy (stateful) API”一节。它们没有被废弃，大量现有代码还在用，所以要读得懂；
 * 但新写的程序建议用 sws_alloc_context() 加 sws_scale_frame()，见本目录的 grab_color.c。
 * 两种写法的对照在 README 第 2 步。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0034 到 0037），
 * 下面在出问题的位置都做了标注。
 */

#include <libavutil/imgutils.h>
#include <libavutil/parseutils.h>
#include <libswscale/swscale.h>

/*
 * 课程注释：画一帧 YUV420P 的测试图。data[0] 是 Y 平面，data[1]、data[2] 是 U、V 平面，
 * 宽和高都只有 Y 的一半，所以后面两个循环只走 height / 2 和 width / 2。
 * 每个平面都用 y * linesize + x 来定位，和 L05 讲的一样：一行的跨度是 linesize，不是 width。
 * frame_index 让每一帧的图案都不同，播放时能看到画面在变。
 */
static void fill_yuv_image(uint8_t *data[4], int linesize[4],
                           int width, int height, int frame_index)
{
    int x, y;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            data[0][y * linesize[0] + x] = x + y + frame_index * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            data[1][y * linesize[1] + x] = 128 + y + frame_index * 2;
            data[2][y * linesize[2] + x] = 64 + x + frame_index * 5;
        }
    }
}

int main(int argc, char **argv)
{
    /*
     * 课程注释：旧接口不用 AVFrame，图像就是“4 个平面指针加 4 个 linesize”这两个数组。
     * 这两个指针数组没有初始化，后面出错跳到 end 时会被 av_freep() 用到（patch 0034）。
     */
    uint8_t *src_data[4], *dst_data[4];
    int src_linesize[4], dst_linesize[4];
    int src_w = 320, src_h = 240, dst_w, dst_h;
    enum AVPixelFormat src_pix_fmt = AV_PIX_FMT_YUV420P, dst_pix_fmt = AV_PIX_FMT_RGB24;
    const char *dst_size = NULL;
    const char *dst_filename = NULL;
    FILE *dst_file;
    int dst_bufsize;
    struct SwsContext *sws_ctx;
    int i, ret;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s output_file output_size\n"
                "API example program to show how to scale an image with libswscale.\n"
                "This program generates a series of pictures, rescales them to the given "
                /* 课程注释：这里换行和句号写反了，句号会印在下一行开头（patch 0035）。 */
                "output_size and saves them to an output file named output_file\n."
                "\n", argv[0]);
        exit(1);
    }
    dst_filename = argv[1];
    dst_size     = argv[2];

    /* 课程注释：av_parse_video_size() 把 "640x480" 这样的字符串解析成宽和高，也认识 "hd720"、"vga" 这类缩写。 */
    if (av_parse_video_size(&dst_w, &dst_h, dst_size) < 0) {
        fprintf(stderr,
                "Invalid size '%s', must be in the form WxH or a valid size abbreviation\n",
                dst_size);
        exit(1);
    }

    dst_file = fopen(dst_filename, "wb");
    if (!dst_file) {
        fprintf(stderr, "Could not open destination file %s\n", dst_filename);
        exit(1);
    }

    /* create scaling context */
    /*
     * 课程注释：建立转换上下文。六个参数说明“从什么变成什么”：源的宽、高、像素格式，目标的宽、高、像素格式。
     * libswscale 一次调用同时做三件事：像素格式转换、缩放、色彩空间换算，所以这里的源和目标可以尺寸不同、格式也不同。
     * SWS_BILINEAR 选择缩放算法（双线性）。最后三个 NULL 是滤波器和算法参数，一般用不到。
     * 上下文建好之后就固定了：只能处理这一种“源到目标”的组合，换了尺寸或格式要重新建一个。
     * 这也是它被叫做 stateful（有状态）接口的原因。
     */
    sws_ctx = sws_getContext(src_w, src_h, src_pix_fmt,
                             dst_w, dst_h, dst_pix_fmt,
                             SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws_ctx) {
        fprintf(stderr,
                "Impossible to create scale context for the conversion "
                "fmt:%s s:%dx%d -> fmt:%s s:%dx%d\n",
                av_get_pix_fmt_name(src_pix_fmt), src_w, src_h,
                av_get_pix_fmt_name(dst_pix_fmt), dst_w, dst_h);
        ret = AVERROR(EINVAL);
        goto end;
    }

    /* allocate source and destination image buffers */
    /*
     * 课程注释：av_image_alloc() 按宽、高、像素格式分配一整块内存，再把各个平面的指针和 linesize 填好。
     * 最后一个参数是对齐：16 表示每行的字节数向上取到 16 的倍数，便于用 SIMD 指令处理。
     * 返回值是分配的总字节数。释放时只需要 av_freep(&src_data[0])，因为各个平面在同一块内存里。
     */
    if ((ret = av_image_alloc(src_data, src_linesize,
                              src_w, src_h, src_pix_fmt, 16)) < 0) {
        fprintf(stderr, "Could not allocate source image\n");
        goto end;
    }

    /*
     * 课程注释：目标图像的对齐给 1，也就是行尾不留填充。这样整块内存正好是一帧 RGB24 的裸数据，
     * 后面可以用一次 fwrite 把它写出去。如果对齐不是 1，就得一行一行写，每行只写 宽 x 3 个字节。
     */
    /* buffer is going to be written to rawvideo file, no alignment */
    if ((ret = av_image_alloc(dst_data, dst_linesize,
                              dst_w, dst_h, dst_pix_fmt, 1)) < 0) {
        fprintf(stderr, "Could not allocate destination image\n");
        goto end;
    }
    dst_bufsize = ret;

    for (i = 0; i < 100; i++) {
        /* generate synthetic video */
        fill_yuv_image(src_data, src_linesize, src_w, src_h, i);

        /* convert to destination format */
        /*
         * 课程注释：执行转换。参数依次是：上下文；源的平面指针和 linesize；从源的第几行开始、处理多少行
         * （这里是 0 和 src_h，即整幅图）；目标的平面指针和 linesize。
         * 返回值是输出的行数，出错时是负数。这里没有检查返回值（patch 0036）。
         */
        sws_scale(sws_ctx, (const uint8_t * const*)src_data,
                  src_linesize, 0, src_h, dst_data, dst_linesize);

        /* write scaled image to file */
        /* 课程注释：没有检查 fwrite 是否写成功，磁盘满了程序也报告成功（patch 0036）。 */
        fwrite(dst_data[0], 1, dst_bufsize, dst_file);
    }

    /* 课程注释：输出是没有任何头信息的裸 RGB 数据，播放时必须自己告诉 ffplay 像素格式和尺寸，和 L05 的 YUV 文件一样。 */
    fprintf(stderr, "Scaling succeeded. Play the output file with the command:\n"
           "ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
           av_get_pix_fmt_name(dst_pix_fmt), dst_w, dst_h, dst_filename);

end:
    fclose(dst_file);
    /* 课程注释：sws_getContext() 失败时也会走到这里，那时 src_data[0] 和 dst_data[0] 还是没有初始化的随机值（patch 0034）。 */
    av_freep(&src_data[0]);
    av_freep(&dst_data[0]);
    /* 课程注释：旧接口的释放函数。新接口对应的是 sws_free_context(&sws_ctx)，名字很像，注意区分。 */
    sws_freeContext(sws_ctx);
    return ret < 0;
}
