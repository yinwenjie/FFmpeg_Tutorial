/*
 * L06 音频解码：把音频裸流解码，转换成 16 位交错 PCM，写成 WAV 文件。
 *
 * 用法：decode_wav <解码器名> <裸流文件> <输出.wav> [planes|noclip]
 *
 * 例子：
 *   decode_wav mp2  samples/audio_44k_stereo.mp2 out/mp2.wav
 *   decode_wav aac  out/tone_lr.aac              out/aac.wav
 *   decode_wav flac out/tone_lr.flac             out/flac.wav
 *
 * 解码器输出什么采样格式由解码器决定：mp2 是 s16p，aac 是 fltp，flac 是 s16 或 s32。
 * 本程序把它们统一转换成 WAV 文件里最通用的格式：16 位整数、各声道交错存放。
 * WAV 文件开头有一个 44 字节的文件头，记着采样率、声道数和位数，普通播放器可以直接播放。
 *
 * 第四个参数用来演示两种常见的错误写法，只用于演示，不要照着写：
 *   planes  planar 格式不做交错，把各个平面依次写出去
 *   noclip  浮点转整数时不做范围限制
 *
 * 这里的格式转换是手写的，目的是看清楚数据的摆放。工程里做格式转换、
 * 改采样率、改声道数用 libswresample，L10 讲。
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/samplefmt.h>

#define INBUF_SIZE      4096
#define WAV_HEADER_SIZE 44

enum Mode {
    MODE_CORRECT,
    MODE_PLANES,   /* 错误演示：不交错 */
    MODE_NOCLIP,   /* 错误演示：不限幅 */
};

typedef struct Output {
    FILE     *file;
    enum Mode mode;

    /* 第一帧的参数。WAV 文件头只能写一组参数，后面的帧必须和它一致。 */
    enum AVSampleFormat format;
    int             sample_rate;
    AVChannelLayout ch_layout;

    int64_t nb_frames;
    int64_t nb_samples;                /* 每个声道的采样数之和 */
    int     min_samples, max_samples;  /* 一帧里采样数的最小值和最大值 */
    int64_t data_bytes;                /* 已经写出的 PCM 数据字节数，不含文件头 */

    double  peak;                      /* 浮点采样里出现过的最大绝对值 */
    int64_t nb_clipped;                /* 超出 16 位范围的采样个数 */
} Output;

/* WAV 文件里的整数都是小端字节序：低位字节在前。 */
static void put_le16(uint8_t *p, unsigned v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}

/*
 * 写 44 字节的 WAV 文件头。文件由三块组成：
 *   RIFF 块：标识 "RIFF"、后面所有内容的字节数、标识 "WAVE"
 *   fmt  块：数据的参数
 *   data 块：标识 "data"、数据的字节数，后面紧跟 PCM 数据
 * 文件头里有两个长度要等数据全部写完才知道，所以这个函数会被调用两次：
 * 开始时先用 0 占位，结束时回到文件开头重写一遍。
 */
static int write_wav_header(FILE *f, int sample_rate, int channels, uint32_t data_bytes)
{
    uint8_t h[WAV_HEADER_SIZE];
    const int block_align = channels * 2;        /* 一个采样时刻的字节数：声道数 × 2 字节 */

    memcpy(h, "RIFF", 4);
    put_le32(h + 4, WAV_HEADER_SIZE - 8 + data_bytes);
    memcpy(h + 8, "WAVE", 4);

    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);                        /* fmt 块的长度 */
    put_le16(h + 20, 1);                         /* 1 表示没有压缩的整数 PCM */
    put_le16(h + 22, channels);
    put_le32(h + 24, sample_rate);
    put_le32(h + 28, sample_rate * block_align); /* 每秒字节数 */
    put_le16(h + 32, block_align);
    put_le16(h + 34, 16);                        /* 每个采样的位数 */

    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);

    return fwrite(h, 1, sizeof(h), f) == sizeof(h) ? 0 : AVERROR(EIO);
}

/*
 * 把一个浮点采样转换成 16 位整数。
 * 浮点格式约定满幅度是 -1.0 到 1.0，乘以 32768 就换算到 16 位整数的范围。
 * 有损编码解出来的波形和原始波形不完全相同，峰值可能略微超过 1.0，
 * 所以转换时必须限幅（clip）：超出范围的值按最大值或最小值算。
 */
static int16_t float_to_s16(double v, Output *out)
{
    const long scaled = lrint(v * 32768.0);     /* lrint 四舍五入到最近的整数 */

    if (fabs(v) > out->peak)
        out->peak = fabs(v);
    if (scaled < INT16_MIN || scaled > INT16_MAX)
        out->nb_clipped++;

    if (out->mode == MODE_NOCLIP)
        return (int16_t)scaled;                 /* 错误写法：超出范围的值直接截断，正的最大值会变成负数 */
    return av_clip_int16(scaled);
}

/*
 * 把一个采样转换成 16 位整数。p 指向这个采样，fmt 是它的数据类型（用 packed 的名字表示）。
 * 整数格式之间的转换就是移位：32 位取高 16 位；8 位是无符号的，先减去中点 128 再左移 8 位。
 */
static int16_t sample_to_s16(const uint8_t *p, enum AVSampleFormat fmt, Output *out)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_U8:  return (int16_t)((*p - 128) * 256);
    case AV_SAMPLE_FMT_S16: return *(const int16_t *)p;
    case AV_SAMPLE_FMT_S32: return (int16_t)(*(const int32_t *)p >> 16);
    case AV_SAMPLE_FMT_FLT: return float_to_s16(*(const float *)p, out);
    case AV_SAMPLE_FMT_DBL: return float_to_s16(*(const double *)p, out);
    default:                return 0;
    }
}

static int is_supported(enum AVSampleFormat fmt)
{
    switch (av_get_packed_sample_fmt(fmt)) {
    case AV_SAMPLE_FMT_U8:
    case AV_SAMPLE_FMT_S16:
    case AV_SAMPLE_FMT_S32:
    case AV_SAMPLE_FMT_FLT:
    case AV_SAMPLE_FMT_DBL:
        return 1;
    default:
        return 0;
    }
}

/* 按原来的数据类型打印一个采样的值。 */
static void print_sample(const uint8_t *p, enum AVSampleFormat fmt)
{
    switch (fmt) {
    case AV_SAMPLE_FMT_U8:  printf(" %6d", *p);                      break;
    case AV_SAMPLE_FMT_S16: printf(" %6d", *(const int16_t *)p);     break;
    case AV_SAMPLE_FMT_S32: printf(" %11d", *(const int32_t *)p);    break;
    case AV_SAMPLE_FMT_FLT: printf(" %9.6f", *(const float *)p);     break;
    case AV_SAMPLE_FMT_DBL: printf(" %9.6f", *(const double *)p);    break;
    default:                                                         break;
    }
}

/*
 * 第 i 个采样时刻、第 ch 个声道的采样在帧里的位置。这是本讲的核心：
 *
 *   planar：每个声道一个平面，extended_data[ch] 指向第 ch 个声道，里面依次是它的全部采样。
 *   packed：只有一个平面 extended_data[0]，各声道的采样交错存放：
 *           第 0 时刻的各个声道，第 1 时刻的各个声道……
 *
 * 用 extended_data 而不是 data：data 只有 8 个位置，planar 格式超过 8 个声道时放不下。
 * 声道数不超过 8 时，两者指向同一组指针。
 */
static const uint8_t *sample_ptr(const AVFrame *frame, int i, int ch)
{
    const int bytes    = av_get_bytes_per_sample(frame->format);
    const int channels = frame->ch_layout.nb_channels;

    if (av_sample_fmt_is_planar(frame->format))
        return frame->extended_data[ch] + i * bytes;
    return frame->extended_data[0] + (i * channels + ch) * bytes;
}

/* 打印一帧的内存布局。 */
static void print_layout(const AVFrame *frame)
{
    const enum AVSampleFormat packed = av_get_packed_sample_fmt(frame->format);
    const int planar   = av_sample_fmt_is_planar(frame->format);
    const int bytes    = av_get_bytes_per_sample(frame->format);
    const int channels = frame->ch_layout.nb_channels;
    const int planes   = planar ? channels : 1;
    char layout[64];

    av_channel_layout_describe(&frame->ch_layout, layout, sizeof(layout));

    printf("== 第一帧 ==\n");
    printf("采样格式 %s（%s，每个采样 %d 字节）\n",
           av_get_sample_fmt_name(frame->format), planar ? "planar" : "packed", bytes);
    printf("采样率 %d Hz，声道布局 %s（%d 个声道）\n", frame->sample_rate, layout, channels);
    /* nb_samples 是每个声道的采样数，不是所有声道加起来的数。 */
    printf("nb_samples = %d，时长 %.2f 毫秒\n",
           frame->nb_samples, frame->nb_samples * 1000.0 / frame->sample_rate);
    /*
     * 音频帧只填 linesize[0]：它是一个平面的字节数，所有平面一样大。
     * 和视频一样，它可能比实际数据多出一些对齐用的填充，写文件时要按 nb_samples 计算长度。
     */
    printf("%d 个平面，linesize[0] = %d 字节，一个平面的有效数据 = %d 字节\n",
           planes, frame->linesize[0], frame->nb_samples * bytes * (planar ? 1 : channels));

    for (int ch = 0; ch < channels; ch++) {
        char name[16];
        const int n = FFMIN(frame->nb_samples, 4);

        av_channel_name(name, sizeof(name),
                        av_channel_layout_channel_from_index(&frame->ch_layout, ch));
        printf("  声道 %d（%s）前 %d 个采样：", ch, name, n);
        for (int i = 0; i < n; i++)
            print_sample(sample_ptr(frame, i, ch), packed);
        printf("\n");
    }
    printf("\n");
}

/* 写一个 16 位采样，小端字节序。 */
static int write_s16(Output *out, int16_t v)
{
    uint8_t b[2];

    put_le16(b, (uint16_t)v);
    if (fwrite(b, 1, 2, out->file) != 2)
        return AVERROR(EIO);
    out->data_bytes += 2;
    return 0;
}

/* 把一帧转换成交错的 16 位采样写出去。 */
static int write_frame(const AVFrame *frame, Output *out)
{
    const enum AVSampleFormat packed = av_get_packed_sample_fmt(frame->format);
    const int channels = frame->ch_layout.nb_channels;
    int ret;

    if (out->mode == MODE_PLANES && av_sample_fmt_is_planar(frame->format)) {
        /* 错误写法：先写完第 0 个声道的全部采样，再写第 1 个声道的。播放器会把相邻的两个采样当成左、右。 */
        for (int ch = 0; ch < channels; ch++)
            for (int i = 0; i < frame->nb_samples; i++)
                if ((ret = write_s16(out, sample_to_s16(sample_ptr(frame, i, ch), packed, out))) < 0)
                    return ret;
        return 0;
    }

    /* 正确写法：外层走采样时刻，内层走声道。 */
    for (int i = 0; i < frame->nb_samples; i++)
        for (int ch = 0; ch < channels; ch++)
            if ((ret = write_s16(out, sample_to_s16(sample_ptr(frame, i, ch), packed, out))) < 0)
                return ret;
    return 0;
}

/* 处理解出来的一帧：第一帧时记下参数、写文件头，之后每一帧核对参数、写数据、做统计。 */
static int process_frame(const AVFrame *frame, Output *out)
{
    int ret;

    if (!out->nb_frames) {
        if (!is_supported(frame->format)) {
            fprintf(stderr, "本程序不处理采样格式 %s\n", av_get_sample_fmt_name(frame->format));
            return AVERROR(ENOSYS);
        }
        out->format      = frame->format;
        out->sample_rate = frame->sample_rate;
        /* AVChannelLayout 不能直接用 = 赋值，要用 av_channel_layout_copy()。 */
        ret = av_channel_layout_copy(&out->ch_layout, &frame->ch_layout);
        if (ret < 0)
            return ret;
        out->min_samples = out->max_samples = frame->nb_samples;

        print_layout(frame);

        /* 先写一个长度为 0 的文件头占位。 */
        ret = write_wav_header(out->file, out->sample_rate, out->ch_layout.nb_channels, 0);
        if (ret < 0)
            return ret;
    } else if (frame->format != out->format || frame->sample_rate != out->sample_rate ||
               av_channel_layout_compare(&frame->ch_layout, &out->ch_layout)) {
        /* 少数码流的参数会中途变化。WAV 文件头只有一组参数，遇到这种情况需要重采样（L10）。 */
        fprintf(stderr, "第 %" PRId64 " 帧的参数和第一帧不同，本程序不处理\n", out->nb_frames + 1);
        return AVERROR(ENOSYS);
    }

    ret = write_frame(frame, out);
    if (ret < 0)
        return ret;

    out->nb_frames++;
    out->nb_samples += frame->nb_samples;
    out->min_samples = FFMIN(out->min_samples, frame->nb_samples);
    out->max_samples = FFMAX(out->max_samples, frame->nb_samples);
    return 0;
}

/* 送一个包（pkt 为 NULL 表示冲刷），取出所有能取的帧。和 L05 的写法相同。 */
static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Output *out)
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
            fprintf(stderr, "取帧出错：%s\n", av_err2str(ret));
            return ret;
        }

        ret = process_frame(frame, out);
        if (ret < 0) {
            if (ret == AVERROR(EIO))
                fprintf(stderr, "写输出文件出错\n");
            return ret;
        }
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

    if (argc < 4 || argc > 5 ||
        (argc == 5 && strcmp(argv[4], "planes") && strcmp(argv[4], "noclip"))) {
        fprintf(stderr, "用法：%s <解码器名> <裸流文件> <输出.wav> [planes|noclip]\n", argv[0]);
        return 1;
    }
    if (argc == 5)
        out.mode = !strcmp(argv[4], "planes") ? MODE_PLANES : MODE_NOCLIP;

    codec = avcodec_find_decoder_by_name(argv[1]);
    if (!codec) {
        fprintf(stderr, "找不到名为 %s 的解码器\n", argv[1]);
        return 1;
    }
    if (codec->type != AVMEDIA_TYPE_AUDIO) {
        fprintf(stderr, "%s 不是音频解码器\n", argv[1]);
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

    /* 读文件、切包、解码。这个循环和 L05 的 decode_yuv.c 完全相同。 */
    while (!eof) {
        uint8_t *data = inbuf;
        size_t data_size = fread(inbuf, 1, INBUF_SIZE, in);

        if (ferror(in)) {
            fprintf(stderr, "读 %s 出错\n", argv[2]);
            ret = AVERROR(EIO);
            goto end;
        }
        eof = !data_size;

        /* 读到文件末尾时 data_size 为 0，再调用 parser 就是让它交出手里剩下的数据。 */
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

    /* 冲刷解码器。 */
    ret = decode(dec, NULL, frame, &out);
    if (ret < 0)
        goto end;

    if (!out.nb_frames) {
        fprintf(stderr, "一帧也没有解出来，%s 里可能不是 %s 裸流\n", argv[2], codec->name);
        ret = AVERROR_INVALIDDATA;
        goto end;
    }

    /* WAV 文件头里的长度是 32 位的，数据超过 4 GB 就写不下了。 */
    if (out.data_bytes > UINT32_MAX - WAV_HEADER_SIZE) {
        fprintf(stderr, "数据超过 4 GB，WAV 文件头放不下这个长度\n");
        ret = AVERROR(EFBIG);
        goto end;
    }

    /* 数据写完了，回到文件开头，把真实的长度填进文件头。输出必须是可以定位的普通文件。 */
    if (fseek(out.file, 0, SEEK_SET) < 0) {
        ret = AVERROR(errno);
        fprintf(stderr, "输出文件不能定位，文件头里的长度没有更新：%s\n", av_err2str(ret));
        goto end;
    }
    ret = write_wav_header(out.file, out.sample_rate, out.ch_layout.nb_channels,
                           (uint32_t)out.data_bytes);
    if (ret < 0) {
        fprintf(stderr, "写输出文件出错\n");
        goto end;
    }

    /* fclose 时缓冲区里的数据才真正写盘，磁盘满之类的错误在这里才报出来，所以要检查返回值。 */
    ret = fclose(out.file);
    out.file = NULL;
    if (ret) {
        fprintf(stderr, "写 %s 出错\n", argv[3]);
        ret = AVERROR(EIO);
        goto end;
    }

    printf("== 统计 ==\n");
    printf("解出 %" PRId64 " 帧，每个声道 %" PRId64 " 个采样，时长 %.3f 秒\n",
           out.nb_frames, out.nb_samples, (double)out.nb_samples / out.sample_rate);
    printf("每帧采样数：最小 %d，最大 %d\n", out.min_samples, out.max_samples);
    if (out.peak > 0)
        printf("浮点采样的最大绝对值 %.4f，超出 16 位范围的采样 %" PRId64 " 个（%s）\n",
               out.peak, out.nb_clipped,
               out.mode == MODE_NOCLIP ? "noclip 演示，没有限幅" : "已限幅");
    printf("写出 %s：%d 字节文件头 + %" PRId64 " 字节数据%s\n", argv[3], WAV_HEADER_SIZE,
           out.data_bytes, out.mode == MODE_PLANES ? "（planes 演示，没有交错）" : "");

end:
    if (in)
        fclose(in);
    if (out.file)
        fclose(out.file);
    av_channel_layout_uninit(&out.ch_layout);
    av_parser_close(parser);
    avcodec_free_context(&dec);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
