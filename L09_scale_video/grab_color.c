/*
 * L09 像素格式转换与缩放：截出指定时刻的一帧，转成 RGB，按需要缩放，存成彩色图片。
 *
 * 用法：grab_color <输入文件> <秒> <输出.ppm> [宽x高] [缩放算法]
 *
 * 例子：
 *   grab_color samples/small_360p25.mp4 2.5 out/f.ppm
 *   grab_color samples/demo_1080p30.mp4 3 out/thumb.ppm 320x180
 *   grab_color samples/small_360p25.mp4 2.5 out/big.ppm 1280x720 point
 *
 * 缩放算法可以写 bilinear（默认）、bicubic、point、area、lanczos。
 *
 * 这是 L08 grab_frame 的彩色版。取帧的部分做了简化（只定位一次，落点不对就从头解），
 * 新内容都在 convert_and_save() 里：用 libswscale 把解码器给的 YUV 帧转成 RGB24。
 * 输出是 PPM 格式：一个文本头（P6、宽、高、最大值 255）后面跟 RGB 数据，常见的看图软件都能打开。
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>

typedef struct Options {
    const char *filename;   /* 输出文件 */
    int         width;      /* 输出的宽和高，0 表示和源一样 */
    int         height;
    SwsScaler   scaler;     /* 缩放算法 */
} Options;

static int parse_scaler(const char *name, SwsScaler *scaler)
{
    static const struct {
        const char *name;
        SwsScaler   scaler;
    } table[] = {
        { "bilinear", SWS_SCALE_BILINEAR }, { "bicubic", SWS_SCALE_BICUBIC },
        { "point",    SWS_SCALE_POINT },    { "area",    SWS_SCALE_AREA },
        { "lanczos",  SWS_SCALE_LANCZOS },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!strcmp(name, table[i].name)) {
            *scaler = table[i].scaler;
            return 0;
        }
    }
    return AVERROR(EINVAL);
}

/* 把 RGB24 的帧写成 PPM 文件。 */
static int write_ppm(const AVFrame *rgb, const char *filename)
{
    FILE *f = fopen(filename, "wb");
    int ret = 0;

    if (!f) {
        fprintf(stderr, "打不开输出文件 %s\n", filename);
        return AVERROR(errno);
    }
    fprintf(f, "P6\n%d %d\n255\n", rgb->width, rgb->height);
    /*
     * 逐行写。一行的有效数据是 宽 x 3 字节，linesize 可能比它大（行尾有填充，L05），
     * 所以不能把 data[0] 一次写出去。
     */
    for (int y = 0; y < rgb->height; y++)
        fwrite(rgb->data[0] + y * rgb->linesize[0], 1, (size_t)rgb->width * 3, f);
    if (ferror(f)) {
        fprintf(stderr, "写 %s 出错\n", filename);
        ret = AVERROR(EIO);
    }
    if (fclose(f) && !ret) {
        fprintf(stderr, "写 %s 出错\n", filename);
        ret = AVERROR(EIO);
    }
    return ret;
}

/*
 * 本讲的核心：把解码出来的帧转成 RGB24，同时缩放到要求的尺寸，然后存盘。
 */
static int convert_and_save(AVFrame *src, const Options *opt)
{
    SwsContext *sws = NULL;
    AVFrame *dst = NULL;
    int64_t t0, t1;
    int ret;

    printf("源：%dx%d %s，色彩矩阵 %s，范围 %s\n", src->width, src->height,
           av_get_pix_fmt_name(src->format), av_color_space_name(src->colorspace),
           av_color_range_name(src->color_range));

    /*
     * YUV 转 RGB 要用一组系数（色彩矩阵）。标清习惯用 BT.601，高清用 BT.709，两组系数不同，
     * 用错了颜色会偏。文件里应该写明用的是哪一组，但很多文件没有写（值是 unspecified）。
     * 没写的时候按通行的约定猜：高度不小于 720 当作 BT.709，否则当作 BT.601。播放器也是这么做的。
     */
    if (src->colorspace == AVCOL_SPC_UNSPECIFIED) {
        src->colorspace = src->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
        printf("文件没有写明色彩矩阵，按高度 %d 当作 %s\n", src->height,
               av_color_space_name(src->colorspace));
    }
    /* 范围同理：没写就当作有限范围（亮度 16 到 235），视频几乎都是这样。 */
    if (src->color_range == AVCOL_RANGE_UNSPECIFIED)
        src->color_range = AVCOL_RANGE_MPEG;

    /* 第 1 步：分配上下文，选缩放算法。不需要告诉它源和目标是什么格式。 */
    sws = sws_alloc_context();
    dst = av_frame_alloc();
    if (!sws || !dst) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    sws->scaler = opt->scaler;

    /*
     * 第 2 步：在目标帧上写明想要什么：像素格式、宽、高、范围。
     * 不用自己分配图像缓冲区，sws_scale_frame() 会按这些属性分配。
     * RGB 图片用全范围（0 到 255）。
     */
    dst->format      = AV_PIX_FMT_RGB24;
    dst->width       = opt->width  ? opt->width  : src->width;
    dst->height      = opt->height ? opt->height : src->height;
    dst->color_range = AVCOL_RANGE_JPEG;

    /* 第 3 步：转换。格式转换、缩放、色彩矩阵和范围的换算在这一次调用里全部完成。 */
    t0  = av_gettime_relative();
    ret = sws_scale_frame(sws, dst, src);
    t1  = av_gettime_relative();
    if (ret < 0) {
        fprintf(stderr, "转换出错：%s\n", av_err2str(ret));
        goto end;
    }
    printf("目标：%dx%d %s，转换用了 %.2f 毫秒（含第一次调用时的初始化）\n", dst->width, dst->height,
           av_get_pix_fmt_name(dst->format), (t1 - t0) / 1000.0);

    ret = write_ppm(dst, opt->filename);
    if (ret >= 0)
        printf("已写入 %s\n", opt->filename);

end:
    av_frame_free(&dst);
    sws_free_context(&sws);
    return ret;
}

/*
 * 送一个包（NULL 表示冲刷），取帧，直到取到显示时间盖住 target 的那一帧。
 * 返回 1 表示取到了（帧留在 frame 里），0 表示还没到，负数是出错。
 * 解出的第一帧就已经超过 target 时把 *overshoot 置 1。
 */
static int decode_until(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame,
                        int64_t target, int *nb_decoded, int *overshoot)
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
        if (!(*nb_decoded)++ && frame->pts > target) {
            *overshoot = 1;
            av_frame_unref(frame);
            return 0;
        }
        if (frame->pts + frame->duration > target)
            return 1;
        av_frame_unref(frame);
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVStream *stream;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    Options opt = { .scaler = SWS_SCALE_BILINEAR };
    char *end_ptr;
    double seconds;
    int64_t target;
    int ret, index, got = 0, nb_decoded = 0, overshoot = 0, restarted = 0;

    if (argc < 4 || argc > 6) {
        fprintf(stderr, "用法：%s <输入文件> <秒> <输出.ppm> [宽x高] [缩放算法]\n"
                "缩放算法：bilinear（默认）、bicubic、point、area、lanczos\n", argv[0]);
        return 1;
    }
    seconds = strtod(argv[2], &end_ptr);
    if (end_ptr == argv[2] || *end_ptr || seconds < 0) {
        fprintf(stderr, "时刻要写成不小于 0 的秒数，例如 2.5\n");
        return 1;
    }
    opt.filename = argv[3];
    if (argc >= 5 && (av_parse_video_size(&opt.width, &opt.height, argv[4]) < 0 ||
                      opt.width <= 0 || opt.height <= 0)) {
        fprintf(stderr, "尺寸要写成 宽x高，例如 320x180\n");
        return 1;
    }
    if (argc == 6 && parse_scaler(argv[5], &opt.scaler) < 0) {
        fprintf(stderr, "不认识的缩放算法 %s，可以写 bilinear、bicubic、point、area、lanczos\n", argv[5]);
        return 1;
    }

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

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* 取帧：定位到目标之前的关键帧，解码并丢弃，直到目标帧。细节见 L08 的 grab_frame.c。 */
    target = av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, stream->time_base);
    if (stream->start_time != AV_NOPTS_VALUE)
        target += stream->start_time;
    ret = av_seek_frame(fmt, index, target, AVSEEK_FLAG_BACKWARD);
    if (ret < 0) {
        fprintf(stderr, "定位到 %.3f 秒失败：%s\n", seconds, av_err2str(ret));
        goto end;
    }
    for (;;) {
        while (!got && !overshoot && (ret = av_read_frame(fmt, pkt)) >= 0) {
            if (pkt->stream_index == index)
                ret = got = decode_until(dec, pkt, frame, target, &nb_decoded, &overshoot);
            av_packet_unref(pkt);
            if (ret < 0)
                goto end;
        }
        if (!got && !overshoot) {
            if (ret != AVERROR_EOF) {
                fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
                goto end;
            }
            ret = got = decode_until(dec, NULL, frame, target, &nb_decoded, &overshoot);
            if (ret < 0)
                goto end;
        }
        if (got)
            break;
        if (restarted || (!overshoot && nb_decoded > 0)) {
            fprintf(stderr, "%.3f 秒超出了视频的长度\n", seconds);
            ret = AVERROR(EINVAL);
            goto end;
        }
        /* 落点在目标之后，或者一帧也没解出来（L08 第 5 步）：回到文件开头重新解。 */
        ret = av_seek_frame(fmt, -1, 0, AVSEEK_FLAG_BYTE);
        if (ret < 0) {
            fprintf(stderr, "定位失败：%s\n", av_err2str(ret));
            goto end;
        }
        avcodec_flush_buffers(dec);
        restarted  = 1;
        overshoot  = 0;
        nb_decoded = 0;
    }
    printf("取到的帧：pts %" PRId64 " = %.3f 秒\n", frame->pts,
           frame->pts * av_q2d(stream->time_base));

    ret = convert_and_save(frame, &opt);

end:
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
