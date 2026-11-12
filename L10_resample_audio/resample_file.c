/*
 * L10 音频重采样：解码一个文件里的音频，用 libswresample 转成指定的采样率和声道布局，
 * 统一成 16 位交错 PCM，写成 WAV 文件。
 *
 * 用法：resample_file <输入文件> <输出.wav> [采样率] [声道布局]
 *
 * 例子：
 *   resample_file samples/small_360p25.mp4 out/a.wav                只转采样格式
 *   resample_file samples/small_360p25.mp4 out/a.wav 48000          改采样率
 *   resample_file samples/small_360p25.mp4 out/a.wav 16000 mono     改采样率并混成单声道
 *   resample_file samples/small_360p25.mp4 out/a.wav 0 5.1          采样率不变（写 0），扩成 5.1 声道
 *
 * L06 的 decode_wav 是手写的格式转换，只认几种采样格式，不能改采样率和声道数，
 * 码流的参数中途变化时只能报错。这里把转换交给 libswresample，这些限制都没有了。
 *
 * 官方示例 resample_audio.c 用的是 swr_convert()，输入输出是裸的缓冲区，输出能装多少个采样要自己算。
 * 本程序用按帧工作的 swr_convert_frame()：输入输出都是 AVFrame，输出缓冲区由库分配。
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#define WAV_HEADER_SIZE 44

typedef struct Resampler {
    SwrContext     *swr;
    AVFrame        *out;            /* 转换结果，反复使用 */
    FILE           *file;

    /* 输出的参数。0 或空的布局表示“和第一帧输入一样”，第一帧到了再定下来。 */
    int             out_rate;
    AVChannelLayout out_layout;

    /* 当前输入的参数，用来发现中途的变化 */
    int             in_rate;
    AVChannelLayout in_layout;
    enum AVSampleFormat in_fmt;

    int64_t         nb_in;          /* 输入的采样数 */
    int64_t         nb_out;         /* 输出的采样数 */
    double          expected;       /* 按采样率的比例应该输出多少 */
    int64_t         data_bytes;
    int             layout_guessed; /* 已经提示过“按默认布局处理” */
} Resampler;

/* WAV 文件头，和 L06 的 decode_wav.c 相同。 */
static void put_le16(uint8_t *p, unsigned v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8 & 0xff;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    put_le16(p, v & 0xffff);
    put_le16(p + 2, v >> 16);
}

static int write_wav_header(FILE *f, int sample_rate, int channels, uint32_t data_bytes)
{
    uint8_t h[WAV_HEADER_SIZE];
    const int block_align = channels * 2;

    memcpy(h, "RIFF", 4);
    put_le32(h + 4, WAV_HEADER_SIZE - 8 + data_bytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, channels);
    put_le32(h + 24, sample_rate);
    put_le32(h + 28, sample_rate * block_align);
    put_le16(h + 32, block_align);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, data_bytes);
    return fwrite(h, 1, sizeof(h), f) == sizeof(h) ? 0 : AVERROR(EIO);
}

static void describe(char *buf, size_t size, enum AVSampleFormat fmt, int rate,
                     const AVChannelLayout *layout)
{
    char name[64];

    av_channel_layout_describe(layout, name, sizeof(name));
    snprintf(buf, size, "%s，%d Hz，%s（%d 声道）", av_get_sample_fmt_name(fmt), rate, name,
             layout->nb_channels);
}

/*
 * 调用一次 swr_convert_frame() 并把结果写进文件。in 为 NULL 表示冲刷：
 * 把重采样器里还留着的采样取出来。
 */
static int convert_and_write(Resampler *r, const AVFrame *in)
{
    AVFrame *out = r->out;
    size_t bytes;
    int ret;

    /*
     * 在输出帧上写明想要什么：采样率、声道布局、采样格式。数据缓冲区不用分配，
     * swr_convert_frame() 会按这次能输出的采样数分配。
     */
    av_frame_unref(out);
    out->sample_rate = r->out_rate;
    out->format      = AV_SAMPLE_FMT_S16;
    ret = av_channel_layout_copy(&out->ch_layout, &r->out_layout);
    if (ret < 0)
        return ret;

    /*
     * 重采样器还没有初始化时，swr_convert_frame() 会按 in 和 out 两个帧上的参数自己初始化。
     * 已经初始化之后，如果输入帧的参数和初始化时不一样，它不转换，返回 AVERROR_INPUT_CHANGED。
     */
    ret = swr_convert_frame(r->swr, out, in);
    if (ret < 0)
        return ret;

    /* s16 是 packed 格式，所有声道交错放在 data[0] 里，每个采样 2 字节。 */
    bytes = (size_t)out->nb_samples * out->ch_layout.nb_channels * 2;
    if (bytes && fwrite(out->data[0], 1, bytes, r->file) != bytes) {
        fprintf(stderr, "写输出文件出错\n");
        return AVERROR(EIO);
    }
    r->nb_out     += out->nb_samples;
    r->data_bytes += bytes;
    return out->nb_samples;
}

/* 把重采样器里留着的采样全部取出来。 */
static int flush(Resampler *r)
{
    int ret, total = 0;

    if (!swr_is_initialized(r->swr))
        return 0;
    while ((ret = convert_and_write(r, NULL)) > 0)
        total += ret;
    return ret < 0 ? ret : total;
}

/* 处理解码出来的一帧。 */
static int process_frame(Resampler *r, AVFrame *frame)
{
    char buf[128];
    int ret;

    /*
     * 有的文件只记了声道数，没有记每个声道是什么（WAV、裸 PCM 常常这样），
     * 解码出来的布局是“未指定”，打印出来是 "2 channels" 而不是 "stereo"。
     * 混音需要知道每个声道的位置，所以按通行的约定补上这个声道数的默认布局：
     * 1 个声道是 mono，2 个是 stereo，6 个是 5.1。
     */
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
        const int nb_channels = frame->ch_layout.nb_channels;

        av_channel_layout_uninit(&frame->ch_layout);
        av_channel_layout_default(&frame->ch_layout, nb_channels);
        if (!r->layout_guessed) {
            av_channel_layout_describe(&frame->ch_layout, buf, sizeof(buf));
            printf("文件没有写明声道布局，%d 个声道按 %s 处理\n", nb_channels, buf);
            r->layout_guessed = 1;
        }
    }

    /* 第一帧：没有指定的输出参数用输入的。 */
    if (!r->out_rate)
        r->out_rate = frame->sample_rate;
    if (!r->out_layout.nb_channels) {
        ret = av_channel_layout_copy(&r->out_layout, &frame->ch_layout);
        if (ret < 0)
            return ret;
    }

    /* 输入的参数变了（包括第一帧）：记下来，打印一行。 */
    if (frame->sample_rate != r->in_rate || frame->format != r->in_fmt ||
        av_channel_layout_compare(&frame->ch_layout, &r->in_layout)) {
        describe(buf, sizeof(buf), frame->format, frame->sample_rate, &frame->ch_layout);
        printf("%s：%s\n", r->in_rate ? "输入的参数变了" : "输入", buf);
        if (!r->in_rate) {
            describe(buf, sizeof(buf), AV_SAMPLE_FMT_S16, r->out_rate, &r->out_layout);
            printf("输出：%s\n", buf);
        }
        r->in_rate = frame->sample_rate;
        r->in_fmt  = frame->format;
        ret = av_channel_layout_copy(&r->in_layout, &frame->ch_layout);
        if (ret < 0)
            return ret;
    }

    ret = convert_and_write(r, frame);
    if (ret == AVERROR_INPUT_CHANGED) {
        /*
         * 重采样器是按旧参数初始化的。先把里面留着的旧数据取完，再关掉它；
         * 下一次调用 swr_convert_frame() 时它会按新的输入参数重新初始化。
         */
        ret = flush(r);
        if (ret >= 0) {
            swr_close(r->swr);
            ret = convert_and_write(r, frame);
        }
    }
    if (ret < 0) {
        fprintf(stderr, "重采样出错：%s\n", av_err2str(ret));
        return ret;
    }
    r->nb_in    += frame->nb_samples;
    r->expected += (double)frame->nb_samples * r->out_rate / frame->sample_rate;
    return 0;
}

static int decode(AVCodecContext *dec, const AVPacket *pkt, AVFrame *frame, Resampler *r)
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
        ret = process_frame(r, frame);
        av_frame_unref(frame);
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
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    Resampler r = { 0 };
    char *end_ptr;
    int ret, index, flushed;

    if (argc < 3 || argc > 5) {
        fprintf(stderr, "用法：%s <输入文件> <输出.wav> [采样率] [声道布局]\n"
                "采样率写 0 表示不变；声道布局写 mono、stereo、5.1 这样的名字\n", argv[0]);
        return 1;
    }
    if (argc >= 4) {
        const long rate = strtol(argv[3], &end_ptr, 10);

        if (end_ptr == argv[3] || *end_ptr || rate < 0 || rate > 768000) {
            fprintf(stderr, "采样率要写成整数，例如 48000；写 0 表示不变\n");
            return 1;
        }
        r.out_rate = rate;
    }
    /* av_channel_layout_from_string() 把 "mono"、"stereo"、"5.1" 这样的名字变成 AVChannelLayout。 */
    if (argc == 5 && av_channel_layout_from_string(&r.out_layout, argv[4]) < 0) {
        fprintf(stderr, "不认识的声道布局 %s，可以写 mono、stereo、5.1 等\n", argv[4]);
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

    /* 建立音频解码器，步骤和 L07 相同。 */
    index = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (index < 0) {
        ret = index;
        fprintf(stderr, "%s 里没有能解码的音频流\n", argv[1]);
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

    /* 只分配，不设置参数：参数到第一次 swr_convert_frame() 时从帧上取。 */
    r.swr = swr_alloc();
    r.out = av_frame_alloc();
    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!r.swr || !r.out || !pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    r.file = fopen(argv[2], "wb");
    if (!r.file) {
        ret = AVERROR(errno);
        fprintf(stderr, "打不开输出文件 %s\n", argv[2]);
        goto end;
    }
    /* 先占住文件头的位置，数据写完再回来填（L06）。 */
    ret = write_wav_header(r.file, 0, 0, 0);
    if (ret < 0)
        goto end;

    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == index)
            ret = decode(dec, pkt, frame, &r);
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    ret = decode(dec, NULL, frame, &r);
    if (ret < 0)
        goto end;

    /*
     * 解码器冲刷完了，还要冲刷重采样器。改采样率时它内部留着一小段输入等后面的数据，
     * 输入结束后不取出来，声音的结尾就少一截。
     */
    flushed = flush(&r);
    if (flushed < 0) {
        ret = flushed;
        fprintf(stderr, "重采样出错：%s\n", av_err2str(ret));
        goto end;
    }
    if (!r.nb_in) {
        fprintf(stderr, "没有解出任何音频\n");
        ret = AVERROR_INVALIDDATA;
        goto end;
    }
    if (r.data_bytes > UINT32_MAX - WAV_HEADER_SIZE) {
        fprintf(stderr, "数据超过 4 GB，WAV 文件头放不下这个长度\n");
        ret = AVERROR(EINVAL);
        goto end;
    }
    if (fseek(r.file, 0, SEEK_SET) < 0) {
        ret = AVERROR(errno);
        goto end;
    }
    ret = write_wav_header(r.file, r.out_rate, r.out_layout.nb_channels, (uint32_t)r.data_bytes);
    if (ret < 0)
        goto end;

    printf("输入 %" PRId64 " 个采样，输出 %" PRId64 " 个采样（按采样率的比例应该是 %.1f），"
           "其中最后冲刷出 %d 个\n", r.nb_in, r.nb_out, r.expected, flushed);
    printf("写出 %s：%d 字节文件头 + %" PRId64 " 字节数据，时长 %.3f 秒\n", argv[2], WAV_HEADER_SIZE,
           r.data_bytes, (double)r.nb_out / r.out_rate);
    ret = 0;

end:
    if (r.file && fclose(r.file) && ret >= 0) {
        fprintf(stderr, "写 %s 出错\n", argv[2]);
        ret = AVERROR(EIO);
    }
    swr_free(&r.swr);
    av_frame_free(&r.out);
    av_channel_layout_uninit(&r.out_layout);
    av_channel_layout_uninit(&r.in_layout);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
