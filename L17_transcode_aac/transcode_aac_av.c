/*
 * L17 音频转码：把一个音频文件（或者视频文件里的音轨）转成 AAC 等音频编码，
 * 解码器出来的帧长和编码器要求的帧长不一样时，用 AVAudioFifo 攒起来再切。
 *
 * 用法：transcode_aac_av <输入文件> <输出文件> [名字=值 ...]
 *
 * 例子：
 *   transcode_aac_av samples/audio_44k_stereo.mp2 out/a.m4a
 *   transcode_aac_av samples/audio_44k_stereo.mp2 out/a.m4a trace=6
 *   transcode_aac_av out/in.wav out/a.m4a ar=48000 ch=1 br=64000
 *   transcode_aac_av samples/audio_44k_stereo.mp2 out/a.m4a fifo=0
 *   transcode_aac_av out/in.wav out/a.ac3 acodec=ac3
 *
 * 本程序自己用的参数：
 *   acodec=名字   输出的音频编码器，默认 aac
 *   br=比特率     比特率，单位 bit/s，默认 96000（和官方示例 transcode_aac.c 一样）
 *   ar=采样率     输出采样率，默认和输入相同；编码器不支持时取它支持的、最接近的一个
 *   ch=声道数     输出声道数，默认 2（和官方示例一样，单声道输入会变成立体声）
 *   fifo=0|1      0：不用 FIFO，解码出来的帧转换格式以后直接送编码器（用来看出错），默认 1
 *   muxdelay=毫秒 复用器的 max_delay，默认 700，和 ffmpeg 命令行一样；对 MPEG-TS 影响很大，见 README
 *   format=名字   输出格式（输出文件名没有扩展名或者不是普通文件时需要）
 *   trace=N       把前 N 个解码帧经过转换、FIFO、编码器时帧长怎么变打印出来
 *   a:名字=值     交给音频编码器的选项，例如 a:aac_coder=fast
 * 其余的“名字=值”都当作复用器的选项交给 avformat_write_header()，例如 movflags=+faststart。
 *
 * 数据的流向：
 *
 *   输入文件 ─► 解复用 ─► 包 ─► 解码器 ─► 帧 ─► 重采样器 ─► 帧 ─► FIFO ─► 帧 ─► 编码器 ─► 包 ─► 复用器
 *                                  (1152 个采样)    (改格式、采样率、声道)  (攒起来再切)   (1024 个采样)
 *
 * 和官方示例 transcode_aac.c 的区别：
 *   1. 输入可以有多路流（视频、封面图），只取最好的一路音频；官方示例要求输入只有一路流（patch 0080）；
 *   2. 输出采样率可以和输入不同：用 libswresample 做采样率转换，转换器里会缓存一些采样，结束时要冲刷；
 *      官方示例用 av_assert0() 断言两边相等，输入是 192 kHz 时 AAC 编码器不支持，直接失败；
 *   3. 编码器可以换（acodec=），帧长取编码器要求的；编码器不限制帧长（PCM、FLAC）时不用 FIFO，解码出来多少就编码多少；
 *   4. 一次送一个包进解码器以后，用循环把解码器里的帧都取出来；官方示例一次只取一帧（patch 0079 修的就是这里的一个后果）；
 *   5. 先把编码器配好、输出流建好，最后才创建输出文件，前面任何一步失败都不会留下一个空文件；
 *   6. 可以用 trace= 看每一帧怎么经过 FIFO，结束时打印解码、编码的统计；
 *   7. 检查每一步的返回值，出错时释放资源并返回非 0 的退出码，不用 exit()。
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

typedef struct Options {
    const char *acodec;     /* 输出的音频编码器 */
    const char *format;     /* 输出格式，NULL 表示按扩展名猜 */
    int64_t     br;         /* 比特率 */
    int         ar;         /* 输出采样率，0 表示和输入相同 */
    int         ch;         /* 输出声道数 */
    int         fifo;       /* 1：编码器要求固定帧长时用 FIFO 重新切 */
    int         muxdelay;   /* 复用器 max_delay，毫秒 */
    int64_t     trace;      /* 打印前几个解码帧的情况 */
} Options;

typedef struct Ctx {
    Options          opt;

    AVFormatContext *ic, *oc;   /* 输入、输出 */
    AVStream        *ist, *ost; /* 输入里选中的那路音频流、输出里的那一路流 */
    int              stream_index;
    AVCodecContext  *dec, *enc;
    SwrContext      *swr;
    AVAudioFifo     *fifo;
    AVDictionary    *enc_opts, *mux_opts;

    AVFrame         *frame;     /* 解码器输出的帧，循环里重复使用 */
    AVPacket        *pkt;       /* 读到的包、编码器输出的包，各用一次就清空 */

    int              fixed;     /* 编码器要求固定帧长 */
    int              use_fifo;  /* 要求固定帧长，并且没有用 fifo=0 关掉 */
    int              chunk;     /* 用 FIFO 时每次送给编码器的采样数：编码器要求的帧长 */
    int64_t          next_pts;  /* 下一帧第一个采样的序号，单位是编码器的时间基 1/采样率 */
    int              header_written;

    /* 统计 */
    int64_t          nb_dec, dec_samples, dec_min, dec_max;
    int64_t          conv_samples;
    int64_t          nb_enc, enc_samples, enc_last;
    int64_t          nb_pkt, bytes;
} Ctx;

/* ------------------------------------------------------------------------ */
/* 打开输入：选一路音频，开解码器                                           */
/* ------------------------------------------------------------------------ */

static int open_input(Ctx *c, const char *filename)
{
    const AVCodec *codec;
    int ret;

    ret = avformat_open_input(&c->ic, filename, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开输入文件 %s：%s\n", filename, av_err2str(ret));
        return ret;
    }
    ret = avformat_find_stream_info(c->ic, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不出输入文件的流信息：%s\n", av_err2str(ret));
        return ret;
    }

    /* 视频文件里的音轨、带封面图的音乐文件都不只一路流：挑最好的一路音频，其余的流读到了就丢掉。 */
    ret = av_find_best_stream(c->ic, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (ret < 0) {
        fprintf(stderr, "输入里没有音频流，或者没有它的解码器：%s\n", av_err2str(ret));
        return ret;
    }
    c->stream_index = ret;
    c->ist = c->ic->streams[ret];

    c->dec = avcodec_alloc_context3(codec);
    if (!c->dec)
        return AVERROR(ENOMEM);
    ret = avcodec_parameters_to_context(c->dec, c->ist->codecpar);
    if (ret < 0)
        return ret;
    /* 把包的时间基告诉解码器，要在打开之前设（L07）。 */
    c->dec->pkt_timebase = c->ist->time_base;
    /*
     * 有的文件（没写声道掩码的 WAV、WMA……）只说“2 个声道”，不说是左右还是别的，这时声道布局是“未指定”。
     * 重采样器初始化时会把它当成这个声道数的默认布局（2 个声道是立体声），可是解码出来的帧上仍然写着“未指定”，
     * 以后 swr_convert_frame() 比较一下发现不一样，报 Input changed。所以打开解码器之前就把布局定下来，
     * ffmpeg 命令行也是这样处理的。
     */
    if (c->dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&c->dec->ch_layout, c->dec->ch_layout.nb_channels);
    ret = avcodec_open2(c->dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开解码器 %s：%s\n", codec->name, av_err2str(ret));
        return ret;
    }
    return 0;
}

/* ------------------------------------------------------------------------ */
/* 打开输出：建输出上下文，配编码器，建流（这时还不创建输出文件）          */
/* ------------------------------------------------------------------------ */

/*
 * 编码器支持哪几种采样率：取最接近 want 的一个。编码器没有限制（列表是 NULL）就原样用 want。
 * ffmpeg 命令行也是这样选的：192 kHz 的输入输出 96 kHz，4 kHz 的输入输出 7.35 kHz。
 */
static int pick_sample_rate(const AVCodecContext *enc, int want)
{
    const int *rates = NULL;
    int n = 0, best = want;

    if (avcodec_get_supported_config(enc, NULL, AV_CODEC_CONFIG_SAMPLE_RATE, 0,
                                     (const void **)&rates, &n) < 0 || !rates)
        return want;
    for (int i = 0; i < n; i++) {
        if (rates[i] == want)
            return want;
        if (i == 0 || FFABS(rates[i] - want) < FFABS(best - want))
            best = rates[i];
    }
    return best;
}

static int open_output(Ctx *c, const char *filename)
{
    const Options *o = &c->opt;
    const AVCodec *codec;
    const enum AVSampleFormat *fmts = NULL;
    const AVChannelLayout *layouts = NULL;
    int nb_fmts = 0, nb_layouts = 0, want_rate, rate, ret;

    ret = avformat_alloc_output_context2(&c->oc, NULL, o->format, filename);
    if (ret < 0) {
        fprintf(stderr, "不知道输出文件 %s 是什么格式，请用 format= 指定\n", filename);
        return ret;
    }

    /*
     * 复用器最多把包攒多久再写出去。ffmpeg 命令行默认 0.7 秒，库的默认值是 0。
     * 大多数格式不受影响，MPEG-TS 受影响：它把时间上靠得近的包装进同一个 PES 包，延迟是 0 时每个音频帧单独装一个，
     * 每个 PES 包后面都要补齐到 188 字节，文件会大三成（见 README）。
     */
    c->oc->max_delay = o->muxdelay * (AV_TIME_BASE / 1000);

    codec = avcodec_find_encoder_by_name(o->acodec);
    if (!codec || codec->type != AVMEDIA_TYPE_AUDIO) {
        fprintf(stderr, "找不到音频编码器 %s\n", o->acodec);
        return AVERROR_ENCODER_NOT_FOUND;
    }
    if (avformat_query_codec(c->oc->oformat, codec->id, FF_COMPLIANCE_NORMAL) == 0) {
        fprintf(stderr, "输出格式 %s 放不下 %s 编码的音频\n", c->oc->oformat->name, codec->name);
        return AVERROR(EINVAL);
    }

    c->enc = avcodec_alloc_context3(codec);
    if (!c->enc)
        return AVERROR(ENOMEM);

    /* 声道布局：按声道数取默认布局（2 是立体声，1 是单声道，6 是 5.1），再看编码器支不支持。 */
    av_channel_layout_default(&c->enc->ch_layout, o->ch);
    if (avcodec_get_supported_config(c->enc, NULL, AV_CODEC_CONFIG_CHANNEL_LAYOUT, 0,
                                     (const void **)&layouts, &nb_layouts) >= 0 && layouts) {
        int found = 0;

        for (int i = 0; i < nb_layouts; i++)
            found |= !av_channel_layout_compare(&layouts[i], &c->enc->ch_layout);
        if (!found) {
            fprintf(stderr, "编码器 %s 不支持 %d 个声道\n", codec->name, o->ch);
            return AVERROR(EINVAL);
        }
    }

    /* 采样格式：输入的格式编码器也支持就用它（不用转换），否则取编码器支持的第一种。 */
    c->enc->sample_fmt = c->dec->sample_fmt;
    if (avcodec_get_supported_config(c->enc, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     (const void **)&fmts, &nb_fmts) >= 0 && fmts) {
        int found = 0;

        for (int i = 0; i < nb_fmts; i++)
            found |= fmts[i] == c->dec->sample_fmt;
        if (!found)
            c->enc->sample_fmt = fmts[0];
    }

    want_rate = o->ar ? o->ar : c->dec->sample_rate;
    rate = pick_sample_rate(c->enc, want_rate);
    if (rate != want_rate)
        fprintf(stderr, "编码器 %s 不支持 %d Hz，改用最接近的 %d Hz\n", codec->name, want_rate, rate);
    c->enc->sample_rate = rate;
    c->enc->bit_rate    = o->br;
    /* 音频编码器的时间基是 1/采样率，pts 就是帧里第一个采样的序号（L14）。 */
    c->enc->time_base   = (AVRational){ 1, rate };
    /* 复用器要求全局头（MP4、MKV）时，要在打开编码器之前设标志（L15）。 */
    if (c->oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    ret = avcodec_open2(c->enc, codec, &c->enc_opts);
    if (ret < 0) {
        fprintf(stderr, "打不开编码器 %s：%s\n", codec->name, av_err2str(ret));
        return ret;
    }
    if (av_dict_count(c->enc_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(c->enc_opts, NULL);

        fprintf(stderr, "编码器不认识选项 %s\n", e->key);
        return AVERROR(EINVAL);
    }

    /*
     * 编码器每帧要多少个采样，打开以后才知道：AAC 是 1024，AC-3 是 1536，MP2 是 1152。
     * 但 frame_size 大于 0 还不够说明“必须”：FLAC 的 frame_size 是它默认的块大小，送多长的帧它都接受，
     * 这样的编码器带有 AV_CODEC_CAP_VARIABLE_FRAME_SIZE 标志；PCM 的 frame_size 是 0。
     * 这两种都不用 FIFO，解码出来多少就编码多少（ffmpeg 命令行也是这样判断的）。
     */
    c->fixed    = c->enc->frame_size > 0 && !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE);
    c->use_fifo = c->fixed && o->fifo;
    c->chunk    = c->enc->frame_size;

    c->ost = avformat_new_stream(c->oc, NULL);
    if (!c->ost)
        return AVERROR(ENOMEM);
    /* 这只是向复用器“建议”的时间基，avformat_write_header() 以后以流里的值为准（L15）。 */
    c->ost->time_base = c->enc->time_base;
    ret = avcodec_parameters_from_context(c->ost->codecpar, c->enc);
    if (ret < 0)
        return ret;

    /*
     * 输入的元数据（标题、艺术家……）和这一路流的元数据、标志都带过去（patch 0081）。
     * MP4 的文件类型标签（major_brand 这三个）只描述输入文件自己，输出是 MP4 时复用器自己写，
     * 输出是别的格式时没有意义，ffmpeg 命令行也不拷贝。
     */
    ret = av_dict_copy(&c->oc->metadata, c->ic->metadata, 0);
    if (ret < 0)
        return ret;
    av_dict_set(&c->oc->metadata, "major_brand", NULL, 0);
    av_dict_set(&c->oc->metadata, "minor_version", NULL, 0);
    av_dict_set(&c->oc->metadata, "compatible_brands", NULL, 0);
    ret = av_dict_copy(&c->ost->metadata, c->ist->metadata, 0);
    if (ret < 0)
        return ret;
    c->ost->disposition = c->ist->disposition;
    return 0;
}

/* 重采样器：把解码器的格式（采样格式、采样率、声道布局）转成编码器要的。三项都一样时它什么都不做。 */
static int init_resampler(Ctx *c)
{
    int ret = swr_alloc_set_opts2(&c->swr,
                                  &c->enc->ch_layout, c->enc->sample_fmt, c->enc->sample_rate,
                                  &c->dec->ch_layout, c->dec->sample_fmt, c->dec->sample_rate,
                                  0, NULL);
    if (ret < 0) {
        fprintf(stderr, "建不了重采样器：%s\n", av_err2str(ret));
        return ret;
    }
    ret = swr_init(c->swr);
    if (ret < 0)
        fprintf(stderr, "初始化重采样器失败：%s\n", av_err2str(ret));
    return ret;
}

/* ------------------------------------------------------------------------ */
/* 编码，写包                                                               */
/* ------------------------------------------------------------------------ */

/* 分配一个装 n 个采样的帧，格式和编码器的参数一致。 */
static AVFrame *alloc_frame(const Ctx *c, int n)
{
    AVFrame *f = av_frame_alloc();

    if (!f)
        return NULL;
    f->nb_samples  = n;
    f->format      = c->enc->sample_fmt;
    f->sample_rate = c->enc->sample_rate;
    if (av_channel_layout_copy(&f->ch_layout, &c->enc->ch_layout) < 0 ||
        av_frame_get_buffer(f, 0) < 0)
        av_frame_free(&f);
    return f;
}

/*
 * 把一帧送给编码器，再把编码器里所有的包取出来写进文件。frame 是 NULL 表示冲刷编码器。
 * 和 L15 的 mux_av.c 一样：编码器的时间基和流的时间基通常不同，包写进去之前要换算。
 * 帧的时间戳官方示例也是自己数出来的：每一帧的 pts 是之前所有帧的采样数之和，不用输入的时间戳。
 */
static int encode_write(Ctx *c, AVFrame *frame)
{
    int ret;

    if (frame) {
        frame->pts = c->next_pts;
        c->next_pts += frame->nb_samples;
        c->nb_enc++;
        c->enc_samples += frame->nb_samples;
        c->enc_last = frame->nb_samples;
    }

    ret = avcodec_send_frame(c->enc, frame);
    if (ret < 0) {
        fprintf(stderr, "送帧给编码器失败：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_packet(c->enc, c->pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "从编码器取包失败：%s\n", av_err2str(ret));
            return ret;
        }
        av_packet_rescale_ts(c->pkt, c->enc->time_base, c->ost->time_base);
        c->pkt->stream_index = c->ost->index;
        c->nb_pkt++;
        c->bytes += c->pkt->size;
        ret = av_interleaved_write_frame(c->oc, c->pkt);
        if (ret < 0) {
            fprintf(stderr, "写包失败：%s\n", av_err2str(ret));
            return ret;
        }
    }
}

/*
 * 从 FIFO 里一帧一帧取，每帧 chunk 个采样，送编码器。
 * flush 不为 0（输入读完了）时，FIFO 里剩下的不足一帧的采样也送：编码器允许最后一帧短。
 * 返回送了几帧（出错时返回负数）。
 */
static int fifo_encode(Ctx *c, int flush)
{
    int frames = 0;

    while (av_audio_fifo_size(c->fifo) >= c->chunk ||
           (flush && av_audio_fifo_size(c->fifo) > 0)) {
        int n = FFMIN(av_audio_fifo_size(c->fifo), c->chunk);
        AVFrame *f = alloc_frame(c, n);
        int ret;

        if (!f)
            return AVERROR(ENOMEM);
        ret = av_audio_fifo_read(c->fifo, (void *const *)f->extended_data, n);
        if (ret < n) {
            av_frame_free(&f);
            fprintf(stderr, "从 FIFO 读数据失败\n");
            return ret < 0 ? ret : AVERROR_BUG;
        }
        ret = encode_write(c, f);
        av_frame_free(&f);
        if (ret < 0)
            return ret;
        frames++;
    }
    return frames;
}

/* ------------------------------------------------------------------------ */
/* 转换格式，存进 FIFO                                                      */
/* ------------------------------------------------------------------------ */

/*
 * 把解码出来的帧转成编码器要的格式，存进 FIFO，凑够一帧就送编码器。in 是 NULL 表示冲刷重采样器。
 *
 * swr_convert_frame() 在输出帧没有分配数据时自己分配：采样数按“转换器里缓存的采样加上这次输入”估一个上界，
 * 转换完把 nb_samples 改成真正转换出来的个数。采样率不同时转换器里会缓存一些采样（swr_get_delay()），
 * 输入结束时用 NULL 输入把它们取出来。采样率相同时转换出来的采样数和输入一样多。
 */
static int convert_store(Ctx *c, const AVFrame *in)
{
    AVFrame *out = av_frame_alloc();
    int fifo_after_write = 0, frames = 0, ret;

    if (!out)
        return AVERROR(ENOMEM);
    out->format      = c->enc->sample_fmt;
    out->sample_rate = c->enc->sample_rate;
    ret = av_channel_layout_copy(&out->ch_layout, &c->enc->ch_layout);
    if (ret < 0)
        goto end;

    ret = swr_convert_frame(c->swr, out, in);
    if (ret < 0) {
        fprintf(stderr, "转换采样失败：%s\n", av_err2str(ret));
        goto end;
    }
    c->conv_samples += out->nb_samples;

    if (out->nb_samples > 0) {
        if (!c->use_fifo) {
            /* 不用 FIFO：解码器给多少，就给编码器多少。编码器要求固定帧长时，帧长不对它会拒绝。 */
            ret = encode_write(c, out);
        } else {
            /* av_audio_fifo_write() 空间不够时自己扩大 FIFO，不用事先调 av_audio_fifo_realloc()。 */
            ret = av_audio_fifo_write(c->fifo, (void *const *)out->extended_data, out->nb_samples);
            if (ret < out->nb_samples) {
                fprintf(stderr, "写 FIFO 失败\n");
                ret = ret < 0 ? ret : AVERROR(ENOMEM);
                goto end;
            }
            fifo_after_write = av_audio_fifo_size(c->fifo);
            ret = frames = fifo_encode(c, 0);
        }
        if (ret < 0)
            goto end;
    }

    if (in && c->nb_dec - 1 < c->opt.trace) {
        if (c->use_fifo)
            printf("解码 #%-3" PRId64 " 帧长 %5d  转换后 %5d  写进 FIFO 后共 %5d  送编码器 %d 帧（每帧 %d）  FIFO 剩 %5d\n",
                   c->nb_dec - 1, in->nb_samples, out->nb_samples, fifo_after_write, frames, c->chunk,
                   av_audio_fifo_size(c->fifo));
        else
            printf("解码 #%-3" PRId64 " 帧长 %5d  转换后 %5d  直接送编码器\n",
                   c->nb_dec - 1, in->nb_samples, out->nb_samples);
    }
    ret = 0;
end:
    av_frame_free(&out);
    return ret;
}

/* ------------------------------------------------------------------------ */
/* 解码                                                                     */
/* ------------------------------------------------------------------------ */

/* 送一个包进解码器，把解码器里的帧都取出来处理。pkt 是 NULL 表示冲刷解码器。 */
static int decode_packet(Ctx *c, const AVPacket *pkt)
{
    int ret = avcodec_send_packet(c->dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "送包给解码器失败：%s\n", av_err2str(ret));
        return ret;
    }
    for (;;) {
        ret = avcodec_receive_frame(c->dec, c->frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "解码失败：%s\n", av_err2str(ret));
            return ret;
        }

        if (c->nb_dec == 0 || c->frame->nb_samples < c->dec_min)
            c->dec_min = c->frame->nb_samples;
        if (c->frame->nb_samples > c->dec_max)
            c->dec_max = c->frame->nb_samples;
        c->nb_dec++;
        c->dec_samples += c->frame->nb_samples;

        ret = convert_store(c, c->frame);
        av_frame_unref(c->frame);
        if (ret < 0)
            return ret;
    }
}

/* 读完整个输入，转码，最后从上游到下游依次冲刷：解码器 → 重采样器 → FIFO → 编码器。 */
static int transcode(Ctx *c)
{
    int ret, left;

    while ((ret = av_read_frame(c->ic, c->pkt)) >= 0) {
        /* 其它流（视频、封面图）的包直接丢掉。 */
        if (c->pkt->stream_index == c->stream_index)
            ret = decode_packet(c, c->pkt);
        else
            ret = 0;
        av_packet_unref(c->pkt);
        if (ret < 0)
            return ret;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读输入失败：%s\n", av_err2str(ret));
        return ret;
    }

    ret = decode_packet(c, NULL);
    if (ret < 0)
        return ret;

    ret = convert_store(c, NULL);
    if (ret < 0)
        return ret;
    if (c->use_fifo) {
        left = av_audio_fifo_size(c->fifo);
        ret = fifo_encode(c, 1);
        if (ret < 0)
            return ret;
        if (c->opt.trace > 0 && left > 0)
            printf("输入读完：FIFO 里剩 %d 个采样，不够一帧（每帧 %d），作为最后一帧送编码器\n",
                   left, c->chunk);
    }
    return encode_write(c, NULL);
}

/* ------------------------------------------------------------------------ */
/* 命令行                                                                   */
/* ------------------------------------------------------------------------ */

/* 把值解析成整数，要求整个字符串都是数字，并且在 [min, max] 里。 */
static int parse_int(const char *name, const char *val, int64_t min, int64_t max, int64_t *out)
{
    char *end;
    long long v;

    errno = 0;
    v = strtoll(val, &end, 10);
    if (errno || end == val || *end || v < min || v > max) {
        fprintf(stderr, "%s= 的值 %s 不对，应该是 %" PRId64 " 到 %" PRId64 " 之间的整数\n", name, val, min, max);
        return AVERROR(EINVAL);
    }
    *out = v;
    return 0;
}

static int parse_args(Ctx *c, int argc, char **argv)
{
    Options *o = &c->opt;
    int ret = 0;

    for (int i = 3; i < argc; i++) {
        const char *eq = strchr(argv[i], '='), *val;
        int64_t v = 0;

        ret = 0;
        if (!eq) {
            fprintf(stderr, "参数 %s 的写法应该是 名字=值\n", argv[i]);
            return AVERROR(EINVAL);
        }
        val = eq + 1;
        if (av_strstart(argv[i], "acodec=", NULL)) {
            o->acodec = val;
        } else if (av_strstart(argv[i], "format=", NULL)) {
            o->format = val;
        } else if (av_strstart(argv[i], "br=", NULL)) {
            ret = parse_int("br", val, 1000, INT_MAX, &o->br);
        } else if (av_strstart(argv[i], "ar=", NULL)) {
            ret = parse_int("ar", val, 1000, 768000, &v);
            o->ar = v;
        } else if (av_strstart(argv[i], "ch=", NULL)) {
            ret = parse_int("ch", val, 1, 8, &v);
            o->ch = v;
        } else if (av_strstart(argv[i], "fifo=", NULL)) {
            ret = parse_int("fifo", val, 0, 1, &v);
            o->fifo = v;
        } else if (av_strstart(argv[i], "muxdelay=", NULL)) {
            ret = parse_int("muxdelay", val, 0, 60000, &v);
            o->muxdelay = v;
        } else if (av_strstart(argv[i], "trace=", NULL)) {
            ret = parse_int("trace", val, 0, INT_MAX, &o->trace);
        } else if (av_strstart(argv[i], "a:", NULL)) {
            char *key = av_strndup(argv[i] + 2, eq - argv[i] - 2);

            if (!key)
                return AVERROR(ENOMEM);
            ret = av_dict_set(&c->enc_opts, key, val, 0);
            av_free(key);
        } else {
            char *key = av_strndup(argv[i], eq - argv[i]);

            if (!key)
                return AVERROR(ENOMEM);
            ret = av_dict_set(&c->mux_opts, key, val, 0);
            av_free(key);
        }
        if (ret < 0)
            return ret;
    }
    return 0;
}

static void describe(const Ctx *c, const char *title, const AVCodecContext *ctx)
{
    char layout[64];

    av_channel_layout_describe(&ctx->ch_layout, layout, sizeof(layout));
    printf("%s：%s，%d Hz，%s，%s", title, avcodec_get_name(ctx->codec_id), ctx->sample_rate,
           layout, av_get_sample_fmt_name(ctx->sample_fmt));
    if (ctx != c->enc) {
        printf("\n");
        return;
    }
    printf("，%" PRId64 " bit/s，", ctx->bit_rate);
    if (!c->fixed)
        printf("帧长不限，不用 FIFO\n");
    else
        printf("每帧必须 %d 个采样，%s\n", c->chunk, c->use_fifo ? "用 FIFO 切" : "没有用 FIFO（fifo=0）");
}

int main(int argc, char **argv)
{
    Ctx c = {
        .opt = { .acodec = "aac", .br = 96000, .ch = 2, .fifo = 1, .muxdelay = 700 },
    };
    int ret;

    if (argc < 3) {
        fprintf(stderr, "用法：%s <输入文件> <输出文件> [名字=值 ...]\n", argv[0]);
        return 1;
    }
    /* 标准输出按行输出：重定向到文件或管道时，也能和标准错误里的消息按发生的先后排在一起。 */
    setvbuf(stdout, NULL, _IOLBF, 0);

    ret = parse_args(&c, argc, argv);
    if (ret < 0)
        goto end;

    c.frame = av_frame_alloc();
    c.pkt   = av_packet_alloc();
    if (!c.frame || !c.pkt) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = open_input(&c, argv[1])) < 0 ||
        (ret = open_output(&c, argv[2])) < 0 ||
        (ret = init_resampler(&c)) < 0)
        goto end;

    if (c.use_fifo) {
        c.fifo = av_audio_fifo_alloc(c.enc->sample_fmt, c.enc->ch_layout.nb_channels, c.chunk);
        if (!c.fifo) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    describe(&c, "输入", c.dec);
    describe(&c, "输出", c.enc);

    /* 到这里编码器、输出流、重采样器都准备好了，才创建输出文件、写文件头。 */
    if (!(c.oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&c.oc->pb, argv[2], AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "打不开输出文件 %s：%s\n", argv[2], av_err2str(ret));
            goto end;
        }
    }
    ret = avformat_write_header(c.oc, &c.mux_opts);
    if (ret < 0) {
        fprintf(stderr, "写文件头失败：%s\n", av_err2str(ret));
        goto end;
    }
    c.header_written = 1;
    if (av_dict_count(c.mux_opts)) {
        const AVDictionaryEntry *e = av_dict_iterate(c.mux_opts, NULL);

        fprintf(stderr, "复用器不认识选项 %s\n", e->key);
        ret = AVERROR(EINVAL);
        goto end;
    }

    ret = transcode(&c);
    if (ret < 0)
        goto end;

    ret = av_write_trailer(c.oc);
    c.header_written = 0;
    if (ret < 0) {
        fprintf(stderr, "写文件尾失败：%s\n", av_err2str(ret));
        goto end;
    }

    printf("解码：%" PRId64 " 帧，%" PRId64 " 个采样，每帧 %" PRId64 " 到 %" PRId64 " 个\n",
           c.nb_dec, c.dec_samples, c.dec_min, c.dec_max);
    if (c.conv_samples != c.dec_samples)
        printf("转换：%" PRId64 " 个采样（采样率变了，采样数跟着变）\n", c.conv_samples);
    printf("编码：%" PRId64 " 帧，%" PRId64 " 个采样，最后一帧 %" PRId64 " 个\n",
           c.nb_enc, c.enc_samples, c.enc_last);
    printf("写出：%" PRId64 " 个包，%" PRId64 " 字节\n", c.nb_pkt, c.bytes);

end:
    /* 中途出错时文件头已经写了：也把文件尾写上，已经转好的部分还能播放（ffmpeg 命令行也是这样）。要在关文件之前写。 */
    if (c.oc && c.header_written)
        av_write_trailer(c.oc);
    /* 关文件时才真正写出缓冲区里剩下的数据，失败要报告。 */
    if (c.oc && !(c.oc->oformat->flags & AVFMT_NOFILE) && c.oc->pb) {
        int close_ret = avio_closep(&c.oc->pb);

        if (close_ret < 0) {
            fprintf(stderr, "关闭输出文件失败：%s\n", av_err2str(close_ret));
            if (ret >= 0)
                ret = close_ret;
        }
    }
    avformat_free_context(c.oc);
    avformat_close_input(&c.ic);
    avcodec_free_context(&c.dec);
    avcodec_free_context(&c.enc);
    swr_free(&c.swr);
    av_audio_fifo_free(c.fifo);
    av_dict_free(&c.enc_opts);
    av_dict_free(&c.mux_opts);
    av_frame_free(&c.frame);
    av_packet_free(&c.pkt);
    if (ret < 0) {
        fprintf(stderr, "出错：%s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}
