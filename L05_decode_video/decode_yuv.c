/*
 * L05 视频解码：把裸流解码成一个完整的 YUV 文件，并打印 AVFrame 的内存布局。
 *
 * 用法：decode_yuv <解码器名> <裸流文件> <输出.yuv> [naive]
 *
 * 例子：
 *   decode_yuv mpeg1video samples/video_360p25.m1v out/video_360p25.yuv
 *   decode_yuv h264 out/small_360p25.h264 out/small_360p25.yuv
 *
 * 输出文件没有任何头部，一帧接一帧，每一帧先是整个 Y 平面，然后是 U 平面、V 平面。
 * 播放时必须自己告诉播放器像素格式、宽高和帧率，程序结束时会打印出对应的 ffplay 命令。
 *
 * 第四个参数写 naive 时，改用一种常见的错误写法（把 linesize 当成宽度），
 * 用来演示画面倾斜的问题。naive 只用于演示，不要照着写。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/common.h>
#include <libavutil/pixdesc.h>

#define INBUF_SIZE 4096

typedef struct Output {
    FILE   *file;
    int     naive;       /* 1 表示使用错误写法 */
    int64_t nb_frames;
    int64_t nb_bytes;
} Output;

/*
 * 第 plane 个平面的宽和高，单位是像素。
 * 平面 0 是亮度，和图像一样大。平面 1、2 是色度，yuv420p 的色度横竖都只有亮度的一半。
 * 缩小的倍数记在像素格式的描述里：log2_chroma_w 为 1 表示宽度除以 2，为 0 表示不缩小。
 * AV_CEIL_RSHIFT 是向上取整的右移：宽度是奇数时，色度宽度要进一位。
 */
static void plane_size(const AVFrame *frame, const AVPixFmtDescriptor *desc, int plane,
                       int *width, int *height)
{
    *width  = plane ? AV_CEIL_RSHIFT(frame->width,  desc->log2_chroma_w) : frame->width;
    *height = plane ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h) : frame->height;
}

/* 本程序只处理最常见的一类格式：三个平面、每个分量 8 位的 YUV，例如 yuv420p、yuv422p、yuv444p。 */
static int is_supported(const AVPixFmtDescriptor *desc)
{
    return desc && desc->nb_components == 3 &&
           (desc->flags & AV_PIX_FMT_FLAG_PLANAR) &&
           !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) &&
           desc->comp[0].depth == 8;
}

/* 打印一帧的内存布局。 */
static void print_layout(const AVFrame *frame, const AVCodecContext *dec,
                         const AVPixFmtDescriptor *desc)
{
    static const char *const names[] = { "Y", "U", "V" };
    int total = 0;

    printf("第 1 帧：像素格式 %s，图像 %dx%d，编码尺寸 %dx%d\n", desc->name,
           frame->width, frame->height, dec->coded_width, dec->coded_height);
    for (int p = 0; p < 3; p++) {
        int w, h;

        plane_size(frame, desc, p, &w, &h);
        printf("  平面 %d（%s）  data[%d] = %p  linesize[%d] = %3d  有效宽度 %3d  行数 %3d  每行填充 %d 字节\n",
               p, names[p], p, (void *)frame->data[p], p, frame->linesize[p], w, h,
               frame->linesize[p] - w);
        total += w * h;
    }
    printf("  一帧写进文件是 %d 字节\n\n", total);
}

/*
 * 正确的写法：一行一行地写。
 * 第 y 行从 data[p] + y * linesize[p] 开始，只有前 w 个字节是图像，后面是填充，不能写进文件。
 */
static int write_frame(const AVFrame *frame, const AVPixFmtDescriptor *desc, Output *out)
{
    for (int p = 0; p < 3; p++) {
        int w, h;

        plane_size(frame, desc, p, &w, &h);
        for (int y = 0; y < h; y++) {
            if (fwrite(frame->data[p] + y * frame->linesize[p], 1, w, out->file) != (size_t)w)
                return AVERROR(EIO);
        }
        out->nb_bytes += (int64_t)w * h;
    }
    return 0;
}

/*
 * 错误的写法，只用于演示：把一个平面当成连续的一整块，一次写出 linesize * 行数 个字节。
 * 只要 linesize 比宽度大，每一行末尾的填充就混进了文件，播放时每一行都比上一行多错开几个像素。
 */
static int write_frame_naive(const AVFrame *frame, const AVPixFmtDescriptor *desc, Output *out)
{
    for (int p = 0; p < 3; p++) {
        int w, h;
        size_t size;

        plane_size(frame, desc, p, &w, &h);
        size = (size_t)frame->linesize[p] * h;
        if (fwrite(frame->data[p], 1, size, out->file) != size)
            return AVERROR(EIO);
        out->nb_bytes += size;
    }
    return 0;
}

/* 送一个包（NULL 表示冲刷），再把能取的帧全部取出来写进文件。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Output *out)
{
    int ret = avcodec_send_packet(dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包出错：%s\n", av_err2str(ret));
        return ret;
    }

    for (;;) {
        const AVPixFmtDescriptor *desc;

        ret = avcodec_receive_frame(dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码出错：%s\n", av_err2str(ret));
            return ret;
        }

        /* 像素格式要从解码出来的帧上取。frame->format 是 enum AVPixelFormat 的值。 */
        desc = av_pix_fmt_desc_get(frame->format);
        if (!is_supported(desc)) {
            fprintf(stderr, "本程序只处理 8 位的平面 YUV，这个文件解出来的是 %s。"
                    "像素格式转换在 L09 讲。\n", desc ? desc->name : "未知格式");
            return AVERROR(ENOSYS);
        }

        if (out->nb_frames == 0)
            print_layout(frame, dec, desc);

        ret = out->naive ? write_frame_naive(frame, desc, out)
                         : write_frame(frame, desc, out);
        if (ret < 0) {
            fprintf(stderr, "写输出文件出错\n");
            return ret;
        }
        out->nb_frames++;
    }
}

int main(int argc, char **argv)
{
    const AVCodec *codec;
    AVCodecParserContext *parser = NULL;
    AVCodecContext *dec = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    FILE *in = NULL;
    Output out = { 0 };
    uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE] = { 0 };
    int eof = 0, ret = 0;

    if (argc < 4 || argc > 5 || (argc == 5 && strcmp(argv[4], "naive"))) {
        fprintf(stderr, "用法：%s <解码器名> <裸流文件> <输出.yuv> [naive]\n", argv[0]);
        return 1;
    }
    out.naive = argc == 5;

    codec = avcodec_find_decoder_by_name(argv[1]);
    if (!codec) {
        fprintf(stderr, "找不到名为 %s 的解码器\n", argv[1]);
        return 1;
    }
    parser = av_parser_init(codec->id);
    if (!parser) {
        fprintf(stderr, "编码格式 %s 没有 parser\n", avcodec_get_name(codec->id));
        return 1;
    }

    dec   = avcodec_alloc_context3(codec);
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!dec || !pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avcodec_open2(dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器：%s\n", av_err2str(ret));
        goto end;
    }

    in = fopen(argv[2], "rb");
    if (!in) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开 %s：%s\n", argv[2], av_err2str(ret));
        goto end;
    }
    out.file = fopen(argv[3], "wb");
    if (!out.file) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开 %s：%s\n", argv[3], av_err2str(ret));
        goto end;
    }

    /* 读文件、切包、解码。这个循环和 decode_trace.c 相同，只是去掉了打印。 */
    while (!eof) {
        uint8_t *data = inbuf;
        size_t data_size = fread(inbuf, 1, INBUF_SIZE, in);

        if (ferror(in)) {
            fprintf(stderr, "读 %s 出错\n", argv[2]);
            ret = AVERROR(EIO);
            goto end;
        }
        eof = !data_size;

        while (data_size > 0 || eof) {
            ret = av_parser_parse2(parser, dec, &pkt->data, &pkt->size,
                                   data, data_size, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                fprintf(stderr, "parser 出错：%s\n", av_err2str(ret));
                goto end;
            }
            data      += ret;
            data_size -= ret;

            if (pkt->size) {
                ret = decode(dec, pkt, frame, &out);
                if (ret < 0)
                    goto end;
            } else if (eof) {
                break;
            }
        }
    }

    /* 冲刷：取出解码器里剩下的帧。 */
    ret = decode(dec, NULL, frame, &out);
    if (ret < 0)
        goto end;

    /* fclose 时缓冲区里的数据才真正写盘，磁盘满之类的错误在这里才报出来，所以要检查返回值。 */
    ret = fclose(out.file);
    out.file = NULL;
    if (ret) {
        fprintf(stderr, "写 %s 出错\n", argv[3]);
        ret = AVERROR(EIO);
        goto end;
    }

    printf("写出 %" PRId64 " 帧，共 %" PRId64 " 字节%s\n", out.nb_frames, out.nb_bytes,
           out.naive ? "（naive 写法）" : "");
    if (out.nb_frames) {
        /* 帧率写在码流里，解码器解析出来之后放在 framerate。有的码流不带帧率，这时退回 25。 */
        AVRational fr = dec->framerate.num && dec->framerate.den ? dec->framerate
                                                                 : (AVRational){ 25, 1 };

        printf("播放命令：\n");
        printf("ffplay -f rawvideo -pixel_format %s -video_size %dx%d -framerate %d/%d %s\n",
               av_get_pix_fmt_name(dec->pix_fmt), dec->width, dec->height,
               fr.num, fr.den, argv[3]);
    }

end:
    if (in)
        fclose(in);
    if (out.file)
        fclose(out.file);
    av_parser_close(parser);
    avcodec_free_context(&dec);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
