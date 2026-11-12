/*
 * Copyright (c) 2012 Stefano Sabatini
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
 * @file libavformat and libavcodec demuxing and decoding API usage example
 * @example demux_decode.c
 *
 * Show how to use the libavformat and libavcodec API to demux and decode audio
 * and video data. Write the output as raw audio and input files to be played by
 * ffplay.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/demux_decode.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它把 L03 的解封装和 L05、L06 的解码接在一起：用 libavformat 打开一个容器文件，
 * 找出视频流和音频流，各开一个解码器，把读到的包按流分给对应的解码器，
 * 解出来的图像写成一个 YUV 文件，采样写成一个 PCM 文件。
 * 读它要抓住三件事：open_codec_context() 怎样用容器里的参数建立解码器，
 * main() 里读包、分发、冲刷的循环，以及两种帧各自怎样写进文件。
 *
 * 这个示例有几处问题，已经整理成 patch 放在仓库的 upstream_patches/ 目录
 * （0008、0025 到 0030），下面在出问题的位置都做了标注。
 * 自己写程序时参考本目录的 decode_av.c。
 */

#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/timestamp.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/*
 * 课程注释：示例把所有状态都放在全局变量里，函数之间不用传参数，代码短一些。
 * 一个程序里只处理一个文件时这样写没有问题。工程代码通常把它们放进一个结构体，
 * 这样同一个进程里才能同时处理多个文件。
 */
static AVFormatContext *fmt_ctx = NULL;
static AVCodecContext *video_dec_ctx = NULL, *audio_dec_ctx;
static int width, height;
static enum AVPixelFormat pix_fmt;
static AVStream *video_stream = NULL, *audio_stream = NULL;
static const char *src_filename = NULL;
static const char *video_dst_filename = NULL;
static const char *audio_dst_filename = NULL;
static FILE *video_dst_file = NULL;
static FILE *audio_dst_file = NULL;

/*
 * 课程注释：一块自己申请的图像缓冲区。解码出来的每一帧先拷到这里，再写进文件。
 * 为什么要多拷一次，见 output_video_frame()。
 */
static uint8_t *video_dst_data[4] = {NULL};
static int      video_dst_linesize[4];
static int video_dst_bufsize;

static int video_stream_idx = -1, audio_stream_idx = -1;
static AVFrame *frame = NULL;
static AVPacket *pkt = NULL;
static int video_frame_count = 0;
static int audio_frame_count = 0;

static int output_video_frame(AVFrame *frame)
{
    /*
     * 课程注释：YUV 文件没有文件头，里面所有的帧必须一样大、格式一样。
     * 码流中途改变分辨率是允许的（直播切换清晰度时就会发生），示例遇到这种情况直接报错退出。
     */
    if (frame->width != width || frame->height != height ||
        frame->format != pix_fmt) {
        /* To handle this change, one could call av_image_alloc again and
         * decode the following frames into another rawvideo file. */
        fprintf(stderr, "Error: Width, height and pixel format have to be "
                "constant in a rawvideo file, but the width, height or "
                "pixel format of the input video changed:\n"
                "old: width = %d, height = %d, format = %s\n"
                "new: width = %d, height = %d, format = %s\n",
                width, height, av_get_pix_fmt_name(pix_fmt),
                frame->width, frame->height,
                av_get_pix_fmt_name(frame->format));
        return -1;
    }

    printf("video_frame n:%d\n",
           video_frame_count++);

    /*
     * 课程注释：L05 讲过，解码器给的帧每一行后面可能有填充，linesize 可能比宽度大，
     * 不能把 data[0] 开始的一整块直接写进文件。L05 的做法是一行一行地写。
     * 这里是另一种做法：main() 里用 av_image_alloc() 申请缓冲区时对齐参数给的是 1，
     * 所以这块缓冲区没有填充，而且三个平面紧挨着放在同一块内存里。
     * av_image_copy2() 按宽和高把每个平面逐行拷过去，丢掉填充。
     * 拷完之后从 video_dst_data[0] 开始的 video_dst_bufsize 个字节正好就是一帧，可以一次写出。
     */
    /* copy decoded frame to destination buffer:
     * this is required since rawvideo expects non aligned data */
    av_image_copy2(video_dst_data, video_dst_linesize,
                   frame->data, frame->linesize,
                   pix_fmt, width, height);

    /* write to rawvideo file */
    /* 课程注释：没有检查 fwrite 的返回值，磁盘满了也不知道（patch 0029）。 */
    fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
    return 0;
}

static int output_audio_frame(AVFrame *frame)
{
    /*
     * 课程注释：这一行算的是“一个声道的数据有多少字节”。
     * 对 planar 格式，它正好是第一个平面的大小，写出去的是第一个声道，下面的英文注释说明了这一点。
     * 对 packed 格式，第一个平面里有 nb_samples × 声道数 个采样，这里少乘了声道数：
     * 双声道时每一帧只写出前一半，后一半丢了。下面的注释说 packed 格式“works fine”，其实不对（patch 0028）。
     * 正确的写法见 L06 的 decode_wav.c 和本目录的 decode_av.c。
     */
    size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample(frame->format);
    printf("audio_frame n:%d nb_samples:%d pts:%s\n",
           audio_frame_count++, frame->nb_samples,
           /*
            * 课程注释：frame->pts 的单位是流的时间基（AVStream.time_base），这里却用解码器上下文的 time_base 去换算。
            * 这个字段在音频解码器里恰好是采样率的倒数。MP4 的音频流时间基也是采样率的倒数，所以碰巧打印对了；
            * 换成 MPEG-TS（时间基 1/90000），打印出来的时间就是错的（patch 0027）。
            */
           av_ts2timestr(frame->pts, &audio_dec_ctx->time_base));

    /* Write the raw audio data samples of the first plane. This works
     * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
     * most audio decoders output planar audio, which uses a separate
     * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
     * In other words, this code will write only the first audio channel
     * in these cases.
     * You should use libswresample or libavfilter to convert the frame
     * to packed data. */
    fwrite(frame->extended_data[0], 1, unpadded_linesize, audio_dst_file);

    return 0;
}

/*
 * 课程注释：送一个包，取出所有能取的帧。写法和 L05、L06 完全一样，
 * 区别只在于它被两个解码器共用：dec 是哪个解码器，帧就交给哪个输出函数。
 * pkt 为 NULL 时是冲刷。
 */
static int decode_packet(AVCodecContext *dec, const AVPacket *pkt)
{
    int ret = 0;

    // submit the packet to the decoder
    ret = avcodec_send_packet(dec, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error submitting a packet for decoding (%s)\n", av_err2str(ret));
        return ret;
    }

    // get all the available frames from the decoder
    while (ret >= 0) {
        ret = avcodec_receive_frame(dec, frame);
        if (ret < 0) {
            // those two return values are special and mean there is no output
            // frame available, but there were no errors during decoding
            if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                return 0;

            fprintf(stderr, "Error during decoding (%s)\n", av_err2str(ret));
            return ret;
        }

        // write the frame data to output file
        if (dec->codec->type == AVMEDIA_TYPE_VIDEO)
            ret = output_video_frame(frame);
        else
            ret = output_audio_frame(frame);

        /*
         * 课程注释：frame 是全局的一个 AVFrame，反复使用。每用完一帧就解除它对数据的引用。
         * 其实不写这一句也可以，avcodec_receive_frame() 在填入新的一帧之前会自己先做一次。
         */
        av_frame_unref(frame);
    }

    return ret;
}

static int open_codec_context(int *stream_idx,
                              AVCodecContext **dec_ctx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
    int ret, stream_index;
    AVStream *st;
    const AVCodec *dec = NULL;

    /*
     * 课程注释：L03 用过的函数。在文件里挑一路指定类型的流，返回它的序号；
     * 没有这种类型的流时返回负数。一个文件有多路音频时，它挑的是标了默认标志或者质量最高的那一路。
     */
    ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Could not find %s stream in input file '%s'\n",
                av_get_media_type_string(type), src_filename);
        return ret;
    } else {
        stream_index = ret;
        st = fmt_ctx->streams[stream_index];

        /* find decoder for the stream */
        /*
         * 课程注释：L05、L06 的示例把解码器写死在代码里（MPEG1VIDEO、MP2）。
         * 这里按流的编码格式去找：容器里记着这路流是什么编码（codecpar->codec_id），
         * avcodec_find_decoder() 返回这种编码的默认解码器。找不到说明编译 FFmpeg 时没有带上它。
         */
        dec = avcodec_find_decoder(st->codecpar->codec_id);
        if (!dec) {
            fprintf(stderr, "Failed to find %s codec\n",
                    av_get_media_type_string(type));
            return AVERROR(EINVAL);
        }

        /* Allocate a codec context for the decoder */
        *dec_ctx = avcodec_alloc_context3(dec);
        if (!*dec_ctx) {
            fprintf(stderr, "Failed to allocate the %s codec context\n",
                    av_get_media_type_string(type));
            return AVERROR(ENOMEM);
        }

        /*
         * 课程注释：这是这一讲最重要的一句。
         * 容器把每一路流的编码参数记在 AVStream.codecpar 里：宽高、像素格式、采样率、声道布局，
         * 还有一块叫 extradata 的数据。MP4 里的 H.264 把 SPS、PPS 放在 extradata 里，包里面没有；
         * MP4 里的 AAC 把采样率、声道配置放在 extradata 里，包前面没有 ADTS 头。
         * 不把这些参数交给解码器，它拿到包也解不出来。avcodec_parameters_to_context() 把它们整个拷进解码器上下文。
         * 必须在 avcodec_open2() 之前调用。
         */
        /* Copy codec parameters from input stream to output codec context */
        if ((ret = avcodec_parameters_to_context(*dec_ctx, st->codecpar)) < 0) {
            fprintf(stderr, "Failed to copy %s codec parameters to decoder context\n",
                    av_get_media_type_string(type));
            return ret;
        }

        /*
         * 课程注释：这里少了一句 (*dec_ctx)->pkt_timebase = st->time_base（patch 0008）。
         * 它告诉解码器包的时间戳用的是什么时间基。不设置的话，解码 MP4 里的 AAC 时会有一条
         * “Could not update timestamps for discarded samples”的警告。
         */
        /* Init the decoders */
        if ((ret = avcodec_open2(*dec_ctx, dec, NULL)) < 0) {
            fprintf(stderr, "Failed to open %s codec\n",
                    av_get_media_type_string(type));
            return ret;
        }
        *stream_idx = stream_index;
    }

    return 0;
}

/*
 * 课程注释：把采样格式换成 ffplay 的 -f 参数要用的名字，和 L06 的 decode_audio.c 里是同一个函数。
 * 脱离源码树、带 -Wextra 编译时，里面的 for 循环有一个符号比较的警告，不影响运行。
 */
static int get_format_from_sample_fmt(const char **fmt,
                                      enum AVSampleFormat sample_fmt)
{
    int i;
    struct sample_fmt_entry {
        enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
    } sample_fmt_entries[] = {
        { AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
        { AV_SAMPLE_FMT_S16, "s16be", "s16le" },
        { AV_SAMPLE_FMT_S32, "s32be", "s32le" },
        { AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
        { AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
    };
    *fmt = NULL;

    for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
        struct sample_fmt_entry *entry = &sample_fmt_entries[i];
        if (sample_fmt == entry->sample_fmt) {
            *fmt = AV_NE(entry->fmt_be, entry->fmt_le);
            return 0;
        }
    }

    fprintf(stderr,
            "sample format %s is not supported as output format\n",
            av_get_sample_fmt_name(sample_fmt));
    return -1;
}

int main (int argc, char **argv)
{
    int ret = 0;

    if (argc != 4) {
        fprintf(stderr, "usage: %s  input_file video_output_file audio_output_file\n"
                "API example program to show how to read frames from an input file.\n"
                "This program reads frames from a file, decodes them, and writes decoded\n"
                "video frames to a rawvideo file named video_output_file, and decoded\n"
                "audio frames to a rawaudio file named audio_output_file.\n",
                argv[0]);
        exit(1);
    }
    src_filename = argv[1];
    video_dst_filename = argv[2];
    audio_dst_filename = argv[3];

    /* 课程注释：下面两步是 L03 的内容：打开文件，读一段数据把每路流的参数补全。 */
    /* open input file, and allocate format context */
    if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0) {
        fprintf(stderr, "Could not open source file %s\n", src_filename);
        exit(1);
    }

    /* retrieve stream information */
    if (avformat_find_stream_info(fmt_ctx, NULL) < 0) {
        fprintf(stderr, "Could not find stream information\n");
        exit(1);
    }

    if (open_codec_context(&video_stream_idx, &video_dec_ctx, fmt_ctx, AVMEDIA_TYPE_VIDEO) >= 0) {
        video_stream = fmt_ctx->streams[video_stream_idx];

        video_dst_file = fopen(video_dst_filename, "wb");
        if (!video_dst_file) {
            fprintf(stderr, "Could not open destination file %s\n", video_dst_filename);
            /*
             * 课程注释：这里把 ret 设成 1，而 main() 最后一句是 return ret < 0。
             * 1 不小于 0，所以输出文件打不开时，程序打印了错误信息，退出码却是 0。
             * 下面还有两处同样的写法（patch 0025）。
             */
            ret = 1;
            goto end;
        }

        /* allocate image where the decoded image will be put */
        width = video_dec_ctx->width;
        height = video_dec_ctx->height;
        pix_fmt = video_dec_ctx->pix_fmt;
        /*
         * 课程注释：按宽、高、像素格式申请一块图像缓冲区。最后一个参数 1 是对齐，1 表示每一行后面不留填充。
         * 返回值是缓冲区的总字节数，640x360 的 yuv420p 是 345600。
         * 宽高和像素格式取自解码器上下文，它们是 avcodec_parameters_to_context() 从容器拷过来的。
         */
        ret = av_image_alloc(video_dst_data, video_dst_linesize,
                             width, height, pix_fmt, 1);
        if (ret < 0) {
            fprintf(stderr, "Could not allocate raw video buffer\n");
            goto end;
        }
        video_dst_bufsize = ret;
    }

    if (open_codec_context(&audio_stream_idx, &audio_dec_ctx, fmt_ctx, AVMEDIA_TYPE_AUDIO) >= 0) {
        audio_stream = fmt_ctx->streams[audio_stream_idx];
        audio_dst_file = fopen(audio_dst_filename, "wb");
        if (!audio_dst_file) {
            fprintf(stderr, "Could not open destination file %s\n", audio_dst_filename);
            ret = 1;
            goto end;
        }
    }

    /* 课程注释：L03 用过，把文件的概况打印到标准错误，和 ffprobe 开头那一段相同。 */
    /* dump input information to stderr */
    av_dump_format(fmt_ctx, 0, src_filename, 0);

    if (!audio_stream && !video_stream) {
        fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
        ret = 1;
        goto end;
    }

    frame = av_frame_alloc();
    if (!frame) {
        fprintf(stderr, "Could not allocate frame\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    pkt = av_packet_alloc();
    if (!pkt) {
        fprintf(stderr, "Could not allocate packet\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (video_stream)
        printf("Demuxing video from file '%s' into '%s'\n", src_filename, video_dst_filename);
    if (audio_stream)
        printf("Demuxing audio from file '%s' into '%s'\n", src_filename, audio_dst_filename);

    /*
     * 课程注释：主循环。av_read_frame() 每次从容器里读出一个包，包里的 stream_index 说明它属于哪一路流。
     * 视频的包交给视频解码器，音频的包交给音频解码器，其他流（字幕、数据）的包不管。
     * 不管这个包用没用，都要 av_packet_unref()，否则包里的数据不会释放。
     * 容器里视频包和音频包是交替存放的，所以两个解码器是轮流工作的。
     *
     * 这个循环有两个问题（patch 0026）。一是 av_read_frame() 返回负数就当作读完了，
     * 不区分正常结束（AVERROR_EOF）和读文件出错。二是解码出错时只是 break，
     * 后面照样冲刷、照样打印“Demuxing succeeded.”，ret 还会被后面的调用覆盖，退出码是 0。
     */
    /* read frames from the file */
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        // check if the packet belongs to a stream we are interested in, otherwise
        // skip it
        if (pkt->stream_index == video_stream_idx)
            ret = decode_packet(video_dec_ctx, pkt);
        else if (pkt->stream_index == audio_stream_idx)
            ret = decode_packet(audio_dec_ctx, pkt);
        av_packet_unref(pkt);
        if (ret < 0)
            break;
    }

    /* 课程注释：包读完之后，两个解码器各冲刷一次，取出留在里面的帧。H.264 不冲刷会少最后几帧。 */
    /* flush the decoders */
    if (video_dec_ctx)
        decode_packet(video_dec_ctx, NULL);
    if (audio_dec_ctx)
        decode_packet(audio_dec_ctx, NULL);

    printf("Demuxing succeeded.\n");

    if (video_stream) {
        printf("Play the output video file with the command:\n"
               "ffplay -f rawvideo -pixel_format %s -video_size %dx%d %s\n",
               av_get_pix_fmt_name(pix_fmt), width, height,
               video_dst_filename);
    }

    if (audio_stream) {
        enum AVSampleFormat sfmt = audio_dec_ctx->sample_fmt;
        AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
        AVChannelLayout *ch_layout = &audio_dec_ctx->ch_layout;
        const char *fmt;
        char buf[64];

        /*
         * 课程注释：planar 格式只写了第一个声道，所以这里把播放命令里的格式改成对应的 packed 格式，
         * 声道布局改成单声道。双声道的文件，解出来只剩左声道。
         */
        if (av_sample_fmt_is_planar(sfmt)) {
            const char *packed = av_get_sample_fmt_name(sfmt);
            printf("Warning: the sample format the decoder produced is planar "
                   "(%s). This example will output the first channel only.\n",
                   packed ? packed : "?");
            sfmt = av_get_packed_sample_fmt(sfmt);
            ch_layout = &mono;
        }

        if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
            goto end;

        if ((ret = av_channel_layout_describe(ch_layout, buf, sizeof(buf))) < 0)
            goto end;

        printf("Play the output audio file with the command:\n"
               "ffplay -f %s -ch_layout %s -sample_rate %d %s\n",
               fmt, buf, audio_dec_ctx->sample_rate,
               audio_dst_filename);
    }

end:
    avcodec_free_context(&video_dec_ctx);
    avcodec_free_context(&audio_dec_ctx);
    avformat_close_input(&fmt_ctx);
    if (video_dst_file)
        fclose(video_dst_file);
    if (audio_dst_file)
        fclose(audio_dst_file);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_free(video_dst_data[0]);

    /* 课程注释：出错（ret 为负）时退出码是 1，否则是 0。前面把 ret 设成 1 的那几处因此返回了 0。 */
    return ret < 0;
}
