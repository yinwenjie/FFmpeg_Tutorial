/*
 * L14 音频编码：把一个 WAV 文件（16 位 PCM）编码成压缩码流。
 *
 * 用法：encode_wav <输入.wav> <输出文件> <编码器名> [选项=值 ...]
 *
 * 例子：
 *   encode_wav out/src.wav out/a.aac aac
 *   encode_wav out/src.wav out/b.aac aac b=64k
 *   encode_wav out/src.wav out/c.mp2 mp2 b=192k
 *   encode_wav out/src.wav out/d.aac aac ar=22050
 *   encode_wav out/src.wav out/e.ac3 ac3
 *
 * “选项=值”和 L13 一样放进选项字典：b 是码率，ar 是采样率（和 ffmpeg 命令行的 -b:a、-ar 对应）。
 *
 * 数据的流向：
 *
 *   WAV 里的 s16 采样
 *      │  libswresample：转成编码器要的采样格式（必要时也转采样率），每次输出的数量不固定
 *      ▼
 *   AVAudioFifo：采样的缓冲队列
 *      │  每次取出正好 frame_size 个采样
 *      ▼
 *   编码器（AAC 每帧 1024 个采样，MP2 每帧 1152 个）
 *      ▼
 *   包，写进文件
 *
 * 输出是裸码流。MP2、AC-3 的包直接连起来就能播放；AAC 的包前面要各加一个 7 字节的 ADTS 头（L06 见过），
 * 否则播放器不知道一帧从哪里开始、采样率是多少。把包装进 MP4 之类的容器是 L15 的内容。
 *
 * 和官方示例 encode_audio.c 的区别：
 *   1. 输入是真实的声音，采样率和声道数从 WAV 文件头里读；
 *   2. 编码器不限于 MP2，采样格式按编码器的要求转换；
 *   3. 用缓冲队列把任意数量的采样切成编码器要求的固定帧长；
 *   4. 给每一帧设时间戳，打印每个包的 pts、时长和大小；
 *   5. 检查写文件的结果，出错时释放资源并返回非 0 的退出码。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

/* 每次从 WAV 文件里读多少个采样（每个声道）。故意取一个和任何编码器的帧长都对不上的数。 */
#define READ_SAMPLES 1000

/* 统计用的计数器。 */
typedef struct Stats {
    int64_t in_samples;     /* 从 WAV 读到的采样数（每个声道） */
    int64_t enc_samples;    /* 送进编码器的采样数，也是下一帧的 pts */
    int     nb_frames;      /* 送进编码器的帧数 */
    int     nb_packets;     /* 写出的包数 */
    int64_t bytes;          /* 写出的包的总字节数（不算 ADTS 头） */
} Stats;

/*
 * 读 WAV 的文件头，找到采样数据开始的位置。
 * WAV 文件由若干“块”组成，每块是 4 字节的名字、4 字节的长度、然后是内容：
 *   "RIFF" <长度> "WAVE"  "fmt " <长度> <格式>  ...别的块...  "data" <长度> <采样数据>
 * "fmt " 和 "data" 之间可能有别的块（ffmpeg 写的 WAV 就有一个 "LIST" 块），要跳过去。
 * 本程序只接受 16 位的 PCM。返回后文件的读写位置停在采样数据的开头。
 */
static int read_wav_header(FILE *in, int *sample_rate, int *channels)
{
    uint8_t buf[16];
    int have_fmt = 0;

    if (fread(buf, 1, 12, in) != 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "输入不是 WAV 文件\n");
        return AVERROR_INVALIDDATA;
    }

    while (fread(buf, 1, 8, in) == 8) {
        const uint32_t size = AV_RL32(buf + 4);     /* WAV 里的数都是小端的 */

        if (!memcmp(buf, "fmt ", 4)) {
            if (size < 16 || fread(buf, 1, 16, in) != 16)
                break;
            /* 格式代码 1 是 PCM；偏移 14 处是每个采样的位数。 */
            if (AV_RL16(buf) != 1 || AV_RL16(buf + 14) != 16) {
                fprintf(stderr, "只支持 16 位 PCM 的 WAV\n");
                return AVERROR_PATCHWELCOME;
            }
            *channels    = AV_RL16(buf + 2);
            *sample_rate = AV_RL32(buf + 4);
            have_fmt = 1;
            if (fseek(in, size - 16 + (size & 1), SEEK_CUR))
                break;
        } else if (!memcmp(buf, "data", 4)) {
            if (!have_fmt)
                break;
            return 0;
        } else {
            /* 不认识的块，跳过。块的长度是奇数时后面有一个填充字节。 */
            if (fseek(in, size + (size & 1), SEEK_CUR))
                break;
        }
    }
    fprintf(stderr, "WAV 文件头不完整\n");
    return AVERROR_INVALIDDATA;
}

/* 从编码器支持的采样格式里选一个：有 s16 就用 s16（不用转换），否则用列表里的第一个。 */
static int select_sample_format(const AVCodec *codec, enum AVSampleFormat *fmt)
{
    const enum AVSampleFormat *formats = NULL;
    int count = 0;
    int ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                           (const void **)&formats, &count);

    if (ret < 0)
        return ret;
    *fmt = AV_SAMPLE_FMT_S16;
    if (!formats || !count)
        return 0;
    for (int i = 0; i < count; i++)
        if (formats[i] == AV_SAMPLE_FMT_S16)
            return 0;
    *fmt = formats[0];
    return 0;
}

/*
 * 给一个 AAC 包写 ADTS 头（7 字节）。头里有同步字、AAC 的规格、采样率的编号、声道数和这一帧的总长度。
 * L06 解码 ADTS 文件时，解析器就是靠这个头把文件切成一帧一帧的。
 */
static int write_adts_header(FILE *out, const AVCodecContext *enc, int packet_size)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                 22050, 16000, 12000, 11025, 8000, 7350 };
    const int profile  = 1;                          /* AAC LC，编号是规格号减 1 */
    const int channels = enc->ch_layout.nb_channels; /* 1 到 6 个声道时，声道配置的编号等于声道数 */
    const int length   = packet_size + 7;
    int rate_index = -1;
    uint8_t h[7];

    for (int i = 0; i < 13; i++)
        if (rates[i] == enc->sample_rate)
            rate_index = i;
    if (rate_index < 0 || channels < 1 || channels > 6 || length > 0x1fff) {
        fprintf(stderr, "这组参数写不成 ADTS 头\n");
        return AVERROR(EINVAL);
    }

    h[0] = 0xff;                                             /* 同步字 12 位全是 1 */
    h[1] = 0xf1;                                             /* 同步字的后 4 位，MPEG-4，没有校验码 */
    h[2] = (profile << 6) | (rate_index << 2) | (channels >> 2);
    h[3] = ((channels & 3) << 6) | (length >> 11);
    h[4] = (length >> 3) & 0xff;
    h[5] = ((length & 7) << 5) | 0x1f;
    h[6] = 0xfc;
    return fwrite(h, 1, 7, out) == 7 ? 0 : AVERROR(EIO);
}

/*
 * 送一帧给编码器，把编好的包取出来写进文件。frame 为 NULL 表示输入结束（冲刷）。
 * 结构和 L13 的视频编码完全一样。
 */
static int encode(AVCodecContext *enc, const AVFrame *frame, AVPacket *pkt, FILE *out, Stats *st)
{
    int ret = avcodec_send_frame(enc, frame);

    if (ret < 0) {
        fprintf(stderr, "向编码器送帧失败：%s\n", av_err2str(ret));
        return ret;
    }

    while (1) {
        size_t sd_size;
        const uint8_t *sd;

        ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "编码失败：%s\n", av_err2str(ret));
            return ret;
        }

        printf("包 %3d  pts %7" PRId64 "  时长 %4" PRId64 "  %4d 字节",
               st->nb_packets, pkt->pts, pkt->duration, pkt->size);
        /*
         * 最后一个包里可能有一部分采样是编码器为了凑满一帧补上的。
         * 编码器用一份附加数据（side data）说明要丢掉多少：前 4 个字节是开头要丢的采样数，接着 4 个字节是末尾要丢的。
         */
        sd = av_packet_get_side_data(pkt, AV_PKT_DATA_SKIP_SAMPLES, &sd_size);
        if (sd && sd_size >= 8 && AV_RL32(sd + 4))
            printf("  末尾有 %u 个采样是填充", AV_RL32(sd + 4));
        printf("\n");

        if (enc->codec_id == AV_CODEC_ID_AAC) {
            ret = write_adts_header(out, enc, pkt->size);
            if (ret < 0) {
                av_packet_unref(pkt);
                return ret;
            }
        }
        if (fwrite(pkt->data, 1, pkt->size, out) != (size_t)pkt->size) {
            fprintf(stderr, "写输出文件失败：%s\n", strerror(errno));
            av_packet_unref(pkt);
            return AVERROR(EIO);
        }

        st->nb_packets++;
        st->bytes += pkt->size;
        av_packet_unref(pkt);
    }
}

/*
 * 从缓冲队列里取采样，组成帧送去编码。
 * 平时只在队列里够一整帧（frame_size 个采样）时才取；
 * flush 为 1 时（输入已经结束）把剩下不足一帧的也取出来，作为最后一帧。
 */
static int encode_from_fifo(AVCodecContext *enc, AVAudioFifo *fifo, AVFrame *frame, AVPacket *pkt,
                            FILE *out, Stats *st, int flush)
{
    /* frame_size 为 0 表示编码器不要求固定帧长（例如各种 PCM），这时有多少送多少。 */
    const int frame_size = enc->frame_size > 0 ? enc->frame_size : READ_SAMPLES;
    int ret;

    while (av_audio_fifo_size(fifo) >= frame_size || (flush && av_audio_fifo_size(fifo) > 0)) {
        const int nb = FFMIN(av_audio_fifo_size(fifo), frame_size);

        /* 上一帧的缓冲区编码器可能还在用，每一帧都重新分配。 */
        av_frame_unref(frame);
        frame->nb_samples  = nb;
        frame->format      = enc->sample_fmt;
        frame->sample_rate = enc->sample_rate;
        ret = av_channel_layout_copy(&frame->ch_layout, &enc->ch_layout);
        if (ret < 0)
            return ret;
        ret = av_frame_get_buffer(frame, 0);
        if (ret < 0)
            return ret;

        if (av_audio_fifo_read(fifo, (void **)frame->extended_data, nb) < nb)
            return AVERROR(EIO);

        /*
         * 时间戳：以编码器的时间基 1/采样率 为单位，就是这一帧之前已经送进去的采样数。
         * 注意是“输出采样率下的采样数”。转了采样率之后，不能再用输入文件里的采样位置。
         */
        frame->pts = st->enc_samples;
        st->enc_samples += nb;
        st->nb_frames++;

        ret = encode(enc, frame, pkt, out, st);
        if (ret < 0)
            return ret;
    }
    return 0;
}

/*
 * 把 nb_in 个输入采样（in 为 NULL 时是冲刷重采样器）转换成编码器的格式，放进缓冲队列。
 * 转换的写法和 L10 相同：先问最多会输出多少，分配缓冲区，再转换。
 */
static int convert_to_fifo(SwrContext *swr, AVAudioFifo *fifo, const AVCodecContext *enc,
                           const uint8_t *in, int nb_in)
{
    uint8_t **converted = NULL;
    const int max_out = swr_get_out_samples(swr, nb_in);
    int ret;

    if (max_out <= 0)
        return max_out;
    ret = av_samples_alloc_array_and_samples(&converted, NULL, enc->ch_layout.nb_channels,
                                             max_out, enc->sample_fmt, 0);
    if (ret < 0)
        return ret;

    ret = swr_convert(swr, converted, max_out, in ? &in : NULL, nb_in);
    if (ret > 0 && av_audio_fifo_write(fifo, (void **)converted, ret) < ret)
        ret = AVERROR(ENOMEM);

    av_freep(&converted[0]);
    av_freep(&converted);
    return ret < 0 ? ret : 0;
}

int main(int argc, char **argv)
{
    const char *in_name, *out_name, *codec_name;
    const AVCodec *codec;
    const AVDictionaryEntry *entry = NULL;
    AVCodecContext *enc = NULL;
    AVDictionary *opts = NULL;
    AVChannelLayout in_layout = { 0 };
    AVAudioFifo *fifo = NULL;
    SwrContext *swr = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    FILE *in = NULL, *out = NULL;
    uint8_t *in_buf = NULL;
    Stats st = { 0 };
    char layout_name[64];
    int in_rate, in_channels;
    int ret;

    if (argc < 4) {
        fprintf(stderr, "用法：%s <输入.wav> <输出文件> <编码器名> [选项=值 ...]\n", argv[0]);
        return 1;
    }
    in_name    = argv[1];
    out_name   = argv[2];
    codec_name = argv[3];

    for (int i = 4; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (!eq || eq == argv[i]) {
            fprintf(stderr, "选项要写成“名字=值”：%s\n", argv[i]);
            ret = AVERROR(EINVAL);
            goto end;
        }
        *eq = '\0';
        ret = av_dict_set(&opts, argv[i], eq + 1, 0);
        if (ret < 0)
            goto end;
    }

    in = fopen(in_name, "rb");
    if (!in) {
        fprintf(stderr, "打不开输入文件 %s：%s\n", in_name, strerror(errno));
        ret = AVERROR(errno);
        goto end;
    }
    ret = read_wav_header(in, &in_rate, &in_channels);
    if (ret < 0)
        goto end;
    /* WAV 的基本文件头里只有声道数，没有声道布局，按声道数取默认布局（2 个声道就是立体声）。 */
    av_channel_layout_default(&in_layout, in_channels);

    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        fprintf(stderr, "没有名叫 %s 的编码器\n", codec_name);
        ret = AVERROR_ENCODER_NOT_FOUND;
        goto end;
    }
    if (codec->type != AVMEDIA_TYPE_AUDIO) {
        fprintf(stderr, "%s 不是音频编码器\n", codec_name);
        ret = AVERROR(EINVAL);
        goto end;
    }

    enc = avcodec_alloc_context3(codec);
    pkt = av_packet_alloc();
    frame = av_frame_alloc();
    if (!enc || !pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 音频编码器必须知道的三样：采样格式、采样率、声道布局。
     * 采样率和声道布局先照输入的填，命令行可以用 ar=... 改采样率。
     * 采样格式由编码器说了算：AAC 只接受 fltp，MP2 只接受 s16。
     * 时间基不设也可以，音频编码器默认用 1/采样率；这里写出来是为了明确。
     */
    ret = select_sample_format(codec, &enc->sample_fmt);
    if (ret < 0)
        goto end;
    enc->sample_rate = in_rate;
    ret = av_channel_layout_copy(&enc->ch_layout, &in_layout);
    if (ret < 0)
        goto end;
    /* 和 L13 一样默认用单线程，保证每台机器上结果相同。 */
    enc->thread_count = 1;

    ret = avcodec_open2(enc, codec, &opts);
    if (ret < 0) {
        fprintf(stderr, "打不开编码器 %s：%s\n", codec_name, av_err2str(ret));
        goto end;
    }
    if (av_dict_count(opts)) {
        while ((entry = av_dict_iterate(opts, entry)))
            fprintf(stderr, "编码器 %s 不认识选项 %s（值 %s）\n", codec_name, entry->key, entry->value);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto end;
    }

    av_channel_layout_describe(&enc->ch_layout, layout_name, sizeof(layout_name));
    printf("输入：%d Hz，%d 声道，s16\n", in_rate, in_channels);
    printf("编码器 %s：%d Hz，%s，%s，目标码率 %" PRId64 " b/s\n",
           codec->name, enc->sample_rate, layout_name, av_get_sample_fmt_name(enc->sample_fmt),
           enc->bit_rate);
    /*
     * frame_size：编码器要求每一帧有多少个采样。
     * initial_padding：编码器在开头自己垫进去的采样数，解码时要丢掉。
     */
    printf("每帧 %d 个采样，开头垫 %d 个采样，时间基 %d/%d\n",
           enc->frame_size, enc->initial_padding, enc->time_base.num, enc->time_base.den);

    /* 转换器：输入的 s16 交错采样 → 编码器的采样格式和采样率。 */
    ret = swr_alloc_set_opts2(&swr,
                              &enc->ch_layout, enc->sample_fmt, enc->sample_rate,
                              &in_layout, AV_SAMPLE_FMT_S16, in_rate, 0, NULL);
    if (ret < 0)
        goto end;
    ret = swr_init(swr);
    if (ret < 0) {
        fprintf(stderr, "初始化重采样器失败：%s\n", av_err2str(ret));
        goto end;
    }

    /* 缓冲队列，初始容量随便给一个，写进去的时候会自动变大。 */
    fifo = av_audio_fifo_alloc(enc->sample_fmt, enc->ch_layout.nb_channels, READ_SAMPLES);
    in_buf = av_malloc((size_t)READ_SAMPLES * in_channels * 2);
    if (!fifo || !in_buf) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    out = fopen(out_name, "wb");
    if (!out) {
        fprintf(stderr, "打不开输出文件 %s：%s\n", out_name, strerror(errno));
        ret = AVERROR(errno);
        goto end;
    }

    while (1) {
        /* 一个“采样”包含所有声道，占 in_channels * 2 个字节。 */
        const int nb = (int)fread(in_buf, (size_t)in_channels * 2, READ_SAMPLES, in);

        if (nb <= 0)
            break;
        st.in_samples += nb;

        ret = convert_to_fifo(swr, fifo, enc, in_buf, nb);
        if (ret < 0)
            goto end;
        ret = encode_from_fifo(enc, fifo, frame, pkt, out, &st, 0);
        if (ret < 0)
            goto end;
    }
    if (ferror(in)) {
        fprintf(stderr, "读输入文件失败\n");
        ret = AVERROR(EIO);
        goto end;
    }

    /*
     * 结束的顺序，从上游到下游：
     *   1. 冲刷重采样器，把它内部剩下的采样取出来放进队列；
     *   2. 把队列里剩下的采样（包括不足一帧的尾巴）送去编码；
     *   3. 冲刷编码器。
     */
    ret = convert_to_fifo(swr, fifo, enc, NULL, 0);
    if (ret < 0)
        goto end;
    ret = encode_from_fifo(enc, fifo, frame, pkt, out, &st, 1);
    if (ret < 0)
        goto end;
    ret = encode(enc, NULL, pkt, out, &st);
    if (ret < 0)
        goto end;

    ret = fclose(out);
    out = NULL;
    if (ret) {
        fprintf(stderr, "写输出文件失败：%s\n", strerror(errno));
        ret = AVERROR(EIO);
        goto end;
    }

    printf("读入 %" PRId64 " 个采样，送进编码器 %" PRId64 " 个采样（%d 帧），写出 %d 个包\n",
           st.in_samples, st.enc_samples, st.nb_frames, st.nb_packets);
    if (st.enc_samples) {
        const double seconds = (double)st.enc_samples / enc->sample_rate;

        printf("共 %" PRId64 " 字节，时长 %.2f 秒，平均码率 %.0f kb/s\n",
               st.bytes, seconds, st.bytes * 8 / seconds / 1000);
    }
    printf("写出 %s\n", out_name);
    ret = 0;

end:
    if (out)
        fclose(out);
    if (in)
        fclose(in);
    av_free(in_buf);
    av_audio_fifo_free(fifo);
    swr_free(&swr);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    av_channel_layout_uninit(&in_layout);
    av_dict_free(&opts);
    return ret < 0 ? 1 : 0;
}
