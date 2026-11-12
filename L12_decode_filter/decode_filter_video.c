/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
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
 * @file
 * API example for decoding and filtering
 * @example decode_filter_video.c
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/decode_filter_video.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它解码输入文件的视频，送进用一个字符串描述的滤镜图（缩小到 78x24，再逆时针转 90 度），
 * 把结果转成灰度，用几种字符在终端里“画”出来，按时间戳的间隔播放。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录（0007、0048 到 0050），
 * 下面在出问题的位置都做了标注。自己写程序时参考本目录的 filter_video.c。
 */

#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>

/*
 * 课程注释：整张滤镜图就是这一个字符串，写法和 ffmpeg 命令行的 -vf 参数完全相同。
 * 逗号把滤镜串成一条链：scale 的输出接 transpose 的输入。下面注释里的“other way”是等价的另一种写法：
 * 用方括号给连接起名字（[scl]），用分号分开两段，第二段靠名字接上第一段。
 */
const char *filter_descr = "scale=78:24,transpose=cclock";
/* other way:
   scale=78:24 [scl]; [scl] transpose=cclock // assumes "[in]" and "[out]" to be input output pads respectively
 */

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_ctx;
AVFilterContext *buffersink_ctx;
AVFilterContext *buffersrc_ctx;
AVFilterGraph *filter_graph;
static int video_stream_index = -1;
static int64_t last_pts = AV_NOPTS_VALUE;

static int open_input_file(const char *filename)
{
    const AVCodec *dec;
    int ret;

    if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /* select the video stream */
    ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find a video stream in the input file\n");
        return ret;
    }
    video_stream_index = ret;

    /* create decoding context */
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx)
        return AVERROR(ENOMEM);
    /* 课程注释：这里没有检查返回值（patch 0048）。 */
    avcodec_parameters_to_context(dec_ctx, fmt_ctx->streams[video_stream_index]->codecpar);

    /* init the video decoder */
    if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open video decoder\n");
        return ret;
    }

    return 0;
}

static int init_filters(const char *filters_descr)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
    const AVFilter *buffersink = avfilter_get_by_name("buffersink");
    /*
     * 课程注释：AVFilterInOut 表示“图上一个还没接好的口”。解析字符串时，用它告诉解析器：
     * 字符串描述的那一段图，开头接到哪里、结尾接到哪里。下面会分别填写。
     */
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVRational time_base = fmt_ctx->streams[video_stream_index]->time_base;

    filter_graph = avfilter_graph_alloc();
    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* buffer video source: the decoded frames from the decoder will be inserted here. */
    /*
     * 课程注释：入口滤镜 buffer（视频的入口，音频的叫 abuffer）的参数：画面尺寸、像素格式、时间基、像素宽高比。
     * 像素格式是按枚举的数值（%d）写进去的。色彩矩阵和色彩范围没有传，滤镜图会把输入当成“未指定”（patch 0007 补上了）。
     * 本目录的 filter_video.c 用 av_buffersrc_parameters_set() 传这些参数，不用拼字符串。
     */
    snprintf(args, sizeof(args),
            "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
            time_base.num, time_base.den,
            dec_ctx->sample_aspect_ratio.num, dec_ctx->sample_aspect_ratio.den);

    ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                       args, NULL, filter_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
        goto end;
    }

    /* buffer video sink: to terminate the filter chain. */
    /*
     * 课程注释：出口滤镜 buffersink。这里没有用一步到位的 avfilter_graph_create_filter()，
     * 而是分成“建实例、设选项、初始化”三步，因为要在初始化之前设置 pixel_formats 选项。
     */
    buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "out");
    if (!buffersink_ctx) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 课程注释：限定出口只接受 gray8（8 位灰度）。效果和在滤镜图末尾加一个 format=gray8 相同：
     * 格式协商时，上游输出的 yuv420p 和这里要求的 gray8 对不上，libavfilter 会自动插入一个 scale 来转换。
     */
    ret = av_opt_set(buffersink_ctx, "pixel_formats", "gray8",
                     AV_OPT_SEARCH_CHILDREN);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
        goto end;
    }

    ret = avfilter_init_dict(buffersink_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot initialize buffer sink\n");
        goto end;
    }

    /*
     * Set the endpoints for the filter graph. The filter_graph will
     * be linked to the graph described by filters_descr.
     */

    /*
     * The buffer source output must be connected to the input pad of
     * the first filter described by filters_descr; since the first
     * filter input label is not specified, it is set to "in" by
     * default.
     */
    /*
     * 课程注释：这里的命名是站在“字符串描述的那段图”的角度看的，容易看反：
     * outputs 是已经建好的滤镜（入口 buffer）空着的输出口，它要接到字符串那段图的输入上，那个输入的名字默认叫 "in"；
     * inputs 是已经建好的滤镜（出口 buffersink）空着的输入口，它要接到字符串那段图的输出上，那个输出的名字默认叫 "out"。
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    /*
     * The buffer sink input must be connected to the output pad of
     * the last filter described by filters_descr; since the last
     * filter output label is not specified, it is set to "out" by
     * default.
     */
    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    /*
     * 课程注释：解析字符串：按描述建立滤镜实例并连接好，再把开头和结尾接到上面给的两个口上。
     * L11 里手工写的“找滤镜、建实例、初始化、连接”，在这里由这一个函数完成。
     * 之后和 L11 一样，还要 avfilter_graph_config() 配置整张图。
     */
    if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr,
                                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

/*
 * 课程注释：显示一帧。先按这一帧和上一帧时间戳的差休眠一会儿，这样播放速度和原视频一致。
 * 然后把每个灰度值（0 到 255）除以 52 得到 0 到 4，用 " .-+#" 五种字符之一代表，越亮的像素用笔画越多的字符。
 * "\033c" 是让终端清屏的控制序列。
 */
static void display_frame(const AVFrame *frame, AVRational time_base)
{
    int x, y;
    uint8_t *p0, *p;
    int64_t delay;

    if (frame->pts != AV_NOPTS_VALUE) {
        if (last_pts != AV_NOPTS_VALUE) {
            /* sleep roughly the right amount of time;
             * usleep is in microseconds, just like AV_TIME_BASE. */
            delay = av_rescale_q(frame->pts - last_pts,
                                 time_base, AV_TIME_BASE_Q);
            if (delay > 0 && delay < 1000000)
                av_usleep(delay);
        }
        last_pts = frame->pts;
    }

    /* Trivial ASCII grayscale display. */
    p0 = frame->data[0];
    puts("\033c");
    for (y = 0; y < frame->height; y++) {
        p = p0;
        for (x = 0; x < frame->width; x++)
            putchar(" .-+#"[*(p++) / 52]);
        putchar('\n');
        p0 += frame->linesize[0];
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int ret;
    AVPacket *packet;
    AVFrame *frame;
    AVFrame *filt_frame;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s file\n", argv[0]);
        exit(1);
    }

    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    packet = av_packet_alloc();
    if (!frame || !filt_frame || !packet) {
        fprintf(stderr, "Could not allocate frame or packet\n");
        exit(1);
    }

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = init_filters(filter_descr)) < 0)
        goto end;

    /* read all packets */
    while (1) {
        /*
         * 课程注释：读到文件结尾就跳出循环了，没有给解码器送空包做冲刷。
         * 解码器里还留着的帧（L08：B 帧重排造成的延迟）没有取出来，125 帧的素材只显示 123 帧（patch 0050）。
         */
        if ((ret = av_read_frame(fmt_ctx, packet)) < 0)
            break;

        if (packet->stream_index == video_stream_index) {
            ret = avcodec_send_packet(dec_ctx, packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Error while sending a packet to the decoder\n");
                break;
            }

            /*
             * 课程注释：这个循环条件有一个隐蔽的问题：ret 会被下面从出口取帧的循环改成 AVERROR(EAGAIN)，
             * 所以这个 while 每个包实际只转一圈。平时一个包解出一帧，看不出毛病（patch 0050 一并改了）。
             */
            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while receiving a frame from the decoder\n");
                    goto end;
                }

                /*
                 * 课程注释：解码器给的 pts 偶尔会缺，best_effort_timestamp 是它根据 pts、dts 推算出的最可靠的时间戳。
                 * 滤镜图里很多滤镜要用到时间戳，送进去之前先把 pts 补上。
                 */
                frame->pts = frame->best_effort_timestamp;

                /* push the decoded frame into the filtergraph */
                /*
                 * 课程注释：把帧送进入口。带 AV_BUFFERSRC_FLAG_KEEP_REF 时入口只增加一份引用，frame 里的数据还在，
                 * 所以后面要自己 av_frame_unref(frame)。L11 用的 av_buffersrc_add_frame() 则是把数据直接拿走。
                 * 这里失败时只 break 出内层循环，错误被忽略，程序接着处理下一个包，最后退出码还是 0（patch 0049）。
                 */
                if (av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
                    av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
                    break;
                }

                /* pull filtered frames from the filtergraph */
                while (1) {
                    /*
                     * 课程注释：从出口取帧，取到 AVERROR(EAGAIN) 为止，和 L11 相同。
                     * 显示时用的时间基是出口那条连接上的时间基（buffersink_ctx->inputs[0]->time_base），不一定等于输入流的时间基：
                     * 滤镜可以改变时间基。更规范的取法是 av_buffersink_get_time_base()。
                     */
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;
                    display_frame(filt_frame, buffersink_ctx->inputs[0]->time_base);
                    av_frame_unref(filt_frame);
                }
                av_frame_unref(frame);
            }
        }
        av_packet_unref(packet);
    }
    if (ret == AVERROR_EOF) {
        /* signal EOF to the filtergraph */
        /* 课程注释：告诉滤镜图输入结束了，然后把图里剩下的帧取完。这一步示例做了；它漏的是在这之前先冲刷解码器。 */
        if (av_buffersrc_add_frame_flags(buffersrc_ctx, NULL, 0) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Error while closing the filtergraph\n");
            goto end;
        }

        /* pull remaining frames from the filtergraph */
        while (1) {
            ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
                goto end;
            display_frame(filt_frame, buffersink_ctx->inputs[0]->time_base);
            av_frame_unref(filt_frame);
        }
    }

end:
    avfilter_graph_free(&filter_graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&fmt_ctx);
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    av_packet_free(&packet);

    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        exit(1);
    }

    exit(0);
}
