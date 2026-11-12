/*
 * L12 解码 + 滤镜：解码一个文件里的视频，送进用字符串描述的滤镜图，结果写成 Y4M 文件。
 *
 * 用法：filter_video <输入文件> <输出.y4m> <滤镜图描述>
 *
 * 例子：
 *   filter_video samples/small_360p25.mp4 out/small.y4m "scale=320:-2"
 *   filter_video samples/small_360p25.mp4 out/flip.y4m  "crop=320:360:160:0,hflip"
 *   filter_video samples/small_360p25.mp4 out/fps5.y4m  "fps=5"
 *   filter_video samples/small_360p25.mp4 out/side.y4m  "split[a][b];[b]hflip[c];[a][c]hstack"
 *
 * 滤镜图描述的写法和 ffmpeg 命令行的 -vf 参数相同。
 *
 * Y4M（YUV4MPEG2）是带一个文本头的未压缩视频：文件开头一行写明宽、高、帧率，
 * 之后每一帧是一行 "FRAME" 加上 yuv420p 的三个平面。ffplay 和 ffmpeg 都能直接打开，
 * 比 L05 的裸 YUV 文件方便：播放时不用再手工指定尺寸。
 *
 * 和官方示例 decode_filter_video.c 的区别：
 *   1. 入口滤镜的参数用 av_buffersrc_parameters_set() 传，不拼字符串，色彩属性和帧率都传到；
 *   2. 文件读完后先冲刷解码器，再结束滤镜图；
 *   3. 出口的尺寸、帧率在配置之后问出来，因为滤镜会改变它们；
 *   4. 任何一步出错都停下来并返回非 0 的退出码。
 */

#include <errno.h>
#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

typedef struct Chain {
    AVFilterGraph   *graph;
    AVFilterContext *src;           /* 入口 buffer */
    AVFilterContext *sink;          /* 出口 buffersink */
    AVFrame         *out;           /* 从出口取帧用，反复使用 */
    FILE            *file;
    const char      *descr;         /* 命令行给的滤镜图描述 */

    int64_t          nb_in;         /* 送进图的帧数 */
    int64_t          nb_out;        /* 从图里取出的帧数 */
} Chain;

/* 打印图里所有的连接和每条连接上协商出的格式。图可以有分支，所以逐个滤镜、逐个输出口地看。 */
static void print_links(const AVFilterGraph *graph)
{
    printf("图里的连接（上游 -> 下游：这条连接上的格式）：\n");
    for (unsigned i = 0; i < graph->nb_filters; i++) {
        const AVFilterContext *f = graph->filters[i];

        for (unsigned j = 0; j < f->nb_outputs; j++) {
            const AVFilterLink *link = f->outputs[j];

            printf("  %s -> %s：%dx%d，%s\n", f->name, link->dst->name, link->w, link->h,
                   av_get_pix_fmt_name(link->format));
        }
    }
}

/* 按第一帧的参数和命令行给的描述搭建滤镜图。 */
static int init_graph(Chain *c, const AVFrame *frame, AVRational time_base,
                      AVRational frame_rate)
{
    AVBufferSrcParameters *par = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    int ret;

    c->graph = avfilter_graph_alloc();
    par      = av_buffersrc_parameters_alloc();
    if (!outputs || !inputs || !c->graph || !par) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 入口。视频的入口滤镜叫 buffer。它的参数用一个结构体传：
     * 尺寸、像素格式、宽高比、色彩属性从帧上取，时间基和帧率从流上取。
     * 这里说的要和实际送进去的帧完全一致，漏了哪一项，送第一帧时入口滤镜就会警告“帧的属性变了”。
     */
    c->src = avfilter_graph_alloc_filter(c->graph, avfilter_get_by_name("buffer"), "in");
    if (!c->src) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    par->format              = frame->format;
    par->width               = frame->width;
    par->height              = frame->height;
    par->sample_aspect_ratio = frame->sample_aspect_ratio;
    par->color_space         = frame->colorspace;
    par->color_range         = frame->color_range;
    par->alpha_mode          = frame->alpha_mode;
    par->chroma_location     = frame->chroma_location;
    par->time_base           = time_base;
    par->frame_rate          = frame_rate;
    ret = av_buffersrc_parameters_set(c->src, par);
    if (ret >= 0)
        ret = avfilter_init_dict(c->src, NULL);
    if (ret < 0) {
        fprintf(stderr, "建立入口滤镜失败：%s\n", av_err2str(ret));
        goto end;
    }

    /*
     * 出口。视频的出口滤镜叫 buffersink。Y4M 文件里放 yuv420p，所以限定出口只接受这一种像素格式；
     * 滤镜图的结果如果是别的格式，格式协商时会自动插入转换。选项要在初始化之前设置。
     */
    c->sink = avfilter_graph_alloc_filter(c->graph, avfilter_get_by_name("buffersink"), "out");
    if (!c->sink) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = av_opt_set(c->sink, "pixel_formats", "yuv420p", AV_OPT_SEARCH_CHILDREN);
    if (ret >= 0)
        ret = avfilter_init_dict(c->sink, NULL);
    if (ret < 0) {
        fprintf(stderr, "建立出口滤镜失败：%s\n", av_err2str(ret));
        goto end;
    }

    /*
     * 告诉解析器，字符串描述的那一段图两头接到哪里。名字是站在“那一段图”的角度起的：
     *   outputs：已有滤镜空着的输出口（入口滤镜的输出），接到描述里名叫 [in] 的输入；
     *   inputs： 已有滤镜空着的输入口（出口滤镜的输入），接到描述里名叫 [out] 的输出。
     * 描述的开头和结尾没有写名字时，默认就是 [in] 和 [out]。
     */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = c->src;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = c->sink;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* 解析描述：建立其中的滤镜实例，按描述连接，并接到上面两个口上。 */
    ret = avfilter_graph_parse_ptr(c->graph, c->descr, &inputs, &outputs, NULL);
    if (ret < 0) {
        fprintf(stderr, "解析滤镜图描述 \"%s\" 失败：%s\n", c->descr, av_err2str(ret));
        goto end;
    }

    ret = avfilter_graph_config(c->graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "配置滤镜图失败：%s\n", av_err2str(ret));
        goto end;
    }
    print_links(c->graph);

end:
    /* 解析之后这两个链表里可能还剩着没用上的口，不管成功失败都要释放。 */
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    av_free(par);
    return ret;
}

/*
 * 写 Y4M 的文件头。出口的尺寸、帧率、宽高比要在图配置好之后问出口滤镜：
 * scale 会改尺寸，fps 会改帧率，不能照搬输入的。
 */
static int write_y4m_header(Chain *c)
{
    const int w = av_buffersink_get_w(c->sink);
    const int h = av_buffersink_get_h(c->sink);
    AVRational rate = av_buffersink_get_frame_rate(c->sink);
    AVRational sar  = av_buffersink_get_sample_aspect_ratio(c->sink);
    const AVRational tb = av_buffersink_get_time_base(c->sink);

    /* 帧率未知时（少数滤镜不保证固定帧率），用时间基的倒数凑一个，只影响播放速度。 */
    if (rate.num <= 0 || rate.den <= 0)
        rate = av_inv_q(tb);
    if (sar.num <= 0 || sar.den <= 0)
        sar = (AVRational){ 1, 1 };

    printf("出口 buffersink：%dx%d，%s，帧率 %d/%d，时间基 %d/%d\n", w, h,
           av_get_pix_fmt_name(av_buffersink_get_format(c->sink)), rate.num, rate.den,
           tb.num, tb.den);
    if (fprintf(c->file, "YUV4MPEG2 W%d H%d F%d:%d Ip A%d:%d C420mpeg2\n", w, h, rate.num,
                rate.den, sar.num, sar.den) < 0)
        return AVERROR(EIO);
    return 0;
}

/* 写一个平面，逐行写，跳过每行末尾的填充字节（L05）。 */
static int write_plane(FILE *f, const uint8_t *data, int linesize, int w, int h)
{
    for (int y = 0; y < h; y++) {
        if (fwrite(data + (ptrdiff_t)y * linesize, 1, w, f) != (size_t)w)
            return AVERROR(EIO);
    }
    return 0;
}

/*
 * 把出口现在能取的帧都取出来，写进文件。
 * 返回 0 表示暂时取完了（要再送输入），返回 1 表示图已经结束，负数是错误。
 */
static int drain(Chain *c)
{
    AVFrame *out = c->out;

    for (;;) {
        /* yuv420p 的色度平面宽高各是亮度的一半，奇数时向上取整。 */
        int cw, ch;
        int ret = av_buffersink_get_frame(c->sink, out);

        if (ret == AVERROR(EAGAIN))
            return 0;
        if (ret == AVERROR_EOF)
            return 1;
        if (ret < 0) {
            fprintf(stderr, "从滤镜图取帧出错：%s\n", av_err2str(ret));
            return ret;
        }
        cw = (out->width + 1) / 2;
        ch = (out->height + 1) / 2;
        if (fputs("FRAME\n", c->file) < 0)
            ret = AVERROR(EIO);
        if (ret >= 0)
            ret = write_plane(c->file, out->data[0], out->linesize[0], out->width, out->height);
        if (ret >= 0)
            ret = write_plane(c->file, out->data[1], out->linesize[1], cw, ch);
        if (ret >= 0)
            ret = write_plane(c->file, out->data[2], out->linesize[2], cw, ch);
        av_frame_unref(out);
        if (ret < 0) {
            fprintf(stderr, "写输出文件出错\n");
            return ret;
        }
        c->nb_out++;
    }
}

/* 送一个包（NULL 表示冲刷解码器），把解出的帧逐个送进滤镜图。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Chain *c,
                  const AVStream *stream, AVRational frame_rate)
{
    int ret = avcodec_send_packet(dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包出错：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
            return ret;
        }
        /* 很多滤镜要用时间戳。解码器给的 pts 缺失时，用它推算出的最可靠的值补上。 */
        frame->pts = frame->best_effort_timestamp;

        if (!c->graph) {
            /* 第一帧：建图，然后才能知道输出的尺寸和帧率，写文件头。 */
            ret = init_graph(c, frame, stream->time_base, frame_rate);
            if (ret >= 0)
                ret = write_y4m_header(c);
            if (ret < 0) {
                av_frame_unref(frame);
                return ret;
            }
        }

        c->nb_in++;
        /* 送进入口。这个函数把帧里的数据拿走，返回后 frame 是空的。 */
        ret = av_buffersrc_add_frame(c->src, frame);
        if (ret < 0) {
            fprintf(stderr, "把帧送进滤镜图出错：%s\n", av_err2str(ret));
            av_frame_unref(frame);
            return ret;
        }
        ret = drain(c);
        if (ret < 0)
            return ret;
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVStream *stream;
    AVRational frame_rate;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    Chain c = { 0 };
    int64_t before_flush;
    int ret, index;

    if (argc != 4) {
        fprintf(stderr, "用法：%s <输入文件> <输出.y4m> <滤镜图描述>\n"
                "例如：%s in.mp4 out.y4m \"scale=320:-2,hflip\"\n", argv[0], argv[0]);
        return 1;
    }
    c.descr = argv[3];

    ret = avformat_open_input(&fmt, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不出流信息：%s\n", av_err2str(ret));
        goto end;
    }

    /* 建立视频解码器，步骤和 L07 相同。 */
    index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0) {
        ret = index;
        fprintf(stderr, "%s 里没有能解码的视频流\n", argv[1]);
        goto end;
    }
    stream = fmt->streams[index];
    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avcodec_parameters_to_context(dec, stream->codecpar);
    if (ret < 0)
        goto end;
    dec->pkt_timebase = stream->time_base;
    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器 %s：%s\n", codec->name, av_err2str(ret));
        goto end;
    }
    /* 帧率不在帧上，要从流的信息里猜（L03）。fps 这样的滤镜和出口的帧率都从它算起。 */
    frame_rate = av_guess_frame_rate(fmt, stream, NULL);

    c.out = av_frame_alloc();
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!c.out || !pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    c.file = fopen(argv[2], "wb");
    if (!c.file) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开输出文件 %s\n", argv[2]);
        goto end;
    }

    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == index)
            ret = decode(dec, pkt, frame, &c, stream, frame_rate);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
        goto end;
    }

    /* 从上游到下游依次冲刷：先解码器，它吐出的帧照常送进图。官方示例漏了这一步。 */
    ret = decode(dec, NULL, frame, &c, stream, frame_rate);
    if (ret < 0)
        goto end;
    if (!c.graph) {
        fprintf(stderr, "没有解出任何视频帧\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /* 再告诉滤镜图输入结束了，把图里剩下的帧取完。 */
    before_flush = c.nb_out;
    ret = av_buffersrc_add_frame(c.src, NULL);
    if (ret < 0) {
        fprintf(stderr, "冲刷滤镜图出错：%s\n", av_err2str(ret));
        goto end;
    }
    ret = drain(&c);
    if (ret < 0)
        goto end;
    if (ret != 1) {
        fprintf(stderr, "滤镜图没有正常结束\n");
        ret = AVERROR_BUG;
        goto end;
    }

    printf("送进 %" PRId64 " 帧，取出 %" PRId64 " 帧，其中冲刷滤镜图时取出 %" PRId64 " 帧\n",
           c.nb_in, c.nb_out, c.nb_out - before_flush);
    printf("写出 %s\n", argv[2]);
    ret = 0;

end:
    if (c.file && fclose(c.file) && ret >= 0) {
        fprintf(stderr, "写 %s 出错\n", argv[2]);
        ret = AVERROR(EIO);
    }
    avfilter_graph_free(&c.graph);
    av_frame_free(&c.out);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
