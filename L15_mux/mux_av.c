/*
 * L15 编码并封装：把视频和音频编码以后写进同一个媒体文件。
 *
 * 用法：mux_av <输入.y4m 或 none> <输入.wav 或 none> <输出文件> [选项=值 ...]
 *
 * 例子：
 *   mux_av out/src.y4m out/src.wav out/av.mp4
 *   mux_av out/src.y4m out/src.wav out/av.mkv
 *   mux_av out/src.y4m out/src.wav out/fs.mp4 movflags=+faststart
 *   mux_av none out/src.wav out/a.m4a
 *
 * 输入视频是 L13 用过的 Y4M，输入音频是 L14 用过的 WAV。
 * 输出文件的格式由扩展名决定，用的编码器是这种格式的默认编码器（MP4 和 MKV 是 libx264 和 AAC），
 * 也可以用 vcodec=、acodec= 指定。
 *
 * 命令行里“名字=值”形式的参数有四个是本程序自己用的：
 *   vcodec=名字    视频编码器       acodec=名字    音频编码器
 *   format=名字    输出格式（输出文件名没有扩展名或者不是普通文件时需要）
 *   log=1          打印每一个写进文件的包
 * 其余的都当作复用器的选项交给 avformat_write_header()，例如 movflags=+faststart。
 *
 * 数据的流向：
 *
 *   Y4M ──► 视频帧 ──► 视频编码器 ──┐
 *                                    ├──► 包 ──► av_interleaved_write_frame() ──► 复用器 ──► 文件
 *   WAV ──► swr ──► 队列 ──► 音频编码器 ┘
 *
 * 和官方示例 mux.c 的区别：
 *   1. 画面和声音来自文件，不是程序自己算的；音频用 L14 的缓冲队列切成编码器要求的帧长；
 *   2. 可以只有视频、只有音频，也可以指定编码器和复用器的选项；
 *   3. 打印打开文件后各路流的时间基（复用器可能改掉我们要求的时间基）；
 *   4. 检查每一步的返回值，出错时释放资源并返回非 0 的退出码，不用 exit()。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/version.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/intreadwrite.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

/* 每次从 WAV 里读多少个采样（每个声道），和 L14 一样。 */
#define READ_SAMPLES 1000

/* 输出文件里的一路流，以及往这一路流里写数据要用到的东西。 */
typedef struct OutStream {
    const char     *name;       /* “视频”或“音频”，打印用 */
    AVStream       *st;         /* 复用器里的流 */
    AVCodecContext *enc;        /* 这一路的编码器 */
    AVFrame        *frame;
    AVPacket       *pkt;
    int64_t         next_pts;   /* 下一帧的 pts，单位是编码器的时间基；视频是帧序号，音频是已送进去的采样数 */
    int             done;       /* 已经冲刷完 */
    int64_t         nb_frames;  /* 送进编码器的帧数 */
    int64_t         nb_packets; /* 写进文件的包数 */
    int64_t         bytes;      /* 写进文件的包的总字节数 */
} OutStream;

typedef struct Ctx {
    AVFormatContext *oc;
    int              log;
    OutStream        v, a;

    FILE            *vin;       /* Y4M 输入 */

    FILE            *ain;       /* WAV 输入 */
    int              a_channels;
    int              a_rate;
    AVChannelLayout  a_layout;
    SwrContext      *swr;
    AVAudioFifo     *fifo;
    uint8_t         *a_buf;
    int              a_eof;
    int64_t          a_in_samples;
} Ctx;

/*
 * 读 Y4M 的文件头和帧。和 L13 的 encode_yuv.c 相同。
 */
static int read_y4m_header(FILE *in, int *width, int *height, AVRational *frame_rate)
{
    char line[256];
    char *token;

    *width = *height = 0;
    *frame_rate = (AVRational){ 0, 1 };

    if (!fgets(line, sizeof(line), in) || strncmp(line, "YUV4MPEG2 ", 10)) {
        fprintf(stderr, "输入不是 Y4M 文件\n");
        return AVERROR_INVALIDDATA;
    }
    if (!strchr(line, '\n')) {
        fprintf(stderr, "Y4M 文件头太长\n");
        return AVERROR_INVALIDDATA;
    }

    for (token = strtok(line + 10, " \n"); token; token = strtok(NULL, " \n")) {
        switch (token[0]) {
        case 'W': *width  = atoi(token + 1); break;
        case 'H': *height = atoi(token + 1); break;
        case 'F':
            if (sscanf(token + 1, "%d:%d", &frame_rate->num, &frame_rate->den) != 2)
                frame_rate->num = 0;
            break;
        case 'C':
            if (strncmp(token, "C420", 4) || !strncmp(token, "C420p", 5)) {
                fprintf(stderr, "只支持 8 位 4:2:0 的 Y4M，这个文件是 %s\n", token);
                return AVERROR_PATCHWELCOME;
            }
            break;
        }
    }
    if (*width <= 0 || *height <= 0 || frame_rate->num <= 0 || frame_rate->den <= 0) {
        fprintf(stderr, "Y4M 文件头里的宽、高或帧率不正确\n");
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

/* 返回 1：读到一帧；0：文件结束；负数：出错。 */
static int read_y4m_frame(FILE *in, AVFrame *frame)
{
    char line[64];

    if (!fgets(line, sizeof(line), in))
        return ferror(in) ? AVERROR(EIO) : 0;
    if (strncmp(line, "FRAME", 5) || !strchr(line, '\n')) {
        fprintf(stderr, "Y4M 文件里的帧头不正确\n");
        return AVERROR_INVALIDDATA;
    }

    for (int plane = 0; plane < 3; plane++) {
        const int w = plane ? (frame->width  + 1) / 2 : frame->width;
        const int h = plane ? (frame->height + 1) / 2 : frame->height;

        for (int y = 0; y < h; y++) {
            if (fread(frame->data[plane] + y * frame->linesize[plane], 1, w, in) != (size_t)w) {
                fprintf(stderr, "Y4M 文件在一帧中间结束了\n");
                return AVERROR_INVALIDDATA;
            }
        }
    }
    return 1;
}

/*
 * 读 WAV 的文件头，停在采样数据的开头。只接受 16 位 PCM。
 * 和 L14 的 encode_wav.c 相比多认一种格式：声道多于两个时，规范的 WAV 用“扩展格式”（格式代码 0xFFFE）：
 * 在基本的 16 字节后面再加 24 字节，里面有声道掩码（channel_mask）和真正的格式代码。
 * 声道掩码的每一位代表一个扬声器位置，所以文件里明确写着每个声道是什么，不用靠声道数去猜。
 * mask 返回 0 表示文件里没有写。
 */
static int read_wav_header(FILE *in, int *sample_rate, int *channels, uint64_t *mask)
{
    uint8_t buf[40];
    int have_fmt = 0;

    *mask = 0;
    if (fread(buf, 1, 12, in) != 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4)) {
        fprintf(stderr, "输入不是 WAV 文件\n");
        return AVERROR_INVALIDDATA;
    }

    while (fread(buf, 1, 8, in) == 8) {
        const uint32_t size = AV_RL32(buf + 4);

        if (!memcmp(buf, "fmt ", 4)) {
            int tag, extra = 0;

            if (size < 16 || fread(buf, 1, 16, in) != 16)
                break;
            tag = AV_RL16(buf);
            *channels    = AV_RL16(buf + 2);
            *sample_rate = AV_RL32(buf + 4);
            if (tag == 0xFFFE && size >= 40) {
                if (fread(buf + 16, 1, 24, in) != 24)
                    break;
                *mask = AV_RL32(buf + 20);
                tag   = AV_RL16(buf + 24);      /* 子格式的第一个字段才是真正的格式代码 */
                extra = 24;
            }
            /* 格式代码 1 是 PCM；偏移 14 处是每个采样的位数。 */
            if (tag != 1 || AV_RL16(buf + 14) != 16) {
                fprintf(stderr, "只支持 16 位 PCM 的 WAV\n");
                return AVERROR_PATCHWELCOME;
            }
            have_fmt = 1;
            if (fseek(in, size - 16 - extra + (size & 1), SEEK_CUR))
                break;
        } else if (!memcmp(buf, "data", 4)) {
            if (!have_fmt)
                break;
            return 0;
        } else {
            if (fseek(in, size + (size & 1), SEEK_CUR))
                break;
        }
    }
    fprintf(stderr, "WAV 文件头不完整\n");
    return AVERROR_INVALIDDATA;
}

/* 从编码器支持的采样格式里选一个：有 s16 就用 s16，否则用列表里的第一个。 */
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

/* 编码器是否接受 yuv420p。没有声明限制时当作接受。 */
static int supports_yuv420p(const AVCodec *codec)
{
    const enum AVPixelFormat *formats = NULL;
    int count = 0;

    if (avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                     (const void **)&formats, &count) < 0)
        return 0;
    if (!formats || !count)
        return 1;
    for (int i = 0; i < count; i++)
        if (formats[i] == AV_PIX_FMT_YUV420P)
            return 1;
    return 0;
}

/* 把音频采样转换成编码器的格式放进队列（in 为 NULL 时冲刷重采样器）。和 L14 相同。 */
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

/*
 * 从编码器里取出所有已经编好的包，写进文件。
 *
 * 编码器输出的包，时间戳的单位是编码器的时间基；文件里这一路流有自己的时间基。
 * 两个时间基通常不同：视频编码器是 1/25，MP4 的视频流是 1/12800。
 * 所以写之前要用 av_packet_rescale_ts() 换算（pts、dts、duration 一起换算）。
 * 再告诉复用器这个包属于哪一路流（stream_index）。
 *
 * av_interleaved_write_frame() 会把包按 dts 排好顺序再写，让同一时刻的画面和声音在文件里挨在一起。
 * 它接管包里的数据，返回以后 pkt 是空的，不用再 av_packet_unref()。
 */
static int write_packets(Ctx *c, OutStream *os)
{
    int ret;

    while ((ret = avcodec_receive_packet(os->enc, os->pkt)) >= 0) {
        av_packet_rescale_ts(os->pkt, os->enc->time_base, os->st->time_base);
        os->pkt->stream_index = os->st->index;
        os->nb_packets++;
        os->bytes += os->pkt->size;
        if (c->log)
            printf("%s 包 %5" PRId64 "  pts %8" PRId64 "  dts %8" PRId64 "  时长 %5" PRId64 "  %6d 字节%s\n",
                   os->name, os->nb_packets - 1, os->pkt->pts, os->pkt->dts, os->pkt->duration,
                   os->pkt->size, (os->pkt->flags & AV_PKT_FLAG_KEY) ? "  关键帧" : "");

        ret = av_interleaved_write_frame(c->oc, os->pkt);
        if (ret < 0) {
            fprintf(stderr, "写%s包失败：%s\n", os->name, av_err2str(ret));
            return ret;
        }
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    fprintf(stderr, "从%s编码器取包失败：%s\n", os->name, av_err2str(ret));
    return ret;
}

/* 送一帧（frame 为 NULL 表示冲刷），再把能取到的包都写进文件。 */
static int send_frame(Ctx *c, OutStream *os, AVFrame *frame)
{
    int ret = avcodec_send_frame(os->enc, frame);

    if (ret < 0) {
        fprintf(stderr, "向%s编码器送帧失败：%s\n", os->name, av_err2str(ret));
        return ret;
    }
    return write_packets(c, os);
}

/* 读一帧画面，编码，写进文件。读到文件结束就冲刷编码器。 */
static int video_step(Ctx *c)
{
    OutStream *os = &c->v;
    int ret = av_frame_make_writable(os->frame);

    if (ret < 0)
        return ret;
    ret = read_y4m_frame(c->vin, os->frame);
    if (ret < 0)
        return ret;
    if (ret == 0) {
        os->done = 1;
        return send_frame(c, os, NULL);
    }

    os->frame->pts      = os->next_pts++;
    os->frame->duration = 1;            /* 一帧的时长，单位是编码器的时间基 */
    os->nb_frames++;
    return send_frame(c, os, os->frame);
}

/*
 * 凑够编码器要求的一帧音频，编码，写进文件。做法和 L14 一样：
 * 队列里不够一帧就继续读 WAV；WAV 读完以后先冲刷重采样器，把队列里剩下的采样作为最后一帧，最后冲刷编码器。
 */
static int audio_step(Ctx *c)
{
    OutStream *os = &c->a;
    const int frame_size = os->enc->frame_size > 0 ? os->enc->frame_size : READ_SAMPLES;
    int avail, nb, ret;

    while (av_audio_fifo_size(c->fifo) < frame_size && !c->a_eof) {
        const int n = (int)fread(c->a_buf, (size_t)c->a_channels * 2, READ_SAMPLES, c->ain);

        if (n <= 0) {
            if (ferror(c->ain)) {
                fprintf(stderr, "读 WAV 文件失败\n");
                return AVERROR(EIO);
            }
            c->a_eof = 1;
            ret = convert_to_fifo(c->swr, c->fifo, os->enc, NULL, 0);
            if (ret < 0)
                return ret;
            break;
        }
        c->a_in_samples += n;
        ret = convert_to_fifo(c->swr, c->fifo, os->enc, c->a_buf, n);
        if (ret < 0)
            return ret;
    }

    avail = av_audio_fifo_size(c->fifo);
    if (avail == 0) {
        os->done = 1;
        return send_frame(c, os, NULL);
    }

    nb = FFMIN(avail, frame_size);
    av_frame_unref(os->frame);
    os->frame->nb_samples  = nb;
    os->frame->format      = os->enc->sample_fmt;
    os->frame->sample_rate = os->enc->sample_rate;
    ret = av_channel_layout_copy(&os->frame->ch_layout, &os->enc->ch_layout);
    if (ret < 0)
        return ret;
    ret = av_frame_get_buffer(os->frame, 0);
    if (ret < 0)
        return ret;
    if (av_audio_fifo_read(c->fifo, (void **)os->frame->extended_data, nb) < nb)
        return AVERROR(EIO);

    /* pts 是这一帧之前已经送进编码器的采样数，时间基是 1/采样率。 */
    os->frame->pts = os->next_pts;
    os->next_pts  += nb;
    os->nb_frames++;
    return send_frame(c, os, os->frame);
}

/*
 * 创建输出文件里的一路流并打开编码器。
 * 次序有讲究：
 *   1. 先看复用器要不要全局头（AVFMT_GLOBALHEADER）。MP4、MKV 要，它们把 SPS/PPS、AAC 的配置
 *      放在文件头里，不放在每个包里。这个标志必须在 avcodec_open2() 之前设，
 *      编码器才会把这些信息放进 extradata，而不是塞进码流。
 *   2. 打开编码器。
 *   3. avcodec_parameters_from_context() 把编码器的参数（含 extradata）复制给流，复用器靠它写文件头。
 */
static int add_stream(Ctx *c, OutStream *os, AVCodecContext *enc, const AVCodec *codec)
{
    char ident[64];
    int ret;

    os->enc = enc;
    if (c->oc->oformat->flags & AVFMT_GLOBALHEADER)
        enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    /* 和 L13、L14 一样默认单线程，保证每台机器上结果相同。 */
    enc->thread_count = 1;

    ret = avcodec_open2(enc, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开%s编码器 %s：%s\n", os->name, codec->name, av_err2str(ret));
        return ret;
    }

    os->st = avformat_new_stream(c->oc, NULL);
    os->pkt = av_packet_alloc();
    os->frame = av_frame_alloc();
    if (!os->st || !os->pkt || !os->frame)
        return AVERROR(ENOMEM);

    ret = avcodec_parameters_from_context(os->st->codecpar, enc);
    if (ret < 0)
        return ret;
    /* 这只是向复用器“建议”的时间基，avformat_write_header() 以后以流里的值为准。 */
    os->st->time_base = enc->time_base;

    /*
     * 记下是谁编码的。ffmpeg 命令行也给每一路流写这一条元数据（encoder=Lavc… libx264），
     * MP4 复用器会把它写进视频的描述里。加上它，输出才能和命令行的结果逐字节相同。
     */
    snprintf(ident, sizeof(ident), "%s %s", LIBAVCODEC_IDENT, codec->name);
    return av_dict_set(&os->st->metadata, "encoder", ident, 0);
}

static void print_stream(const OutStream *os)
{
    printf("%s：编码器 %s，编码器时间基 %d/%d，文件里的流时间基 %d/%d，extradata %d 字节\n",
           os->name, os->enc->codec->name,
           os->enc->time_base.num, os->enc->time_base.den,
           os->st->time_base.num, os->st->time_base.den,
           os->enc->extradata_size);
}

int main(int argc, char **argv)
{
    const char *vin_name, *ain_name, *out_name;
    const char *vcodec_name = NULL, *acodec_name = NULL, *format_name = NULL;
    const AVCodec *vcodec = NULL, *acodec = NULL;
    const AVDictionaryEntry *entry = NULL;
    AVDictionary *mux_opts = NULL;
    AVCodecContext *venc = NULL, *aenc = NULL;
    Ctx c = { 0 };
    uint64_t mask;
    int ret;

    if (argc < 4) {
        fprintf(stderr, "用法：%s <输入.y4m 或 none> <输入.wav 或 none> <输出文件> [选项=值 ...]\n", argv[0]);
        return 1;
    }
    vin_name = argv[1];
    ain_name = argv[2];
    out_name = argv[3];
    if (!strcmp(vin_name, "none") && !strcmp(ain_name, "none")) {
        fprintf(stderr, "视频和音频不能都是 none\n");
        return 1;
    }

    for (int i = 4; i < argc; i++) {
        char *eq = strchr(argv[i], '=');

        if (!eq || eq == argv[i]) {
            fprintf(stderr, "选项要写成“名字=值”：%s\n", argv[i]);
            ret = AVERROR(EINVAL);
            goto end;
        }
        *eq = '\0';
        if (!strcmp(argv[i], "vcodec"))
            vcodec_name = eq + 1;
        else if (!strcmp(argv[i], "acodec"))
            acodec_name = eq + 1;
        else if (!strcmp(argv[i], "format"))
            format_name = eq + 1;
        else if (!strcmp(argv[i], "log"))
            c.log = atoi(eq + 1);
        else if ((ret = av_dict_set(&mux_opts, argv[i], eq + 1, 0)) < 0)
            goto end;
    }

    /* 输出上下文。格式由 format= 指定，没有就按输出文件的扩展名猜。 */
    ret = avformat_alloc_output_context2(&c.oc, NULL, format_name, out_name);
    if (!c.oc) {
        fprintf(stderr, "不知道输出文件 %s 是什么格式，请用 format= 指定\n", out_name);
        goto end;
    }
    printf("输出格式：%s（%s）\n", c.oc->oformat->name, c.oc->oformat->long_name);

    /* ---------- 视频：读 Y4M 文件头，找编码器，配置、打开、加一路流 ---------- */
    if (strcmp(vin_name, "none")) {
        int width, height;
        AVRational frame_rate;

        c.vin = fopen(vin_name, "rb");
        if (!c.vin) {
            fprintf(stderr, "打不开输入文件 %s：%s\n", vin_name, strerror(errno));
            ret = AVERROR(errno);
            goto end;
        }
        ret = read_y4m_header(c.vin, &width, &height, &frame_rate);
        if (ret < 0)
            goto end;

        if (vcodec_name)
            vcodec = avcodec_find_encoder_by_name(vcodec_name);
        else if (c.oc->oformat->video_codec != AV_CODEC_ID_NONE)
            vcodec = avcodec_find_encoder(c.oc->oformat->video_codec);
        if (!vcodec || vcodec->type != AVMEDIA_TYPE_VIDEO) {
            fprintf(stderr, "找不到视频编码器%s%s（这种格式没有默认的视频编码器时要用 vcodec= 指定）\n",
                    vcodec_name ? " " : "", vcodec_name ? vcodec_name : "");
            ret = AVERROR_ENCODER_NOT_FOUND;
            goto end;
        }
        if (!supports_yuv420p(vcodec)) {
            fprintf(stderr, "视频编码器 %s 不接受 yuv420p\n", vcodec->name);
            ret = AVERROR(EINVAL);
            goto end;
        }

        venc = avcodec_alloc_context3(vcodec);
        if (!venc) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        venc->width     = width;
        venc->height    = height;
        venc->pix_fmt   = AV_PIX_FMT_YUV420P;
        venc->time_base = av_inv_q(frame_rate);
        venc->framerate = frame_rate;
        venc->sample_aspect_ratio = (AVRational){ 1, 1 };
        /* 帧带着时长，让编码器把它传给包。没有它，MP4 里最后一个包的时长是 0（上游 patch 0011、0065）。 */
        venc->flags |= AV_CODEC_FLAG_FRAME_DURATION;

        c.v.name = "视频";
        ret = add_stream(&c, &c.v, venc, vcodec);
        if (ret < 0)
            goto end;
        c.v.st->avg_frame_rate = frame_rate;

        c.v.frame->format = venc->pix_fmt;
        c.v.frame->width  = width;
        c.v.frame->height = height;
        ret = av_frame_get_buffer(c.v.frame, 0);
        if (ret < 0)
            goto end;
    }

    /* ---------- 音频：读 WAV 文件头，找编码器，配置、打开、加一路流，建立转换器和队列 ---------- */
    if (strcmp(ain_name, "none")) {
        c.ain = fopen(ain_name, "rb");
        if (!c.ain) {
            fprintf(stderr, "打不开输入文件 %s：%s\n", ain_name, strerror(errno));
            ret = AVERROR(errno);
            goto end;
        }
        ret = read_wav_header(c.ain, &c.a_rate, &c.a_channels, &mask);
        if (ret < 0)
            goto end;
        /* 文件里有声道掩码就按掩码，没有就按声道数取默认布局（2 个声道是立体声，6 个是 5.1）。 */
        if (mask && av_popcount64(mask) == c.a_channels)
            ret = av_channel_layout_from_mask(&c.a_layout, mask);
        else
            av_channel_layout_default(&c.a_layout, c.a_channels);
        if (ret < 0)
            goto end;

        if (acodec_name)
            acodec = avcodec_find_encoder_by_name(acodec_name);
        else if (c.oc->oformat->audio_codec != AV_CODEC_ID_NONE)
            acodec = avcodec_find_encoder(c.oc->oformat->audio_codec);
        if (!acodec || acodec->type != AVMEDIA_TYPE_AUDIO) {
            fprintf(stderr, "找不到音频编码器%s%s（这种格式没有默认的音频编码器时要用 acodec= 指定）\n",
                    acodec_name ? " " : "", acodec_name ? acodec_name : "");
            ret = AVERROR_ENCODER_NOT_FOUND;
            goto end;
        }

        aenc = avcodec_alloc_context3(acodec);
        if (!aenc) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        ret = select_sample_format(acodec, &aenc->sample_fmt);
        if (ret < 0)
            goto end;
        aenc->sample_rate = c.a_rate;
        aenc->time_base   = (AVRational){ 1, c.a_rate };
        ret = av_channel_layout_copy(&aenc->ch_layout, &c.a_layout);
        if (ret < 0)
            goto end;

        c.a.name = "音频";
        ret = add_stream(&c, &c.a, aenc, acodec);
        if (ret < 0)
            goto end;

        ret = swr_alloc_set_opts2(&c.swr,
                                  &aenc->ch_layout, aenc->sample_fmt, aenc->sample_rate,
                                  &c.a_layout, AV_SAMPLE_FMT_S16, c.a_rate, 0, NULL);
        if (ret < 0)
            goto end;
        ret = swr_init(c.swr);
        if (ret < 0) {
            fprintf(stderr, "初始化重采样器失败：%s\n", av_err2str(ret));
            goto end;
        }
        c.fifo  = av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, READ_SAMPLES);
        c.a_buf = av_malloc((size_t)READ_SAMPLES * c.a_channels * 2);
        if (!c.fifo || !c.a_buf) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
    }

    /* ---------- 打开输出文件，写文件头 ---------- */
    if (!(c.oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&c.oc->pb, out_name, AVIO_FLAG_WRITE);
        if (ret < 0) {
            fprintf(stderr, "打不开输出文件 %s：%s\n", out_name, av_err2str(ret));
            goto end;
        }
    }
    /*
     * avformat_write_header() 要在所有流都建好、参数都设好之后调用。它写文件头，
     * 并且定下每一路流最终的时间基（复用器可能不接受我们给的）。
     * 复用器的选项（例如 movflags）也在这里传入；返回后字典里剩下的是复用器不认识的。
     */
    ret = avformat_write_header(c.oc, &mux_opts);
    if (ret < 0) {
        fprintf(stderr, "写文件头失败：%s\n", av_err2str(ret));
        goto end;
    }
    if (av_dict_count(mux_opts)) {
        while ((entry = av_dict_iterate(mux_opts, entry)))
            fprintf(stderr, "复用器 %s 不认识选项 %s（值 %s）\n", c.oc->oformat->name, entry->key, entry->value);
        ret = AVERROR_OPTION_NOT_FOUND;
        goto end;
    }
    if (c.v.enc)
        print_stream(&c.v);
    if (c.a.enc)
        print_stream(&c.a);

    /*
     * 主循环：每次让“时间落后”的那一路往前走一步，这样两路的包大致按时间顺序产生。
     * av_compare_ts() 可以比较两个不同时间基下的时间戳。
     */
    while ((c.v.enc && !c.v.done) || (c.a.enc && !c.a.done)) {
        const int video_active = c.v.enc && !c.v.done;
        const int audio_active = c.a.enc && !c.a.done;

        if (video_active &&
            (!audio_active || av_compare_ts(c.v.next_pts, c.v.enc->time_base,
                                            c.a.next_pts, c.a.enc->time_base) <= 0))
            ret = video_step(&c);
        else
            ret = audio_step(&c);
        if (ret < 0)
            goto end;
    }

    /*
     * 写文件尾。MP4 的索引（moov，记录每个包在文件里的位置和时间）是在这里写的，
     * 所以不能省，也不能不检查结果。
     */
    ret = av_write_trailer(c.oc);
    if (ret < 0) {
        fprintf(stderr, "写文件尾失败：%s\n", av_err2str(ret));
        goto end;
    }
    if (!(c.oc->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_closep(&c.oc->pb);
        if (ret < 0) {
            fprintf(stderr, "关闭输出文件失败：%s\n", av_err2str(ret));
            goto end;
        }
    }

    if (c.v.enc)
        printf("视频：送进 %" PRId64 " 帧，写出 %" PRId64 " 个包，共 %" PRId64 " 字节\n",
               c.v.nb_frames, c.v.nb_packets, c.v.bytes);
    if (c.a.enc)
        printf("音频：读入 %" PRId64 " 个采样，送进编码器 %" PRId64 " 个采样（%" PRId64 " 帧），写出 %" PRId64
               " 个包，共 %" PRId64 " 字节\n",
               c.a_in_samples, c.a.next_pts, c.a.nb_frames, c.a.nb_packets, c.a.bytes);
    printf("写出 %s\n", out_name);
    ret = 0;

end:
    /* 出错退出时不补写文件尾，直接释放；输出文件是不完整的。 */
    if (c.oc && !(c.oc->oformat->flags & AVFMT_NOFILE))
        avio_closep(&c.oc->pb);
    avformat_free_context(c.oc);
    if (c.vin)
        fclose(c.vin);
    if (c.ain)
        fclose(c.ain);
    av_frame_free(&c.v.frame);
    av_frame_free(&c.a.frame);
    av_packet_free(&c.v.pkt);
    av_packet_free(&c.a.pkt);
    avcodec_free_context(&venc);
    avcodec_free_context(&aenc);
    av_audio_fifo_free(c.fifo);
    swr_free(&c.swr);
    av_free(c.a_buf);
    av_channel_layout_uninit(&c.a_layout);
    av_dict_free(&mux_opts);
    return ret < 0 ? 1 : 0;
}
