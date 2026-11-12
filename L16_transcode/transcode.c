/*
 * Copyright (c) 2010 Nicolas George
 * Copyright (c) 2011 Stefano Sabatini
 * Copyright (c) 2014 Andrey Utkin
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
 * @file demuxing, decoding, filtering, encoding and muxing API usage example
 * @example transcode.c
 *
 * Convert input to output file, applying some hard-coded filter-graph on both
 * audio and video streams.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/transcode.c 的拷贝，代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它把输入文件的每一路音视频流解码、过一个滤镜图、再用同样的编码器编码，写进输出文件。
 * 注意：拷贝的不是提交 a337e8d 的原版，而是打了 11 个 patch（0001、0007、0011、0014、0071 到 0077）的版本。
 * 原版一运行就失败（见 upstream_patches/ 的 0001），L02 说过“L16 给出修正后的版本”就是这一份。
 * 改动过的地方在下面都做了标注，写着对应的 patch 编号。
 * 程序结构（L07 到 L15 的东西在这里合到了一起）：
 *     open_input_file()   打开输入，给每一路流开解码器（L03、L05、L06）
 *     open_output_file()  创建输出，给每一路流建编码器和输出流（L13 到 L15）
 *     init_filters()      给每一路流建滤镜图（L11、L12）
 *     main()              读包 → 解码 → 滤镜 → 编码 → 写包，最后冲刷（L07、L12、L15）
 * 自己写程序时参考本目录的 transcode_av.c。
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

/* 课程注释：全局变量：输入、输出的 AVFormatContext。官方示例为了篇幅短用了全局变量，自己的程序最好放进一个结构体里（L22）。 */
static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;
/*
 * 课程注释：每一路流的滤镜图相关对象：入口（buffersrc）、出口（buffersink）、滤镜图本身，
 * 还有从编码器取包用的 enc_pkt、从滤镜图取帧用的 filtered_frame，每一路各有一份，循环里重复使用。
 */
typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;

    AVPacket *enc_pkt;
    AVFrame *filtered_frame;
} FilteringContext;
static FilteringContext *filter_ctx;

/* 课程注释：每一路流的解码器、编码器和解码出来的帧。 */
typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;

    AVFrame *dec_frame;

    /* state of av_rescale_delta() for the audio timestamps */
    int64_t audio_pts_last;
} StreamContext;
static StreamContext *stream_ctx;

/*
 * 课程注释：打开输入文件，给每一路流开解码器。和 L07 的 demux_decode.c 的区别是这里处理所有的流，不只是第一路视频和音频。
 * 下标 i 就是输入流的序号，后面 stream_ctx[i]、filter_ctx[i]、输出的第 i 路流都和它一一对应。
 */
static int open_input_file(const char *filename)
{
    int ret;
    unsigned int i;

    ifmt_ctx = NULL;
    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    stream_ctx = av_calloc(ifmt_ctx->nb_streams, sizeof(*stream_ctx));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream = ifmt_ctx->streams[i];
        const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            av_log(NULL, AV_LOG_ERROR, "Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy decoder parameters to input decoder context "
                   "for stream #%u\n", i);
            return ret;
        }

        /* 课程注释：把输入流的时间基告诉解码器（L12 讲过）。解码出来的帧以及滤镜图入口的时间基都依赖它。 */
        /* Inform the decoder about the timebase for the packet timestamps.
         * This is highly recommended, but not mandatory. */
        codec_ctx->pkt_timebase = stream->time_base;

        /* Reencode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* 课程注释：视频要记下帧率（av_guess_frame_rate 综合平均帧率和时间基猜一个），后面编码器的时间基是它的倒数。 */
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, NULL);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
        stream_ctx[i].dec_ctx = codec_ctx;

        stream_ctx[i].dec_frame = av_frame_alloc();
        if (!stream_ctx[i].dec_frame)
            return AVERROR(ENOMEM);
        stream_ctx[i].audio_pts_last = AV_NOPTS_VALUE;
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

/*
 * 课程注释：创建输出文件，给每一路流建编码器和输出流。这一部分和 L15 的 mux_av.c 是同样的套路，
 * 区别是编码器的参数不是自己定的，而是照着解码器抄。
 */
static int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    const AVCodec *encoder;
    int ret;
    unsigned int i;

    ofmt_ctx = NULL;
    /* 课程注释：按输出文件的扩展名猜容器格式。猜不出来（扩展名不认识）时返回错误，要把这个错误码原样传回去：patch 0014。 */
    ret = avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Could not create output context\n");
        return ret;
    }

    /*
     * 课程注释：拷贝容器的元数据（标题等）。原版没有这一段，patch 0075 补上（remux.c 的 0013 是同样的问题）。
     * 课程的 transcode_av.c 还去掉了 MP4 的 major_brand 等三个只描述输入文件本身的标签。
     */
    ret = av_dict_copy(&ofmt_ctx->metadata, ifmt_ctx->metadata, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Failed to copy metadata\n");
        return ret;
    }

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        out_stream = avformat_new_stream(ofmt_ctx, NULL);
        if (!out_stream) {
            av_log(NULL, AV_LOG_ERROR, "Failed allocating output stream\n");
            return AVERROR(ENOMEM);
        }

        in_stream = ifmt_ctx->streams[i];
        dec_ctx = stream_ctx[i].dec_ctx;

        ret = av_dict_copy(&out_stream->metadata, in_stream->metadata, 0);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to copy metadata of stream #%u\n", i);
            return ret;
        }
        /*
         * 课程注释：连同每一路流的元数据（语言、handler 名）和标志（default、forced）。同样来自 patch 0075。
         * disposition 的 default 标志决定播放器默认选哪一路音轨或字幕，丢了就变成“都不是默认”。
         */
        out_stream->disposition = in_stream->disposition;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
                || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /*
             * 课程注释：选编码器。这个示例的做法是“输入是什么编码，输出就用什么编码”，所以输入是 H.264 输出就是 H.264，输入是 AAC 输出就是 AAC。
             * 输出容器放不放得下这种编码、有没有这种编码器，这里都不检查：比如输入是 MPEG-1 视频却要写成 MP4，会在后面失败。
             * 自己的程序一般按输出格式的默认编码器选，或者让用户指定（transcode_av.c 的做法）。
             */
            /* in this example, we choose transcoding to same codec */
            encoder = avcodec_find_encoder(dec_ctx->codec_id);
            if (!encoder) {
                av_log(NULL, AV_LOG_FATAL, "Necessary encoder not found\n");
                return AVERROR_INVALIDDATA;
            }
            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
                av_log(NULL, AV_LOG_FATAL, "Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            /* In this example, we transcode to same properties (picture size,
             * sample rate etc.). These properties can be changed for output
             * streams easily using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                const enum AVPixelFormat *pix_fmts = NULL;

                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                /*
                 * 课程注释：编码器的画面参数：宽高、像素宽高比，然后是色彩属性（范围、原色、传输特性、矩阵系数、色度采样位置）。
                 * 色彩属性是 patch 0074 补上的：原版漏了，输出的视频不带这些信息，播放器要靠猜，颜色可能不对。
                 * L13 讲过，这些信息通过编码器上下文写进码流或者容器。
                 */
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                enc_ctx->color_range = dec_ctx->color_range;
                enc_ctx->color_primaries = dec_ctx->color_primaries;
                enc_ctx->color_trc = dec_ctx->color_trc;
                enc_ctx->colorspace = dec_ctx->colorspace;
                enc_ctx->chroma_sample_location = dec_ctx->chroma_sample_location;

                /*
                 * 课程注释：向编码器查询它支持哪些像素格式，取第一个。官方示例这样取是为了简单；
                 * 更好的做法是把整个列表交给滤镜图出口，由它挑一个和输入最接近的，转换最少（transcode_av.c 的做法）。
                 * libx264 的第一个是 yuv420p，输入本来就是 yuv420p 就不用转换。
                 */
                ret = avcodec_get_supported_config(enc_ctx, NULL,
                                                   AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                                   (const void**)&pix_fmts, NULL);

                /* take first format from list of supported formats */
                enc_ctx->pix_fmt = (ret >= 0 && pix_fmts) ?
                                   pix_fmts[0] : dec_ctx->pix_fmt;

                /*
                 * 课程注释：视频编码器的时间基取帧率的倒数，这样每一帧的 pts 差 1（L13）。
                 * 这个办法假定帧率恒定。可变帧率的输入（比如手机录的视频）帧率是个平均值，每一帧的时间戳会被取整到这个网格上，产生抖动。
                 */
                /* video time_base can be set to whatever is handy and supported by encoder */
                enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
            } else {
                const enum AVSampleFormat *sample_fmts = NULL;

                enc_ctx->sample_rate = dec_ctx->sample_rate;
                ret = av_channel_layout_copy(&enc_ctx->ch_layout, &dec_ctx->ch_layout);
                if (ret < 0)
                    return ret;

                ret = avcodec_get_supported_config(enc_ctx, NULL,
                                                   AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                                   (const void**)&sample_fmts, NULL);

                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = (ret >= 0 && sample_fmts) ?
                                      sample_fmts[0] : dec_ctx->sample_fmt;

                /* 课程注释：音频编码器的时间基是 1/采样率，pts 就是第一个采样的序号（L14）。 */
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            /* 课程注释：复用器要求全局头（MP4、MKV）时，要在打开编码器之前设 AV_CODEC_FLAG_GLOBAL_HEADER（L15）。 */
            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            /* The frames sent to the encoder carry a valid duration, which
             * the encoder should pass on to the packets it outputs. */
            /*
             * 课程注释：告诉编码器“送来的帧带着时长，请把它带到输出的包上”。原版没有这个标志，输出的视频包都没有时长，
             * MP4 最后一帧的时长是 0，被编辑列表挡在外面，播放时丢掉最后一帧：patch 0011，L03 讲过这个现象。
             * 这个标志要配合下面 encode_write_frame() 里换算 duration 的那一行，缺一个都不行。
             */
            enc_ctx->flags |= AV_CODEC_FLAG_FRAME_DURATION;

            /* Third parameter can be used to pass settings to encoder */
            /* 课程注释：打开编码器。第三个参数可以传编码器选项（比如 crf、preset，L13），这个示例没有传，用默认值。 */
            ret = avcodec_open2(enc_ctx, encoder, NULL);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Cannot open %s encoder for stream #%u\n", encoder->name, i);
                return ret;
            }
            /* 课程注释：把编码器的参数（包括 extradata）复制给输出流，复用器写文件头时用它（L15）。 */
            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Failed to copy encoder parameters to output stream #%u\n", i);
                return ret;
            }

            /*
             * 课程注释：向复用器“建议”流的时间基。写文件头以后复用器可能改成别的值（MP4 的视频会变成 1/12800），
             * 所以后面写包之前要用 av_packet_rescale_ts() 换算，不能假定两者相等。
             */
            out_stream->time_base = enc_ctx->time_base;
            stream_ctx[i].enc_ctx = enc_ctx;
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            av_log(NULL, AV_LOG_FATAL, "Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            /* 课程注释：输入里不是音视频的流（字幕、数据流）不重新编码，参数直接拷贝，包原样写出去（流拷贝，和 L04 的 remux 一样）。 */
            /* if this stream must be remuxed */
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Copying parameters for stream #%u failed\n", i);
                return ret;
            }
            out_stream->time_base = in_stream->time_base;
        }

    }
    /* 课程注释：打印输出文件的流信息。这时只是准备好了，文件还没有打开。 */
    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        /* 课程注释：打开输出文件。下面的错误信息原版少了一个换行，后面的日志会接在同一行：patch 0076。 */
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Could not open output file '%s'\n", filename);
            return ret;
        }
    }

    /* 课程注释：写文件头。到这里所有的流都建好了，参数也都设完了；返回之后流的时间基才确定（L15）。 */
    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

/*
 * 课程注释：给一路流建滤镜图。结构和 L12 的 decode_filter_video.c、decode_filter_audio.c 一样：
 *     buffer/abuffer（入口，参数照着解码器）→ filter_spec 描述的滤镜 → buffersink/abuffersink（出口，参数照着编码器）
 * 出口的参数这里是照着“已经打开的编码器”设置的：编码器要什么格式，出口就只给什么格式，格式不一样滤镜图会自动插入 scale、aresample 转换。
 */
static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
        AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret = 0;
    const AVFilter *buffersrc = NULL;
    const AVFilter *buffersink = NULL;
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                /*
                 * 课程注释：入口的参数：尺寸、像素格式、时间基（来自 pkt_timebase）、像素宽高比，以及色彩属性。
                 * 色彩属性（colorspace、range、alpha_mode、chroma_location）是 patch 0007 补上的：
                 * 原版不传，第一帧进来时滤镜图就警告 “Changing video frame properties on the fly is not supported by all filters”（L12 讲过这个警告）。
                 */
                "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d:"
                "colorspace=%d:range=%d:alpha_mode=%d:chroma_location=%d",
                dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                dec_ctx->pkt_timebase.num, dec_ctx->pkt_timebase.den,
                dec_ctx->sample_aspect_ratio.num,
                dec_ctx->sample_aspect_ratio.den,
                dec_ctx->colorspace, dec_ctx->color_range, dec_ctx->alpha_mode,
                dec_ctx->chroma_sample_location);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer source\n");
            goto end;
        }

        /*
         * 课程注释：出口要先分配、设置选项、再初始化，这个顺序不能变。
         * 原版设置的是 pix_fmts、sample_fmts、ch_layouts、sample_rates 这几个选项，它们在新版本里已经不存在，
         * 运行到这里就报 “Cannot set output pixel format”：patch 0001。
         * 现在的写法是用 av_opt_set_array() 设置 pixel_formats、sample_formats、channel_layouts、samplerates 这几个数组选项。
         */
        buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "out");
        if (!buffersink_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create buffer sink\n");
            ret = AVERROR(ENOMEM);
            goto end;
        }

        /* 课程注释：出口只接受编码器的像素格式，数组里只有这一个。 */
        ret = av_opt_set_array(buffersink_ctx, "pixel_formats",
                AV_OPT_SEARCH_CHILDREN, 0, 1,
                AV_OPT_TYPE_PIXEL_FMT, &enc_ctx->pix_fmt);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output pixel format\n");
            goto end;
        }

        ret = avfilter_init_dict(buffersink_ctx, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot initialize buffer sink\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        char buf[64];
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            av_log(NULL, AV_LOG_ERROR, "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        if (dec_ctx->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
            av_channel_layout_default(&dec_ctx->ch_layout, dec_ctx->ch_layout.nb_channels);
        av_channel_layout_describe(&dec_ctx->ch_layout, buf, sizeof(buf));
        snprintf(args, sizeof(args),
                /* 课程注释：音频入口的时间基是 1/采样率，因为送进来的帧已经被 set_frame_pts() 换成了这个单位（patch 0077，原版用输入流的时间基）。 */
                "time_base=1/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
                dec_ctx->sample_rate, dec_ctx->sample_rate,
                av_get_sample_fmt_name(dec_ctx->sample_fmt),
                buf);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                args, NULL, filter_graph);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
            goto end;
        }

        buffersink_ctx = avfilter_graph_alloc_filter(filter_graph, buffersink, "out");
        if (!buffersink_ctx) {
            av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
            ret = AVERROR(ENOMEM);
            goto end;
        }

        /* 课程注释：音频出口同样限制三样：采样格式、声道布局、采样率，让滤镜图在需要时自己插入 aresample（L10）。 */
        ret = av_opt_set_array(buffersink_ctx, "sample_formats",
                AV_OPT_SEARCH_CHILDREN, 0, 1,
                AV_OPT_TYPE_SAMPLE_FMT, &enc_ctx->sample_fmt);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample format\n");
            goto end;
        }

        ret = av_opt_set_array(buffersink_ctx, "channel_layouts",
                AV_OPT_SEARCH_CHILDREN, 0, 1,
                AV_OPT_TYPE_CHLAYOUT, &enc_ctx->ch_layout);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output channel layout\n");
            goto end;
        }

        ret = av_opt_set_array(buffersink_ctx, "samplerates",
                AV_OPT_SEARCH_CHILDREN, 0, 1,
                AV_OPT_TYPE_INT, &enc_ctx->sample_rate);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot set output sample rate\n");
            goto end;
        }

        /*
         * 课程注释：AAC 这类编码器要求每一帧的采样数固定（1024），让出口把帧切成这个长度，不用自己攒（L14 的 AVAudioFifo 做的就是这件事）。
         * 这就是为什么编码器要在滤镜图之前打开：frame_size 要等 avcodec_open2() 以后才知道。
         */
        if (enc_ctx->frame_size > 0)
            av_buffersink_set_frame_size(buffersink_ctx, enc_ctx->frame_size);

        ret = avfilter_init_dict(buffersink_ctx, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot initialize audio buffer sink\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /*
     * 课程注释：把入口、出口接到 filter_spec 的两头。这里的写法（名字 in、out 互换）和 L12 一样：对解析器来说，
     * “in”是滤镜图的输入端，所以它接在入口滤镜的输出上。
     */
    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = NULL;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                    &inputs, &outputs, NULL)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
        goto end;

    /* Fill FilteringContext */
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;
    fctx->filter_graph = filter_graph;

/*
 * 课程注释：清理。输入打不开的时候 ifmt_ctx 是 NULL、stream_ctx 还没分配，原版的循环不检查，直接崩溃：patch 0071，
 * 所以现在循环条件里先判断了这两个指针。
 */
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

/*
 * 课程注释：给每一路音视频流建滤镜图。这里用的是“什么都不做”的滤镜：视频 null，音频 anull。
 * 想让转码的同时做点别的，只要改这里的字符串，例如视频 "scale=320:-2"，音频 "volume=0.5"。
 * 滤镜改了尺寸、帧率的话，编码器要跟着改，而这个示例的编码器是在滤镜图之前打开的，所以做不到：要么限制只能用不改画面参数的滤镜，
 * 要么像 transcode_av.c 那样，先配滤镜图，再按出口的参数开编码器。
 */
static int init_filters(void)
{
    const char *filter_spec;
    unsigned int i;
    int ret;
    filter_ctx = av_malloc_array(ifmt_ctx->nb_streams, sizeof(*filter_ctx));
    if (!filter_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        filter_ctx[i].buffersrc_ctx  = NULL;
        filter_ctx[i].buffersink_ctx = NULL;
        filter_ctx[i].filter_graph   = NULL;
        if (!(ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
                || ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
            continue;


        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            filter_spec = "null"; /* passthrough (dummy) filter for video */
        else
            filter_spec = "anull"; /* passthrough (dummy) filter for audio */
        ret = init_filter(&filter_ctx[i], stream_ctx[i].dec_ctx,
                stream_ctx[i].enc_ctx, filter_spec);
        if (ret)
            return ret;

        filter_ctx[i].enc_pkt = av_packet_alloc();
        if (!filter_ctx[i].enc_pkt)
            return AVERROR(ENOMEM);

        filter_ctx[i].filtered_frame = av_frame_alloc();
        if (!filter_ctx[i].filtered_frame)
            return AVERROR(ENOMEM);
    }
    return 0;
}

/*
 * 课程注释：编码一帧并把得到的包写进文件。flush 为 1 时送 NULL，冲刷编码器（L13）。
 * 这个函数里有两处时间戳换算，是本讲的重点：
 *     帧：滤镜图出口的时间基 → 编码器的时间基（pts 和 duration 一起换）
 *     包：编码器的时间基 → 输出流的时间基（av_packet_rescale_ts）
 */
static int encode_write_frame(unsigned int stream_index, int flush)
{
    StreamContext *stream = &stream_ctx[stream_index];
    FilteringContext *filter = &filter_ctx[stream_index];
    AVFrame *filt_frame = flush ? NULL : filter->filtered_frame;
    AVPacket *enc_pkt = filter->enc_pkt;
    int ret;

    av_log(NULL, AV_LOG_INFO, "Encoding frame\n");
    /* encode filtered frame */
    av_packet_unref(enc_pkt);

    if (filt_frame) {
        /*
         * 课程注释：第一处换算：帧从滤镜图出口的时间基（frame->time_base，下面 filter_encode_write_frame 里设的）换到编码器的时间基。
         * 这里不能直接用输入流的时间基：滤镜图里的 fps、setpts 等滤镜会改时间基（L12）。
         */
        if (filt_frame->pts != AV_NOPTS_VALUE)
            filt_frame->pts = av_rescale_q(filt_frame->pts, filt_frame->time_base,
                                           stream->enc_ctx->time_base);
        /* 课程注释：时长也要一起换。原版漏了这一行，编码器输出的视频包没有时长：patch 0011（和上面的 FRAME_DURATION 标志配套）。 */
        filt_frame->duration = av_rescale_q(filt_frame->duration, filt_frame->time_base,
                                            stream->enc_ctx->time_base);
    }

    /* 课程注释：送帧给编码器。filt_frame 为 NULL 就是冲刷。 */
    ret = avcodec_send_frame(stream->enc_ctx, filt_frame);

    if (ret < 0)
        return ret;

    while (ret >= 0) {
        ret = avcodec_receive_packet(stream->enc_ctx, enc_pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;

        /* prepare packet for muxing */
        /*
         * 课程注释：第二处换算：包从编码器的时间基换到输出流的时间基，pts、dts、duration 一起换。
         * 输出流的下标和输入流的下标相同，所以直接用 stream_index。
         */
        enc_pkt->stream_index = stream_index;
        av_packet_rescale_ts(enc_pkt,
                             stream->enc_ctx->time_base,
                             ofmt_ctx->streams[stream_index]->time_base);

        av_log(NULL, AV_LOG_DEBUG, "Muxing frame\n");
        /* mux encoded frame */
        /* 课程注释：交给复用器。av_interleaved_write_frame() 会按 dts 排好交织顺序再写（L15），包里的数据归复用器管。 */
        ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
    }

    return ret;
}

/* Set the timestamp of a decoded frame to the one the filters expect: for
 * video the best guess of the decoder, in the time base of the stream; for
 * audio the same, converted to 1/sample_rate. */
/*
 * 课程注释：设置解码出来的帧的 pts，patch 0077 新增的函数。
 * 视频用 best_effort_timestamp：解码器在 pts 缺失、乱序时按 dts 等线索推测出的值（L08），单位是输入流的时间基。
 * 音频要多做一步：换成 1/采样率为单位，同时去掉容器时间基太粗带来的抖动，见下面的说明。
 */
static void set_frame_pts(StreamContext *stream)
{
    AVFrame *frame = stream->dec_frame;
    AVRational sr_tb;

    frame->pts = frame->best_effort_timestamp;
    if (stream->dec_ctx->codec_type != AVMEDIA_TYPE_AUDIO)
        return;
    sr_tb = (AVRational){ 1, frame->sample_rate };

    /*
     * 课程注释：FLV、MKV 的时间基是毫秒，包的时间戳都取整过。1024 个采样在 44100 Hz 下是 23.22 毫秒，容器里存的是 23、23、24、23……毫秒，
     * 直接换成 1/44100 以后，相邻两帧的 pts 差 1014、1015、1058……而不是 1024，编码出来的音频包时间戳也跟着不均匀。
     * av_rescale_delta() 利用“每帧正好 nb_samples 个采样”这个已知的时长，在取整的误差范围内把时间戳接平；
     * 时间戳真的有断档（丢了数据）时它自动退回普通的换算。ffmpeg 命令行也是这样做的。
     */
    /* Containers with a coarse time base (1/1000 in FLV and Matroska) store
     * rounded timestamps, so the frames of 1024 samples come out 1014, 1015,
     * 1058... samples apart after a plain conversion to 1/sample_rate.
     * av_rescale_delta() uses the known duration of the frames to make the
     * timestamps continuous again, and falls back to a plain conversion when
     * there is a real gap. */
    if (frame->pts == AV_NOPTS_VALUE) {
        frame->pts = stream->audio_pts_last == AV_NOPTS_VALUE ? 0 : stream->audio_pts_last;
        stream->audio_pts_last = frame->pts + frame->nb_samples;
    } else {
        frame->pts = av_rescale_delta(stream->dec_ctx->pkt_timebase, frame->pts,
                                      sr_tb, frame->nb_samples,
                                      &stream->audio_pts_last, sr_tb);
    }
}

/*
 * 课程注释：解码出来的帧 → 滤镜图 → 编码器 → 文件。frame 为 NULL 表示输入结束，要让滤镜图把里面缓存的帧都吐出来。
 * 一帧进去，滤镜图可能吐出 0 个、1 个或者多个帧（比如 fps 滤镜，或者 AAC 要求的 1024 采样的切分），所以要用循环取，直到返回 EAGAIN 或 EOF。
 */
static int filter_encode_write_frame(AVFrame *frame, unsigned int stream_index)
{
    FilteringContext *filter = &filter_ctx[stream_index];
    int ret;

    av_log(NULL, AV_LOG_INFO, "Pushing decoded frame to filters\n");
    /* push the decoded frame into the filtergraph */
    /* 课程注释：把帧送进滤镜图入口（L12）。flags 为 0 时帧的数据交给滤镜图，之后 frame 里是空的。 */
    ret = av_buffersrc_add_frame_flags(filter->buffersrc_ctx,
            frame, 0);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error while feeding the filtergraph\n");
        return ret;
    }

    /* pull filtered frames from the filtergraph */
    while (1) {
        av_log(NULL, AV_LOG_INFO, "Pulling filtered frame from filters\n");
        /* 课程注释：从出口取帧。EAGAIN 表示“现在没有，再送点输入”，EOF 表示“滤镜图已经冲刷完了”，两个都是正常情况，不是错误。 */
        ret = av_buffersink_get_frame(filter->buffersink_ctx,
                                      filter->filtered_frame);
        if (ret < 0) {
            /* if no more frames for output - returns AVERROR(EAGAIN)
             * if flushed and no more frames for output - returns AVERROR_EOF
             * rewrite retcode to 0 to show it as normal procedure completion
             */
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            break;
        }

        /*
         * 课程注释：向出口问它输出的帧用的时间基，记在帧上，encode_write_frame() 换算时间戳时用。
         * 这就是 L12 讲稿里说的“交给编码器的帧，时间戳要用出口的时间基换算”。
         * （这一行末尾有两个分号，空语句，不影响运行，是官方示例里留着的笔误。）
         */
        filter->filtered_frame->time_base = av_buffersink_get_time_base(filter->buffersink_ctx);;
        /*
         * 课程注释：把帧类型清成“未指定”。解码器给的帧类型是 I、P、B（L08），如果原样交给编码器，编码器会照着输入的类型编，
         * 输入里的每个 I 帧都会变成输出的关键帧，关键帧间隔被输入带偏。清掉以后由编码器自己决定。
         */
        filter->filtered_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ret = encode_write_frame(stream_index, 0);
        av_frame_unref(filter->filtered_frame);
        if (ret < 0)
            break;
    }

    return ret;
}

/*
 * 课程注释：冲刷编码器：编码器有延迟（B 帧、前瞻），要送 NULL 才会把缓存里的帧编完吐出来（L13）。
 * 编码器没有 AV_CODEC_CAP_DELAY 能力（比如 PCM）就没有缓存，不用冲刷。
 */
static int flush_encoder(unsigned int stream_index)
{
    if (!(stream_ctx[stream_index].enc_ctx->codec->capabilities &
                AV_CODEC_CAP_DELAY))
        return 0;

    av_log(NULL, AV_LOG_INFO, "Flushing stream #%u encoder\n", stream_index);
    return encode_write_frame(stream_index, 1);
}

/* 课程注释：主流程。三步：初始化（打开输入、打开输出、建滤镜图）→ 循环读包处理 → 冲刷并收尾。 */
int main(int argc, char **argv)
{
    int ret;
    AVPacket *packet = NULL;
    unsigned int stream_index;
    unsigned int i;

    if (argc != 3) {
        av_log(NULL, AV_LOG_ERROR, "Usage: %s <input file> <output file>\n", argv[0]);
        return 1;
    }

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;
    if ((ret = open_output_file(argv[2])) < 0)
        goto end;
    if ((ret = init_filters()) < 0)
        goto end;
    if (!(packet = av_packet_alloc()))
        goto end;

    /*
     * 课程注释：读包的主循环。每读到一个包，按它所属的流分两种处理：
     *     音视频流（filter_graph 不为空）：解码 → 滤镜 → 编码 → 写；
     *     其它流：不处理，时间基换算以后原样写出去。
     */
    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(ifmt_ctx, packet)) < 0)
            break;
        stream_index = packet->stream_index;
        av_log(NULL, AV_LOG_DEBUG, "Demuxer gave frame of stream_index %u\n",
                stream_index);

        if (filter_ctx[stream_index].filter_graph) {
            StreamContext *stream = &stream_ctx[stream_index];

            av_log(NULL, AV_LOG_DEBUG, "Going to reencode&filter the frame\n");

            /*
             * 课程注释：送包给解码器。送包失败时原版只 break 出循环，后面照常冲刷、写文件尾、返回 0，输出被悄悄截断，调用方还以为成功：
             * patch 0073 改成 goto end，保留错误码。
             */
            ret = avcodec_send_packet(stream->dec_ctx, packet);
            if (ret < 0) {
                av_log(NULL, AV_LOG_ERROR, "Decoding failed\n");
                goto end;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    break;
                else if (ret < 0)
                    goto end;

                /* 课程注释：设置帧的时间戳，见上面的 set_frame_pts()。送进滤镜图之前，每一帧的 pts 都要整理好。 */
                set_frame_pts(stream);
                ret = filter_encode_write_frame(stream->dec_frame, stream_index);
                if (ret < 0)
                    goto end;
            }
        } else {
            /*
             * 课程注释：不是音视频的流：只换时间基，原样写出去。这就是“流拷贝”：不解码、不编码，速度快，没有质量损失，
             * 但也不能在中间做任何处理，而且输出容器要放得下这种编码。
             */
            /* remux this frame without reencoding */
            av_packet_rescale_ts(packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 ofmt_ctx->streams[stream_index]->time_base);

            ret = av_interleaved_write_frame(ofmt_ctx, packet);
            if (ret < 0)
                goto end;
        }
        av_packet_unref(packet);
    }

    /*
     * 课程注释：输入读完以后的收尾：对每一路流，从上游到下游依次冲刷——先解码器，再滤镜图，最后编码器（L11 讲过这个顺序）。
     * 顺序反了的话，下游先结束，上游后吐出来的帧就没处去了。
     */
    /* flush decoders, filters and encoders */
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        StreamContext *stream;

        if (!filter_ctx[i].filter_graph)
            continue;

        stream = &stream_ctx[i];

        av_log(NULL, AV_LOG_INFO, "Flushing stream %u decoder\n", i);

        /* flush decoder */
        ret = avcodec_send_packet(stream->dec_ctx, NULL);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing decoding failed\n");
            goto end;
        }

        while (ret >= 0) {
            ret = avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);
            if (ret == AVERROR_EOF)
                break;
            else if (ret < 0)
                goto end;

            set_frame_pts(stream);
            ret = filter_encode_write_frame(stream->dec_frame, i);
            if (ret < 0)
                goto end;
        }

        /* flush filter */
        ret = filter_encode_write_frame(NULL, i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing filter failed\n");
            goto end;
        }

        /* flush encoder */
        ret = flush_encoder(i);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR, "Flushing encoder failed\n");
            goto end;
        }
    }

    /* 课程注释：写文件尾。MP4 的 moov 索引在这里写；原版没有记下它的返回值，写失败了也照常返回 0，留下一个打不开的文件：patch 0072。 */
    ret = av_write_trailer(ofmt_ctx);
end:
    av_packet_free(&packet);
    for (i = 0; ifmt_ctx && stream_ctx && i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);
        if (filter_ctx && filter_ctx[i].filter_graph) {
            avfilter_graph_free(&filter_ctx[i].filter_graph);
            av_packet_free(&filter_ctx[i].enc_pkt);
            av_frame_free(&filter_ctx[i].filtered_frame);
        }

        av_frame_free(&stream_ctx[i].dec_frame);
    }
    av_free(filter_ctx);
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        /* 课程注释：关闭输出文件时缓冲区里剩下的数据才真正写出去，磁盘满了就在这里失败，所以返回值要检查（patch 0072）。 */
        int close_ret = avio_closep(&ofmt_ctx->pb);
        if (close_ret < 0 && ret >= 0)
            ret = close_ret;
    }
    avformat_free_context(ofmt_ctx);

    if (ret < 0)
        /*
         * 课程注释：用 av_err2str() 把错误码翻译成文字，所以各处要把真实的错误码一路传回来，不能自己换成别的。
         * patch 0014 改的就是这个：输出文件的扩展名不认识时，avformat_alloc_output_context2() 的返回值原版被丢掉，换成 AVERROR_UNKNOWN，
         * 这里只能打印 “Unknown error occurred”，现在能打印真实的 “Invalid argument”。
         */
        av_log(NULL, AV_LOG_ERROR, "Error occurred: %s\n", av_err2str(ret));

    return ret ? 1 : 0;
}
