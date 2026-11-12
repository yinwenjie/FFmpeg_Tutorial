/*
 * L07 解封装加解码：从容器文件里同时解码视频和音频。
 *
 * 用法：decode_av <输入文件> <视频输出.yuv> <音频输出.pcm> [trace|noparams]
 *
 * 例子：
 *   decode_av samples/small_360p25.mp4 out/v.yuv out/a.pcm
 *   decode_av samples/small_360p25.mp4 out/v.yuv out/a.pcm trace
 *
 * 视频写成没有文件头的 YUV 文件，音频写成没有文件头、各声道交错的 PCM 文件，
 * 采样的数据类型保持解码器输出的类型（例如 fltp 写成 f32le）。
 * 输入里没有视频或者没有音频时，只处理有的那一路。输出文件名写 - 表示不要这一路。
 *
 * 第四个参数：
 *   trace     打印前 40 个事件（读到的包和解出的帧），用来观察包和帧的先后关系。
 *   noparams  不把容器里的编码参数交给解码器，用来演示这一步为什么不能省。只用于演示。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/common.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

#define TRACE_EVENTS 40

/* 一路流的解码状态。视频和音频各有一个。 */
typedef struct StreamDecoder {
    const char     *label;      /* “视频”或“音频”，打印用 */
    AVStream       *st;         /* 这一路流，NULL 表示不处理 */
    AVCodecContext *dec;
    const char     *filename;   /* 输出文件名 */
    FILE           *file;

    int64_t nb_packets;         /* 送进解码器的包数 */
    int64_t nb_frames;          /* 解出来的帧数 */
    int64_t nb_samples;         /* 音频：每个声道的采样总数 */
    int64_t nb_bytes;           /* 写进文件的字节数 */
    int64_t first_pts, last_pts;/* 第一帧和最后一帧的时间戳，单位是流的时间基 */

    /* 第一帧的参数。输出文件没有文件头，后面的帧必须和第一帧一致。 */
    int                 format; /* 视频是 enum AVPixelFormat，音频是 enum AVSampleFormat */
    int                 width, height;
    int                 sample_rate;
    AVChannelLayout     ch_layout;
} StreamDecoder;

static int trace;               /* 1 表示打印事件 */
static int nb_events;           /* 已经打印的事件数 */

/* 把时间戳换算成秒。没有时间戳时返回 -1。 */
static double ts_seconds(int64_t ts, AVRational time_base)
{
    return ts == AV_NOPTS_VALUE ? -1.0 : ts * av_q2d(time_base);
}

/*
 * 找到一路指定类型的流，为它建立并打开解码器。
 * 返回 0 表示成功；文件里没有这种流时也返回 0，sd->st 保持 NULL；出错返回负数。
 */
static int open_decoder(StreamDecoder *sd, AVFormatContext *fmt, enum AVMediaType type,
                        int noparams)
{
    const AVCodec *codec = NULL;
    AVStream *st;
    int ret;

    /*
     * 第 5 个参数传了 &codec：av_find_best_stream() 在选流的同时把这路流的解码器也找出来。
     * 找不到解码器的流它不会选。这比先选流、再调 avcodec_find_decoder() 少一步。
     */
    ret = av_find_best_stream(fmt, type, -1, -1, &codec, 0);
    if (ret == AVERROR_STREAM_NOT_FOUND) {
        printf("输入里没有%s流\n", sd->label);
        return 0;
    }
    if (ret < 0) {
        fprintf(stderr, "选不出%s流：%s\n", sd->label, av_err2str(ret));
        return ret;
    }
    st = fmt->streams[ret];

    sd->dec = avcodec_alloc_context3(codec);
    if (!sd->dec)
        return AVERROR(ENOMEM);

    /*
     * 把容器里记录的编码参数拷进解码器上下文：宽高、像素格式、采样率、声道布局，以及 extradata。
     * 这一步必须在 avcodec_open2() 之前。noparams 模式故意跳过它。
     */
    if (!noparams) {
        ret = avcodec_parameters_to_context(sd->dec, st->codecpar);
        if (ret < 0) {
            fprintf(stderr, "拷贝%s流的编码参数出错：%s\n", sd->label, av_err2str(ret));
            return ret;
        }
    }

    /*
     * 告诉解码器包的时间戳用的是什么时间基。解码器要丢掉开头的若干采样时（MP4 里的 AAC），
     * 靠它来修正帧的时间戳。解出来的帧的 pts 用的也是这个时间基。
     */
    sd->dec->pkt_timebase = st->time_base;

    ret = avcodec_open2(sd->dec, codec, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开%s解码器 %s：%s\n", sd->label, codec->name, av_err2str(ret));
        return ret;
    }

    sd->st = st;
    sd->first_pts = sd->last_pts = AV_NOPTS_VALUE;
    return 0;
}

/*
 * 写一帧图像。一行一行地写，每行只写有效的字节，不写 linesize 里多出来的填充（L05）。
 * av_image_get_linesize() 算出某个平面一行有多少个有效字节，对 8 位和 10 位、
 * 平面格式和打包格式都适用。色度平面的行数按像素格式描述里的 log2_chroma_h 计算。
 */
static int write_video_frame(StreamDecoder *sd, const AVFrame *frame)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);

    for (int p = 0; p < 4 && frame->data[p]; p++) {
        const int bytes = av_image_get_linesize(frame->format, frame->width, p);
        const int rows  = (p == 1 || p == 2) ? AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h)
                                             : frame->height;

        if (bytes < 0)
            return bytes;
        for (int y = 0; y < rows; y++) {
            if (fwrite(frame->data[p] + y * frame->linesize[p], 1, bytes, sd->file) != (size_t)bytes)
                return AVERROR(EIO);
        }
        sd->nb_bytes += (int64_t)bytes * rows;
    }
    return 0;
}

/*
 * 写一帧音频。文件里要的是交错排列（L06）：
 * packed 格式的第一个平面已经是交错的，整块写出；planar 格式要逐个采样、逐个声道地取。
 * 数据类型不转换。要转换采样格式、采样率或声道数，用 libswresample（L10）。
 */
static int write_audio_frame(StreamDecoder *sd, const AVFrame *frame)
{
    const int    bytes    = av_get_bytes_per_sample(frame->format);
    const int    channels = frame->ch_layout.nb_channels;
    const size_t total    = (size_t)frame->nb_samples * channels * bytes;

    if (av_sample_fmt_is_planar(frame->format)) {
        for (int i = 0; i < frame->nb_samples; i++)
            for (int ch = 0; ch < channels; ch++)
                fwrite(frame->extended_data[ch] + i * bytes, 1, bytes, sd->file);
    } else {
        fwrite(frame->extended_data[0], 1, total, sd->file);
    }
    if (ferror(sd->file))
        return AVERROR(EIO);

    sd->nb_bytes += total;
    return 0;
}

/* 处理解码出来的一帧：检查参数，统计，写文件。 */
static int process_frame(StreamDecoder *sd, const AVFrame *frame)
{
    const int is_video = sd->dec->codec_type == AVMEDIA_TYPE_VIDEO;
    /*
     * 帧的时间戳。best_effort_timestamp 在 pts 有效时就是 pts；
     * 有的容器（例如 AVI）不记录 pts，这时 FFmpeg 根据 dts 等信息估计一个值放在这里。
     */
    const int64_t pts = frame->best_effort_timestamp;
    int ret;

    if (sd->nb_frames == 0) {
        /* 第一帧：记下参数。参数要从帧上取，帧上的值才是解码器实际输出的。 */
        sd->format = frame->format;
        if (is_video) {
            const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(frame->format);

            if (!desc || (desc->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL))) {
                fprintf(stderr, "本程序不处理像素格式 %s\n", desc ? desc->name : "未知格式");
                return AVERROR(ENOSYS);
            }
            sd->width  = frame->width;
            sd->height = frame->height;
        } else {
            sd->sample_rate = frame->sample_rate;
            ret = av_channel_layout_copy(&sd->ch_layout, &frame->ch_layout);
            if (ret < 0)
                return ret;
        }
        sd->first_pts = pts;
    } else if (frame->format != sd->format ||
               (is_video  && (frame->width != sd->width || frame->height != sd->height)) ||
               (!is_video && (frame->sample_rate != sd->sample_rate ||
                              av_channel_layout_compare(&frame->ch_layout, &sd->ch_layout)))) {
        fprintf(stderr, "%s流的参数中途发生了变化，没有文件头的输出文件无法表示这种情况\n", sd->label);
        return AVERROR(ENOSYS);
    }

    if (trace && nb_events < TRACE_EVENTS) {
        nb_events++;
        if (is_video)
            printf("        → %s帧 %3" PRId64 "  pts %6" PRId64 " = %.3f 秒  %c 帧\n", sd->label,
                   sd->nb_frames, pts, ts_seconds(pts, sd->st->time_base),
                   av_get_picture_type_char(frame->pict_type));
        else
            printf("        → %s帧 %3" PRId64 "  pts %6" PRId64 " = %.3f 秒  %d 个采样\n", sd->label,
                   sd->nb_frames, pts, ts_seconds(pts, sd->st->time_base), frame->nb_samples);
    }

    if (sd->file) {
        ret = is_video ? write_video_frame(sd, frame) : write_audio_frame(sd, frame);
        if (ret < 0) {
            fprintf(stderr, "写 %s 出错\n", sd->filename);
            return ret;
        }
    }

    sd->last_pts = pts;
    sd->nb_frames++;
    sd->nb_samples += frame->nb_samples;   /* 视频帧的 nb_samples 是 0 */
    return 0;
}

/* 送一个包（NULL 表示冲刷），再把能取的帧全部取出来。和 L05、L06 的写法相同。 */
static int decode(StreamDecoder *sd, const AVPacket *pkt, AVFrame *frame)
{
    int ret = avcodec_send_packet(sd->dec, pkt);

    if (ret < 0) {
        fprintf(stderr, "%s送包出错：%s\n", sd->label, av_err2str(ret));
        return ret;
    }
    if (pkt)
        sd->nb_packets++;

    for (;;) {
        ret = avcodec_receive_frame(sd->dec, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "%s解码出错：%s\n", sd->label, av_err2str(ret));
            return ret;
        }

        ret = process_frame(sd, frame);
        /* 帧用完了，解除引用。下一次 avcodec_receive_frame() 也会先做这件事，这里写出来是为了明确。 */
        av_frame_unref(frame);
        if (ret < 0)
            return ret;
    }
}

/* 打开输出文件。文件名是 - 时不打开，这一路只解码、不写文件。 */
static int open_output(StreamDecoder *sd)
{
    if (!sd->st || !strcmp(sd->filename, "-"))
        return 0;
    sd->file = fopen(sd->filename, "wb");
    if (!sd->file) {
        int ret = AVERROR(errno);

        fprintf(stderr, "打不开 %s：%s\n", sd->filename, av_err2str(ret));
        return ret;
    }
    return 0;
}

/* 关闭输出文件。缓冲区里的数据这时才真正写盘，所以要检查返回值。 */
static int close_output(StreamDecoder *sd)
{
    if (sd->file) {
        int ret = fclose(sd->file);

        sd->file = NULL;
        if (ret) {
            fprintf(stderr, "写 %s 出错\n", sd->filename);
            return AVERROR(EIO);
        }
    }
    return 0;
}

/* 打印一路流的统计和播放命令。 */
static void print_summary(const StreamDecoder *sd)
{
    const int is_video = sd->dec->codec_type == AVMEDIA_TYPE_VIDEO;

    printf("%s：流 #%d，解码器 %s，%" PRId64 " 个包，%" PRId64 " 帧", sd->label, sd->st->index,
           sd->dec->codec->name, sd->nb_packets, sd->nb_frames);
    if (!sd->nb_frames) {
        printf("\n");
        return;
    }
    if (!is_video)
        printf("，每个声道 %" PRId64 " 个采样", sd->nb_samples);
    printf("，第一帧 %.3f 秒，最后一帧 %.3f 秒\n",
           ts_seconds(sd->first_pts, sd->st->time_base),
           ts_seconds(sd->last_pts, sd->st->time_base));
    if (strcmp(sd->filename, "-"))
        printf("  写出 %" PRId64 " 字节到 %s\n", sd->nb_bytes, sd->filename);

    if (is_video) {
        /* 帧率用容器给出的平均帧率。它只用来生成播放命令。 */
        AVRational fr = sd->st->avg_frame_rate.num && sd->st->avg_frame_rate.den
                        ? sd->st->avg_frame_rate : (AVRational){ 25, 1 };

        printf("  %s %dx%d\n", av_get_pix_fmt_name(sd->format), sd->width, sd->height);
        if (strcmp(sd->filename, "-"))
            printf("  ffplay -f rawvideo -pixel_format %s -video_size %dx%d -framerate %d/%d %s\n",
                   av_get_pix_fmt_name(sd->format), sd->width, sd->height, fr.num, fr.den,
                   sd->filename);
    } else {
        /* PCM 文件的格式名：数据类型加字节序。本课程只在小端机器上运行，直接写 le。 */
        static const struct { enum AVSampleFormat fmt; const char *name; } names[] = {
            { AV_SAMPLE_FMT_U8,  "u8"    }, { AV_SAMPLE_FMT_S16, "s16le" },
            { AV_SAMPLE_FMT_S32, "s32le" }, { AV_SAMPLE_FMT_FLT, "f32le" },
            { AV_SAMPLE_FMT_DBL, "f64le" },
        };
        const enum AVSampleFormat packed = av_get_packed_sample_fmt(sd->format);
        const char *name = NULL;
        char layout[64];

        for (size_t i = 0; i < FF_ARRAY_ELEMS(names); i++)
            if (names[i].fmt == packed)
                name = names[i].name;
        av_channel_layout_describe(&sd->ch_layout, layout, sizeof(layout));
        printf("  %s，%d Hz，%s\n", av_get_sample_fmt_name(sd->format), sd->sample_rate, layout);
        if (name && strcmp(sd->filename, "-"))
            printf("  ffplay -f %s -ch_layout %s -sample_rate %d %s\n", name, layout,
                   sd->sample_rate, sd->filename);
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVPacket *pkt = NULL;
    AVFrame *frame = NULL;
    StreamDecoder video = { .label = "视频" }, audio = { .label = "音频" };
    int noparams = 0, ret;

    if (argc < 4 || argc > 5 ||
        (argc == 5 && strcmp(argv[4], "trace") && strcmp(argv[4], "noparams"))) {
        fprintf(stderr, "用法：%s <输入文件> <视频输出.yuv> <音频输出.pcm> [trace|noparams]\n"
                "输出文件名写 - 表示不要这一路\n", argv[0]);
        return 1;
    }
    video.filename = argv[2];
    audio.filename = argv[3];
    if (argc == 5) {
        trace    = !strcmp(argv[4], "trace");
        noparams = !strcmp(argv[4], "noparams");
    }

    /* 第 1 步：打开容器，补全流参数（L03）。 */
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

    /* 第 2 步：为视频流和音频流各建立一个解码器。 */
    if ((ret = open_decoder(&video, fmt, AVMEDIA_TYPE_VIDEO, noparams)) < 0 ||
        (ret = open_decoder(&audio, fmt, AVMEDIA_TYPE_AUDIO, noparams)) < 0)
        goto end;
    if (!video.st && !audio.st) {
        fprintf(stderr, "%s 里既没有视频流也没有音频流\n", argv[1]);
        ret = AVERROR_STREAM_NOT_FOUND;
        goto end;
    }

    /*
     * 其他流的包用不着。把它们标成 AVDISCARD_ALL，解封装器就不再把这些包交出来，
     * 有的容器还能因此少读一些数据。
     */
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        if (fmt->streams[i] != video.st && fmt->streams[i] != audio.st)
            fmt->streams[i]->discard = AVDISCARD_ALL;

    if ((ret = open_output(&video)) < 0 || (ret = open_output(&audio)) < 0)
        goto end;

    pkt   = av_packet_alloc();
    frame = av_frame_alloc();
    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /* 第 3 步：读包，按 stream_index 分给对应的解码器。 */
    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        StreamDecoder *sd = video.st && pkt->stream_index == video.st->index ? &video :
                            audio.st && pkt->stream_index == audio.st->index ? &audio : NULL;

        if (sd) {
            if (trace && nb_events < TRACE_EVENTS) {
                nb_events++;
                printf("读到%s包  dts %6" PRId64 "  pts %6" PRId64 " = %.3f 秒  %5d 字节%s\n",
                       sd->label, pkt->dts, pkt->pts, ts_seconds(pkt->pts, sd->st->time_base),
                       pkt->size, pkt->flags & AV_PKT_FLAG_KEY ? "  关键帧" : "");
            }
            ret = decode(sd, pkt, frame);
        }
        /* 不管这个包有没有用到，都要解除引用，否则包里的数据一直占着内存。 */
        av_packet_unref(pkt);
        if (ret < 0)
            goto end;
    }
    /* av_read_frame() 读到文件末尾时返回 AVERROR_EOF，其他负数是真的出错了。 */
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));
        goto end;
    }

    /* 第 4 步：冲刷两个解码器，取出留在里面的帧。 */
    if (trace && nb_events < TRACE_EVENTS)
        printf("文件读完，开始冲刷\n");
    if ((video.st && (ret = decode(&video, NULL, frame)) < 0) ||
        (audio.st && (ret = decode(&audio, NULL, frame)) < 0))
        goto end;

    if ((ret = close_output(&video)) < 0 || (ret = close_output(&audio)) < 0)
        goto end;

    if (trace)
        printf("（只打印前 %d 个事件）\n\n", TRACE_EVENTS);
    if (video.st)
        print_summary(&video);
    if (audio.st)
        print_summary(&audio);
    ret = 0;

end:
    if (video.file)
        fclose(video.file);
    if (audio.file)
        fclose(audio.file);
    av_channel_layout_uninit(&audio.ch_layout);
    avcodec_free_context(&video.dec);
    avcodec_free_context(&audio.dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}
