/*
 * Copyright (c) 2013-2022 Andreas Unterweger
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file audio transcoding to MPEG/AAC API usage example
 * @example transcode_aac.c
 *
 * Convert an input audio file to AAC in an MP4 container. Formats other than
 * MP4 are supported based on the output file extension.
 * @author Andreas Unterweger (dustsigns@gmail.com)
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/transcode_aac.c 的拷贝，代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它把输入文件里的音频转成 AAC，写进 MP4（别的容器按输出文件的扩展名猜）。和 L16 的 transcode.c 比，它没有滤镜图，
 * 格式转换和帧长调整都是自己做的：libswresample 转采样格式，AVAudioFifo 攒采样、切成编码器要的长度。
 * 注意：拷贝的不是提交 a337e8d 的原版，而是打了 4 个 patch（0078 到 0081）的版本，改动过的地方下面都标了编号：
 *     0078  包的时间戳要换算到输出流的时间基（原版只有 MP4 碰巧对，MKV、FLV、TS、AVI 的时间戳全错）
 *     0079  读到文件尾以后，解码器里还有帧没取出来（有的解码器会丢最后一帧）
 *     0080  输入里有视频或者封面图（不止一路流）时原版直接拒绝
 *     0081  输入的元数据（标题、艺术家）原版不拷贝
 * 程序结构（数据流和 L16 的转码一样，只是中间多了“转格式”和“攒采样”两站）：
 *     open_input_file()    打开输入，选一路音频，开解码器（L03、L07）
 *     open_output_file()   创建输出，开 AAC 编码器，建输出流（L13 到 L15）
 *     init_resampler()     建转换器，把解码器的采样格式转成编码器要的（L10）
 *     init_fifo()          建 FIFO，用来攒采样
 *     main()               读包 → 解码 → 转换 → 进 FIFO → 够一帧就出 FIFO → 编码 → 写包，最后冲刷
 * 自己写程序时参考本目录的 transcode_aac_av.c。
 */

#include <stdio.h>

#include <libavutil/mem.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>

#include <libavcodec/avcodec.h>

#include <libavutil/audio_fifo.h>
#include <libavutil/avassert.h>
#include <libavutil/avstring.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#include <libswresample/swresample.h>

/*
 * 课程注释：输出的码率和声道数是写死的：96 kbit/s、立体声。输入是单声道，输出也是双声道（左右一样）；
 * 输入是 5.1，转换器会把 6 个声道混成两个。采样率不写死，照着输入走（所以输入是 AAC 不支持的采样率，比如 192 kHz，这个示例就失败了）。
 * transcode_aac_av.c 把这三项做成了命令行参数 br=、ch=、ar=。
 */
/* The output bit rate in bit/s */
#define OUTPUT_BIT_RATE 96000
/* The number of output channels */
#define OUTPUT_CHANNELS 2

/**
 * Open an input file and the required decoder.
 * @param      filename             File to be opened
 * @param[out] input_format_context Format context of opened file
 * @param[out] input_codec_context  Codec context of opened file
 * @param[out] input_stream_index   Index of the audio stream that is decoded
 * @return Error code (0 if successful)
 */
/* 课程注释：打开输入文件，选出要解码的那一路音频，开解码器。套路和 L07 的 demux_decode.c 一样，只取音频这一路。 */
static int open_input_file(const char *filename,
                           AVFormatContext **input_format_context,
                           AVCodecContext **input_codec_context,
                           int *input_stream_index)
{
    AVCodecContext *avctx;
    const AVCodec *input_codec;
    const AVStream *stream;
    int error;

    /* Open the input file to read from it. */
    if ((error = avformat_open_input(input_format_context, filename, NULL,
                                     NULL)) < 0) {
        fprintf(stderr, "Could not open input file '%s' (error '%s')\n",
                filename, av_err2str(error));
        *input_format_context = NULL;
        return error;
    }

    /* Get information on the input file (number of streams etc.). */
    if ((error = avformat_find_stream_info(*input_format_context, NULL)) < 0) {
        fprintf(stderr, "Could not open find stream info (error '%s')\n",
                av_err2str(error));
        avformat_close_input(input_format_context);
        return error;
    }

    /*
     * 课程注释：patch 0080。原版要求输入只有一路流，视频文件里的音轨、带封面图的音乐文件（封面图是第二路流）都用不了。
     * av_find_best_stream() 在所有流里挑“最好的”一路音频：先排除标成听障、视障辅助的音轨，优先选带 default 标志的，
     * 然后是探测时读到的帧多的、码率高的；同时检查有没有对应的解码器，一并返回。其余的流读到了就丢掉（下面 decode_audio_frame）。
     */
    /* Select the audio stream and find a decoder for it. Other streams in
     * the input file, such as the video of a movie or the cover art of an
     * audio file, are ignored. */
    error = av_find_best_stream(*input_format_context, AVMEDIA_TYPE_AUDIO,
                                -1, -1, &input_codec, 0);
    if (error < 0) {
        fprintf(stderr, "Could not find an audio stream and its decoder "
                "(error '%s')\n", av_err2str(error));
        avformat_close_input(input_format_context);
        return error;
    }
    *input_stream_index = error;
    stream = (*input_format_context)->streams[*input_stream_index];

    /* Allocate a new decoding context. */
    avctx = avcodec_alloc_context3(input_codec);
    if (!avctx) {
        fprintf(stderr, "Could not allocate a decoding context\n");
        avformat_close_input(input_format_context);
        return AVERROR(ENOMEM);
    }

    /* Initialize the stream parameters with demuxer information. */
    error = avcodec_parameters_to_context(avctx, stream->codecpar);
    if (error < 0) {
        avformat_close_input(input_format_context);
        avcodec_free_context(&avctx);
        return error;
    }

    /* Open the decoder for the audio stream to use it later. */
    if ((error = avcodec_open2(avctx, input_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open input codec (error '%s')\n",
                av_err2str(error));
        avcodec_free_context(&avctx);
        avformat_close_input(input_format_context);
        return error;
    }

    /*
     * 课程注释：把包的时间基告诉解码器。这个示例把它放在 avcodec_open2() 之后，对音频解码器碰巧没有关系：
     * libavcodec/decode.c 是每解出一帧才用到它。但没有任何文档保证，自己写的程序按 L07 的做法设在打开之前。
     */
    /* Set the packet timebase for the decoder. */
    avctx->pkt_timebase = stream->time_base;

    /* Save the decoder context for easier access later. */
    *input_codec_context = avctx;

    return 0;
}

/**
 * Open an output file and the required encoder.
 * Also set some basic encoder parameters.
 * Some of these parameters are based on the input file's parameters.
 * @param      filename              File to be opened
 * @param      input_codec_context   Codec context of input file
 * @param[out] output_format_context Format context of output file
 * @param[out] output_codec_context  Codec context of output file
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：创建输出文件，开 AAC 编码器，建输出流。套路和 L15 的 mux_av.c 一样，编码器的参数照着输入定。
 * 注意顺序：这个示例先创建输出文件，再配编码器。编码器配置失败（比如输入是 192 kHz，AAC 不支持）时，文件已经被创建、清空了，
 * 命令行返回错误，输出文件却留下一个 0 字节的垃圾。transcode_aac_av.c 把顺序倒过来：编码器和输出流都配好以后才创建文件。
 */
static int open_output_file(const char *filename,
                            AVCodecContext *input_codec_context,
                            AVFormatContext **output_format_context,
                            AVCodecContext **output_codec_context)
{
    AVCodecContext *avctx          = NULL;
    AVIOContext *output_io_context = NULL;
    AVStream *stream               = NULL;
    const AVCodec *output_codec    = NULL;
    const void *sample_fmts;
    int error;

    /* Open the output file to write to it. */
    if ((error = avio_open(&output_io_context, filename,
                           AVIO_FLAG_WRITE)) < 0) {
        fprintf(stderr, "Could not open output file '%s' (error '%s')\n",
                filename, av_err2str(error));
        return error;
    }

    /* Create a new format context for the output container format. */
    if (!(*output_format_context = avformat_alloc_context())) {
        fprintf(stderr, "Could not allocate output format context\n");
        return AVERROR(ENOMEM);
    }

    /* Associate the output file (pointer) with the container format context. */
    (*output_format_context)->pb = output_io_context;

    /*
     * 课程注释：按输出文件的扩展名猜容器格式。猜不出来就失败，使用者要换一个扩展名。
     * 这个示例的 main() 出错时统一返回 AVERROR_EXIT，不区分原因，具体原因由出错的地方自己打印到屏幕上。
     */
    /* Guess the desired container format based on the file extension. */
    if (!((*output_format_context)->oformat = av_guess_format(NULL, filename,
                                                              NULL))) {
        fprintf(stderr, "Could not find output file format\n");
        goto cleanup;
    }

    if (!((*output_format_context)->url = av_strdup(filename))) {
        fprintf(stderr, "Could not allocate url.\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    /* 课程注释：编码器写死是 AAC。avcodec_find_encoder() 返回 FFmpeg 自带的那个 aac 编码器（libfdk_aac 要另外编译进来）。 */
    /* Find the encoder to be used by its name. */
    if (!(output_codec = avcodec_find_encoder(AV_CODEC_ID_AAC))) {
        fprintf(stderr, "Could not find an AAC encoder.\n");
        goto cleanup;
    }

    /* Create a new audio stream in the output file container. */
    if (!(stream = avformat_new_stream(*output_format_context, NULL))) {
        fprintf(stderr, "Could not create new stream\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    avctx = avcodec_alloc_context3(output_codec);
    if (!avctx) {
        fprintf(stderr, "Could not allocate an encoding context\n");
        error = AVERROR(ENOMEM);
        goto cleanup;
    }

    /*
     * 课程注释：编码器的基本参数。声道布局按声道数取默认的（2 个是立体声）；
     * 采样格式取编码器支持的第一种，AAC 是 fltp（planar 浮点）；采样率照着输入。
     * avcodec_get_supported_config() 是新接口，取代了过时的 AVCodec::sample_fmts 字段，L13 讲过。
     * 采样率这里不管编码器支不支持，是否支持要等 avcodec_open2() 才知道：AAC 只支持 96000 到 7350 之间的 13 种固定值。
     */
    /* Set the basic encoder parameters.
     * The input file's sample rate is used to avoid a sample rate conversion. */
    av_channel_layout_default(&avctx->ch_layout, OUTPUT_CHANNELS);
    error = avcodec_get_supported_config(avctx, NULL, AV_CODEC_CONFIG_SAMPLE_FORMAT,
                                         0, &sample_fmts, NULL);
    av_assert0(error >= 0);
    avctx->sample_fmt     = sample_fmts ? *(const enum AVSampleFormat*)sample_fmts
                                        : input_codec_context->sample_fmt;
    avctx->sample_rate    = input_codec_context->sample_rate;
    avctx->bit_rate       = OUTPUT_BIT_RATE;

    /*
     * 课程注释：patch 0078。音频编码器的时间基是 1/采样率，pts 就是这一帧第一个采样的序号（L14）。
     * 原版没有设编码器的时间基，靠的是“流的时间基碰巧是 1/采样率”。
     */
    /* The encoder timestamps its frames and packets in samples. */
    avctx->time_base.den  = input_codec_context->sample_rate;
    avctx->time_base.num  = 1;

    /*
     * 课程注释：patch 0078。向复用器“建议”流的时间基，复用器可以不听（L15）：
     *     MP4          1/采样率，和建议的一样，所以原版输出看起来没问题
     *     MKV、FLV     1/1000
     *     MPEG-TS      1/90000
     *     AVI          一个奇怪的分数（5 秒 44.1 kHz 的输入，写出 5 MB 的文件，时长 5143 秒）
     * 所以写包之前必须用 av_packet_rescale_ts() 换算（见 encode_audio_frame）。
     */
    /* Suggest the same time base for the container. The muxer is free to
     * choose another one in avformat_write_header(), so the packet
     * timestamps are converted before they are written. */
    stream->time_base     = avctx->time_base;

    /*
     * 课程注释：复用器要求全局头（MP4、MKV）时，要在打开编码器之前设 AV_CODEC_FLAG_GLOBAL_HEADER，
     * 编码器才会把 AudioSpecificConfig 这类配置放进 extradata，而不是放在每个包里（L15）。
     */
    /* Some container formats (like MP4) require global headers to be present.
     * Mark the encoder so that it behaves accordingly. */
    if ((*output_format_context)->oformat->flags & AVFMT_GLOBALHEADER)
        avctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    /*
     * 课程注释：打开编码器。从这里开始 frame_size 才有值：AAC 是 1024，后面所有“攒够一帧再编码”的判断都靠它。
     * 编码器不认识的参数在这里失败，AAC 编码器会拒绝 AAC 不支持的采样率、声道数。
     */
    /* Open the encoder for the audio stream to use it later. */
    if ((error = avcodec_open2(avctx, output_codec, NULL)) < 0) {
        fprintf(stderr, "Could not open output codec (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

    error = avcodec_parameters_from_context(stream->codecpar, avctx);
    if (error < 0) {
        fprintf(stderr, "Could not initialize stream parameters\n");
        goto cleanup;
    }

    /* Save the encoder context for easier access later. */
    *output_codec_context = avctx;

    return 0;

cleanup:
    avcodec_free_context(&avctx);
    avio_closep(&(*output_format_context)->pb);
    avformat_free_context(*output_format_context);
    *output_format_context = NULL;
    return error < 0 ? error : AVERROR_EXIT;
}

/**
 * Copy the metadata of the input file and of its audio stream (title, artist,
 * language, ...) to the output file, and the dispositions of the stream.
 * This has to be done before the header of the output file is written.
 * @param input_format_context  Format context of the input file
 * @param input_stream_index    Index of the audio stream in the input file
 * @param output_format_context Format context of the output file
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：patch 0081。拷贝输入的元数据（容器的、这一路流的），以及流的标志（default、forced）。
 * 没有这一步，转出来的文件没有标题、艺术家，在音乐库里就是一首无名的歌。要在 avformat_write_header() 之前做。
 */
static int copy_metadata(const AVFormatContext *input_format_context,
                         int input_stream_index,
                         AVFormatContext *output_format_context)
{
    const AVStream *input_stream = input_format_context->streams[input_stream_index];
    AVStream *output_stream      = output_format_context->streams[0];
    int error;

    if ((error = av_dict_copy(&output_format_context->metadata,
                              input_format_context->metadata, 0)) < 0 ||
        (error = av_dict_copy(&output_stream->metadata,
                              input_stream->metadata, 0)) < 0) {
        fprintf(stderr, "Could not copy metadata (error '%s')\n",
                av_err2str(error));
        return error;
    }
    output_stream->disposition = input_stream->disposition;
    return 0;
}

/**
 * Initialize one data packet for reading or writing.
 * @param[out] packet Packet to be initialized
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：分配一个包。这个示例每读一个包、每编出一个包都分配、释放一次，简单但是多余：
 * L03 的做法是在循环外分配一个，循环里用完 av_packet_unref() 重复使用。
 */
static int init_packet(AVPacket **packet)
{
    if (!(*packet = av_packet_alloc())) {
        fprintf(stderr, "Could not allocate packet\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Initialize one audio frame for reading from the input file.
 * @param[out] frame Frame to be initialized
 * @return Error code (0 if successful)
 */
static int init_input_frame(AVFrame **frame)
{
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate input frame\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Initialize the audio resampler based on the input and output codec settings.
 * If the input and output sample formats differ, a conversion is required
 * libswresample takes care of this, but requires initialization.
 * @param      input_codec_context  Codec context of the input file
 * @param      output_codec_context Codec context of the output file
 * @param[out] resample_context     Resample context for the required conversion
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：建重采样器（L10 讲过）：输入是解码器的格式（比如 mp2 的 s16p），输出是编码器要的格式（AAC 的 fltp）。
 * 两边声道数不同时（单声道 → 立体声、5.1 → 立体声）它也负责混音。三项都一样时它什么都不做，直接拷贝。
 */
static int init_resampler(AVCodecContext *input_codec_context,
                          AVCodecContext *output_codec_context,
                          SwrContext **resample_context)
{
        int error;

        /*
         * Create a resampler context for the conversion.
         * Set the conversion parameters.
         */
        error = swr_alloc_set_opts2(resample_context,
                                             &output_codec_context->ch_layout,
                                              output_codec_context->sample_fmt,
                                              output_codec_context->sample_rate,
                                             &input_codec_context->ch_layout,
                                              input_codec_context->sample_fmt,
                                              input_codec_context->sample_rate,
                                              0, NULL);
        if (error < 0) {
            fprintf(stderr, "Could not allocate resample context\n");
            return error;
        }
        /*
        * Perform a sanity check so that the number of converted samples is
        * not greater than the number of samples to be converted.
        * If the sample rates differ, this case has to be handled differently
        */
        /*
         * 课程注释：这个示例不做采样率转换：输出的采样率总是等于输入的，这条断言永远成立，它提醒的是“想改采样率先看这里”。
         * 采样率不同时转换器会缓存一些采样，一次转换出来的个数不等于输入的个数，而且结束时要冲刷（swr_convert() 传 NULL 输入）。
         * transcode_aac_av.c 的做法是用 swr_convert_frame()，让它自己分配输出帧，并且在最后冲刷。
         */
        av_assert0(output_codec_context->sample_rate == input_codec_context->sample_rate);

        /* Open the resampler with the specified parameters. */
        if ((error = swr_init(*resample_context)) < 0) {
            fprintf(stderr, "Could not open resample context\n");
            swr_free(resample_context);
            return error;
        }
    return 0;
}

/**
 * Initialize a FIFO buffer for the audio samples to be encoded.
 * @param[out] fifo                 Sample buffer
 * @param      output_codec_context Codec context of the output file
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：建 FIFO（先进先出的采样队列）。为什么需要它：解码器每次给多少采样由输入的编码决定（mp2 是 1152，FLAC 是 4096，
 * Vorbis 最后一帧更短），编码器每次要多少由它自己决定（AAC 是 1024，不能多也不能少，最后一帧例外）。两个数字不同，
 * 中间就要一个缓冲：解码出来的采样先全放进去，够编码器要的数量了再取出来，剩下的留着等下一轮。
 * 最后一个参数是初始容量，随便给，av_audio_fifo_write() 空间不够会自己扩大。
 */
static int init_fifo(AVAudioFifo **fifo, AVCodecContext *output_codec_context)
{
    /* Create the FIFO buffer based on the specified output sample format. */
    if (!(*fifo = av_audio_fifo_alloc(output_codec_context->sample_fmt,
                                      output_codec_context->ch_layout.nb_channels, 1))) {
        fprintf(stderr, "Could not allocate FIFO\n");
        return AVERROR(ENOMEM);
    }
    return 0;
}

/**
 * Write the header of the output file container.
 * @param output_format_context Format context of the output file
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：写文件头。到这里编码器和输出流的参数都定下来了，元数据也设好了；返回以后复用器才可能改流的时间基（L15）。
 * 这个示例没有传复用器选项（第二个参数是 NULL），要用 movflags=+faststart 这类选项要自己传，transcode_aac_av.c 的命令行就是这样做的。
 */
static int write_output_file_header(AVFormatContext *output_format_context)
{
    int error;
    if ((error = avformat_write_header(output_format_context, NULL)) < 0) {
        fprintf(stderr, "Could not write output file header (error '%s')\n",
                av_err2str(error));
        return error;
    }
    return 0;
}

/**
 * Decode one audio frame from the input file.
 * @param      frame                Audio frame to be decoded
 * @param      input_format_context Format context of the input file
 * @param      input_codec_context  Codec context of the input file
 * @param      input_stream_index   Index of the audio stream to be decoded.
 *                                  Packets of other streams are skipped.
 * @param[out] data_present         Indicates whether data has been decoded
 * @param[out] finished             Indicates whether the end of file has
 *                                  been reached and all data has been
 *                                  decoded. If this flag is false, there
 *                                  is more data to be decoded, i.e., this
 *                                  function has to be called again.
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：读一个包、解一帧。和 L06 的“送一个包、循环取所有的帧”不同，这里每次调用最多给出一帧，靠外层的循环重复调用。
 * 返回值有三种状态：data_present = 1 有一帧；data_present = 0 且 finished = 0 解码器需要更多的包；finished = 1 全部解完了。
 */
static int decode_audio_frame(AVFrame *frame,
                              AVFormatContext *input_format_context,
                              AVCodecContext *input_codec_context,
                              int input_stream_index,
                              int *data_present, int *finished)
{
    /* Packet used for temporary storage. */
    AVPacket *input_packet;
    int error;

    error = init_packet(&input_packet);
    if (error < 0)
        return error;

    *data_present = 0;
    *finished = 0;
    /* Read one audio frame from the input file into a temporary packet,
     * skipping the packets of other streams. */
    do {
        av_packet_unref(input_packet);
        error = av_read_frame(input_format_context, input_packet);
    /* 课程注释：patch 0080。读到别的流（视频、封面图）的包就丢掉，继续读，直到读到要解码的音频流的包或者文件结束。 */
    } while (error >= 0 && input_packet->stream_index != input_stream_index);
    if (error < 0) {
        /*
         * 课程注释：patch 0079。文件尾时 av_read_frame() 返回 AVERROR_EOF，包是空的；把空包送给解码器就是“冲刷”：
         * 告诉它没有输入了，请把缓存的帧都吐出来（L06）。有的解码器有延迟（wmav2、ape、opus……带 AV_CODEC_CAP_DELAY），冲刷时还会吐出帧。
         * 原版一读到文件尾就直接宣布“解码结束”，这些帧就丢了：一个 219136 个采样的 WMA 文件，输出少一整帧 2048 个采样。
         */
        /* At the end of the file the packet stays empty. Sending it to the
         * decoder below flushes the decoder, which may still hold frames. */
        if (error != AVERROR_EOF) {
            fprintf(stderr, "Could not read frame (error '%s')\n",
                    av_err2str(error));
            goto cleanup;
        }
    }

    /* Send the audio frame stored in the temporary packet to the decoder.
     * The input audio stream decoder is used to do this.
     * Flushing a decoder that has been flushed already returns AVERROR_EOF,
     * which is not an error: the remaining frames are received below. */
    error = avcodec_send_packet(input_codec_context, input_packet);
    if (error < 0 && error != AVERROR_EOF) {
        fprintf(stderr, "Could not send packet for decoding (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

    /*
     * 课程注释：取一帧。EAGAIN 是“需要更多的包”，不是错误；AVERROR_EOF 是“解码器冲刷完了”，这时才设 finished。
     * （patch 0079 之前 finished 在读到文件尾时就设了。）一次只取一帧，对于“一个包出一帧”的音频解码器没有问题。
     */
    /* Receive one frame from the decoder. */
    error = avcodec_receive_frame(input_codec_context, frame);
    /* If the decoder asks for more data to be able to decode a frame,
     * return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
    /* If the end of the input file is reached, stop decoding. */
    } else if (error == AVERROR_EOF) {
        *finished = 1;
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not decode frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    /* Default case: Return decoded data. */
    } else {
        *data_present = 1;
        goto cleanup;
    }

cleanup:
    av_packet_free(&input_packet);
    return error;
}

/**
 * Initialize a temporary storage for the specified number of audio samples.
 * The conversion requires temporary storage due to the different format.
 * The number of audio samples to be allocated is specified in frame_size.
 * @param[out] converted_input_samples Array of converted samples. The
 *                                     dimensions are reference, channel
 *                                     (for multi-channel audio), sample.
 * @param      output_codec_context    Codec context of the output file
 * @param      frame_size              Number of samples to be converted in
 *                                     each round
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：为转换后的采样分配临时的存放空间。采样用的是裸的指针数组：每个声道一个指针（planar 格式，比如 fltp），
 * packed 格式（比如 s16）所有声道交织在一起，只有第一个指针有用。这里按“编码器要的格式”、frame_size 个采样分配。
 * 裸指针数组不带格式和声道布局的信息，容易搞错；transcode_aac_av.c 用 AVFrame 装转换结果，让 swr_convert_frame() 自己分配。
 */
static int init_converted_samples(uint8_t ***converted_input_samples,
                                  AVCodecContext *output_codec_context,
                                  int frame_size)
{
    int error;

    /* Allocate as many pointers as there are audio channels.
     * Each pointer will point to the audio samples of the corresponding
     * channels (although it may be NULL for interleaved formats).
     * Allocate memory for the samples of all channels in one consecutive
     * block for convenience. */
    if ((error = av_samples_alloc_array_and_samples(converted_input_samples, NULL,
                                  output_codec_context->ch_layout.nb_channels,
                                  frame_size,
                                  output_codec_context->sample_fmt, 0)) < 0) {
        fprintf(stderr,
                "Could not allocate converted input samples (error '%s')\n",
                av_err2str(error));
        return error;
    }
    return 0;
}

/**
 * Convert the input audio samples into the output sample format.
 * The conversion happens on a per-frame basis, the size of which is
 * specified by frame_size.
 * @param      input_data       Samples to be decoded. The dimensions are
 *                              channel (for multi-channel audio), sample.
 * @param[out] converted_data   Converted samples. The dimensions are channel
 *                              (for multi-channel audio), sample.
 * @param      frame_size       Number of samples to be converted
 * @param      resample_context Resample context for the conversion
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：格式转换：采样格式（s16p → fltp）、必要时声道数（5.1 → 立体声）。输入输出的采样数都是 frame_size，
 * 因为采样率相同时转换器不缓存采样，进多少、出多少。
 */
static int convert_samples(const uint8_t **input_data,
                           uint8_t **converted_data, const int frame_size,
                           SwrContext *resample_context)
{
    int error;

    /* Convert the samples using the resampler. */
    if ((error = swr_convert(resample_context,
                             converted_data, frame_size,
                             input_data    , frame_size)) < 0) {
        fprintf(stderr, "Could not convert input samples (error '%s')\n",
                av_err2str(error));
        return error;
    }

    return 0;
}

/**
 * Add converted input audio samples to the FIFO buffer for later processing.
 * @param fifo                    Buffer to add the samples to
 * @param converted_input_samples Samples to be added. The dimensions are channel
 *                                (for multi-channel audio), sample.
 * @param frame_size              Number of samples to be converted
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：把转换后的采样放进 FIFO。下面的 av_audio_fifo_realloc() 其实是多余的：av_audio_fifo_write() 在空间不够时
 * 自己会扩大（扩成 2 倍），官方示例里先扩一遍是老版本留下来的写法。
 */
static int add_samples_to_fifo(AVAudioFifo *fifo,
                               uint8_t **converted_input_samples,
                               const int frame_size)
{
    int error;

    /* Make the FIFO as large as it needs to be to hold both,
     * the old and the new samples. */
    if ((error = av_audio_fifo_realloc(fifo, av_audio_fifo_size(fifo) + frame_size)) < 0) {
        fprintf(stderr, "Could not reallocate FIFO\n");
        return error;
    }

    /* Store the new samples in the FIFO buffer. */
    if (av_audio_fifo_write(fifo, (void **)converted_input_samples,
                            frame_size) < frame_size) {
        fprintf(stderr, "Could not write data to FIFO\n");
        return AVERROR_EXIT;
    }
    return 0;
}

/**
 * Read one audio frame from the input file, decode, convert and store
 * it in the FIFO buffer.
 * @param      fifo                 Buffer used for temporary storage
 * @param      input_format_context Format context of the input file
 * @param      input_codec_context  Codec context of the input file
 * @param      input_stream_index   Index of the audio stream to be decoded
 * @param      output_codec_context Codec context of the output file
 * @param      resampler_context    Resample context for the conversion
 * @param[out] finished             Indicates whether the end of file has
 *                                  been reached and all data has been
 *                                  decoded. If this flag is false,
 *                                  there is more data to be decoded,
 *                                  i.e., this function has to be called
 *                                  again.
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：流水线的上半段：读包 → 解码 → 转格式 → 放进 FIFO。每调用一次处理一个解码帧。
 * 流水线的下半段是 load_encode_and_write()：从 FIFO 取一帧 → 编码 → 写包。两段由 main() 里的两个 while 循环轮流驱动。
 */
static int read_decode_convert_and_store(AVAudioFifo *fifo,
                                         AVFormatContext *input_format_context,
                                         AVCodecContext *input_codec_context,
                                         int input_stream_index,
                                         AVCodecContext *output_codec_context,
                                         SwrContext *resampler_context,
                                         int *finished)
{
    /* Temporary storage of the input samples of the frame read from the file. */
    AVFrame *input_frame = NULL;
    /* Temporary storage for the converted input samples. */
    uint8_t **converted_input_samples = NULL;
    int data_present;
    int ret = AVERROR_EXIT;

    /* Initialize temporary storage for one input frame. */
    if (init_input_frame(&input_frame))
        goto cleanup;
    /* Decode one frame worth of audio samples. */
    if (decode_audio_frame(input_frame, input_format_context,
                           input_codec_context, input_stream_index,
                           &data_present, finished))
        goto cleanup;
    /* If we are at the end of the file and there are no more samples
     * in the decoder which are delayed, we are actually finished.
     * This must not be treated as an error. */
    if (*finished) {
        ret = 0;
        goto cleanup;
    }
    /* If there is decoded data, convert and store it. */
    if (data_present) {
        /* Initialize the temporary storage for the converted input samples. */
        if (init_converted_samples(&converted_input_samples, output_codec_context,
                                   input_frame->nb_samples))
            goto cleanup;

        /* Convert the input samples to the desired output sample format.
         * This requires a temporary storage provided by converted_input_samples. */
        if (convert_samples((const uint8_t**)input_frame->extended_data, converted_input_samples,
                            input_frame->nb_samples, resampler_context))
            goto cleanup;

        /* Add the converted input samples to the FIFO buffer for later processing. */
        if (add_samples_to_fifo(fifo, converted_input_samples,
                                input_frame->nb_samples))
            goto cleanup;
        ret = 0;
    }
    ret = 0;

cleanup:
    if (converted_input_samples)
        av_freep(&converted_input_samples[0]);
    av_freep(&converted_input_samples);
    av_frame_free(&input_frame);

    return ret;
}

/**
 * Initialize one input frame for writing to the output file.
 * The frame will be exactly frame_size samples large.
 * @param[out] frame                Frame to be initialized
 * @param      output_codec_context Codec context of the output file
 * @param      frame_size           Size of the frame
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：分配一个送给编码器的 AVFrame，里面能放 frame_size 个采样。要先设好 nb_samples、格式、采样率、声道布局，
 * av_frame_get_buffer() 才知道该分配多少内存。从 FIFO 取出来的采样会放进这个帧。
 */
static int init_output_frame(AVFrame **frame,
                             AVCodecContext *output_codec_context,
                             int frame_size)
{
    int error;

    /* Create a new frame to store the audio samples. */
    if (!(*frame = av_frame_alloc())) {
        fprintf(stderr, "Could not allocate output frame\n");
        return AVERROR_EXIT;
    }

    /* Set the frame's parameters, especially its size and format.
     * av_frame_get_buffer needs this to allocate memory for the
     * audio samples of the frame.
     * Default channel layouts based on the number of channels
     * are assumed for simplicity. */
    (*frame)->nb_samples     = frame_size;
    av_channel_layout_copy(&(*frame)->ch_layout, &output_codec_context->ch_layout);
    (*frame)->format         = output_codec_context->sample_fmt;
    (*frame)->sample_rate    = output_codec_context->sample_rate;

    /* Allocate the samples of the created frame. This call will make
     * sure that the audio frame can hold as many samples as specified. */
    if ((error = av_frame_get_buffer(*frame, 0)) < 0) {
        fprintf(stderr, "Could not allocate output frame samples (error '%s')\n",
                av_err2str(error));
        av_frame_free(frame);
        return error;
    }

    return 0;
}

/*
 * 课程注释：全局变量：已经送给编码器的采样总数。音频帧的 pts 是自己数出来的，每一帧的 pts 是前面所有帧的采样数之和，
 * 不用输入文件的时间戳（L14）。好处是输出的时间戳绝对均匀，不管输入的容器时间基多粗；
 * 代价是输入时间戳里的偏移和抖动都被忽略，输出总是从 0 开始、连续：比如 WMA 解码器有延迟，解出来的第一帧时间戳是 92 毫秒而不是 0；
 * 裸 AC-3 流的时间戳每帧比实际少 0.69 个 1/90000 时间单位，整个 5 秒的文件累计差了 49 个采样。ffmpeg 命令行则跟着这些时间戳走，两者在这两种输入上的输出不同（README 第 8 步）。
 */
/* Global timestamp for the audio frames. */
static int64_t pts = 0;

/**
 * Encode one frame worth of audio to the output file.
 * @param      frame                 Samples to be encoded
 * @param      output_format_context Format context of the output file
 * @param      output_codec_context  Codec context of the output file
 * @param[out] data_present          Indicates whether data has been
 *                                   encoded
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：送一帧给编码器，取最多一个包，写进文件。frame 为 NULL 表示冲刷编码器。
 * 这里一次只取一个包，AAC 编码器送一帧出一个包，没有问题；一次送帧可能出多个包的编码器要用循环取（L13）。
 */
static int encode_audio_frame(AVFrame *frame,
                              AVFormatContext *output_format_context,
                              AVCodecContext *output_codec_context,
                              int *data_present)
{
    /* Packet used for temporary storage. */
    AVPacket *output_packet;
    int error;

    error = init_packet(&output_packet);
    if (error < 0)
        return error;

    /* Set a timestamp based on the sample rate for the container. */
    if (frame) {
        frame->pts = pts;
        pts += frame->nb_samples;
    }

    *data_present = 0;
    /* Send the audio frame stored in the temporary packet to the encoder.
     * The output audio stream encoder is used to do this. */
    error = avcodec_send_frame(output_codec_context, frame);
    /* Check for errors, but proceed with fetching encoded samples if the
     *  encoder signals that it has nothing more to encode. */
    if (error < 0 && error != AVERROR_EOF) {
      fprintf(stderr, "Could not send packet for encoding (error '%s')\n",
              av_err2str(error));
      goto cleanup;
    }

    /* Receive one encoded frame from the encoder. */
    error = avcodec_receive_packet(output_codec_context, output_packet);
    /* If the encoder asks for more data to be able to provide an
     * encoded frame, return indicating that no data is present. */
    if (error == AVERROR(EAGAIN)) {
        error = 0;
        goto cleanup;
    /* If the last frame has been encoded, stop encoding. */
    } else if (error == AVERROR_EOF) {
        error = 0;
        goto cleanup;
    } else if (error < 0) {
        fprintf(stderr, "Could not encode frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    /* Default case: Return encoded data. */
    } else {
        *data_present = 1;
    }

    /*
     * 课程注释：patch 0078。把包的时间戳从编码器的时间基换到输出流的时间基，pts、dts、duration 一起换（L15）。
     * 原版没有这一步，MKV 的输出是 221.5 秒而不是 5 秒。
     */
    /* Convert the timestamps from the encoder's time base to the time base
     * the muxer has chosen for the stream. */
    if (*data_present) {
        av_packet_rescale_ts(output_packet, output_codec_context->time_base,
                             output_format_context->streams[0]->time_base);
        output_packet->stream_index = 0;
    }

    /* Write one audio frame from the temporary packet to the output file. */
    if (*data_present &&
        /*
         * 课程注释：写包。av_write_frame() 直接交给复用器，不排交织顺序；单路流不需要排序，用它没有问题。
         * 有多路流的时候要用 av_interleaved_write_frame()（L15、L16）。
         */
        (error = av_write_frame(output_format_context, output_packet)) < 0) {
        fprintf(stderr, "Could not write frame (error '%s')\n",
                av_err2str(error));
        goto cleanup;
    }

cleanup:
    av_packet_free(&output_packet);
    return error;
}

/**
 * Load one audio frame from the FIFO buffer, encode and write it to the
 * output file.
 * @param fifo                  Buffer used for temporary storage
 * @param output_format_context Format context of the output file
 * @param output_codec_context  Codec context of the output file
 * @return Error code (0 if successful)
 */
/*
 * 课程注释：流水线的下半段：从 FIFO 取一帧采样 → 编码 → 写包。取多少：FIFO 里够一整帧就取 frame_size 个；
 * 不够（输入读完了，剩下的零头）就有多少取多少，这就是最后一帧会比较短。AAC 编码器带 AV_CODEC_CAP_SMALL_LAST_FRAME，
 * 允许最后一帧短；没有这个标志的编码器要自己用静音补齐。
 */
static int load_encode_and_write(AVAudioFifo *fifo,
                                 AVFormatContext *output_format_context,
                                 AVCodecContext *output_codec_context)
{
    /* Temporary storage of the output samples of the frame written to the file. */
    AVFrame *output_frame;
    /* Use the maximum number of possible samples per frame.
     * If there is less than the maximum possible frame size in the FIFO
     * buffer use this number. Otherwise, use the maximum possible frame size. */
    const int frame_size = FFMIN(av_audio_fifo_size(fifo),
                                 output_codec_context->frame_size);
    int data_written;

    /* Initialize temporary storage for one output frame. */
    if (init_output_frame(&output_frame, output_codec_context, frame_size))
        return AVERROR_EXIT;

    /* Read as many samples from the FIFO buffer as required to fill the frame.
     * The samples are stored in the frame temporarily. */
    if (av_audio_fifo_read(fifo, (void **)output_frame->data, frame_size) < frame_size) {
        fprintf(stderr, "Could not read data from FIFO\n");
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }

    /* Encode one frame worth of audio samples. */
    if (encode_audio_frame(output_frame, output_format_context,
                           output_codec_context, &data_written)) {
        av_frame_free(&output_frame);
        return AVERROR_EXIT;
    }
    av_frame_free(&output_frame);
    return 0;
}

/**
 * Write the trailer of the output file container.
 * @param output_format_context Format context of the output file
 * @return Error code (0 if successful)
 */
static int write_output_file_trailer(AVFormatContext *output_format_context)
{
    int error;
    if ((error = av_write_trailer(output_format_context)) < 0) {
        fprintf(stderr, "Could not write output file trailer (error '%s')\n",
                av_err2str(error));
        return error;
    }
    return 0;
}

/*
 * 课程注释：主流程。初始化 → 两层循环 → 冲刷编码器 → 写文件尾。初始化的顺序有讲究：
 *     输入 → 输出（要用输入的采样率）→ 元数据 → 转换器（要用输入和输出的格式）→ FIFO（要用输出的格式）→ 文件头
 * 输出的编码器必须先打开，因为 frame_size 要等 avcodec_open2() 之后才知道，下面循环里的判断都靠它。
 */
int main(int argc, char **argv)
{
    AVFormatContext *input_format_context = NULL, *output_format_context = NULL;
    AVCodecContext *input_codec_context = NULL, *output_codec_context = NULL;
    SwrContext *resample_context = NULL;
    AVAudioFifo *fifo = NULL;
    int input_stream_index = 0;
    int ret = AVERROR_EXIT;

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input file> <output file>\n", argv[0]);
        exit(1);
    }

    /* Open the input file for reading. */
    if (open_input_file(argv[1], &input_format_context,
                        &input_codec_context, &input_stream_index))
        goto cleanup;
    /* Open the output file for writing. */
    if (open_output_file(argv[2], input_codec_context,
                         &output_format_context, &output_codec_context))
        goto cleanup;
    /* Copy the metadata (title, artist, ...) of the input file. */
    if (copy_metadata(input_format_context, input_stream_index,
                      output_format_context))
        goto cleanup;
    /* Initialize the resampler to be able to convert audio sample formats. */
    if (init_resampler(input_codec_context, output_codec_context,
                       &resample_context))
        goto cleanup;
    /* Initialize the FIFO buffer to store audio samples to be encoded. */
    if (init_fifo(&fifo, output_codec_context))
        goto cleanup;
    /* Write the header of the output file container. */
    if (write_output_file_header(output_format_context))
        goto cleanup;

    /*
     * 课程注释：外层循环的每一轮做两件事：先“攒”（解码、转换，放进 FIFO），再“切”（从 FIFO 取一帧一帧编码）。
     * 以 mp2 输入为例：解码器给 1152 个采样，FIFO 里就有 1152 个，编码器要 1024：
     *     第 1 轮  FIFO 1152 → 编码 1 帧 → 剩 128
     *     第 2 轮  FIFO 128 + 1152 = 1280 → 编码 1 帧 → 剩 256
     *     第 3 轮  FIFO 256 + 1152 = 1408 → 编码 1 帧 → 剩 384
     *     …
     * 每轮多剩 128 个，攒够 8 轮（1024 ÷ 128）就多出一整帧，所以平均 8 个解码帧编出 9 个 AAC 帧。
     */
    /* Loop as long as we have input samples to read or output samples
     * to write; abort as soon as we have neither. */
    while (1) {
        /* Use the encoder's desired frame size for processing. */
        const int output_frame_size = output_codec_context->frame_size;
        int finished                = 0;

        /* Make sure that there is one frame worth of samples in the FIFO
         * buffer so that the encoder can do its work.
         * Since the decoder's and the encoder's frame size may differ, we
         * need to FIFO buffer to store as many frames worth of input samples
         * that they make up at least one frame worth of output samples. */
        /* 课程注释：“攒”：FIFO 里不够一帧就继续解码，直到够了，或者输入读完了（finished）。 */
        while (av_audio_fifo_size(fifo) < output_frame_size) {
            /* Decode one frame worth of audio samples, convert it to the
             * output sample format and put it into the FIFO buffer. */
            if (read_decode_convert_and_store(fifo, input_format_context,
                                              input_codec_context,
                                              input_stream_index,
                                              output_codec_context,
                                              resample_context, &finished))
                goto cleanup;

            /* If we are at the end of the input file, we continue
             * encoding the remaining audio samples to the output file. */
            if (finished)
                break;
        }

        /* If we have enough samples for the encoder, we encode them.
         * At the end of the file, we pass the remaining samples to
         * the encoder. */
        /*
         * 课程注释：“切”：FIFO 里够一整帧就取出来编码，一次可能取好几帧。输入读完以后（finished），
         * 零头不够一帧也要取出来编码，不然最后的几百个采样就丢了（patch 0079 之前，WMA 文件还会因为解码器里的帧没取完丢得更多）。
         */
        while (av_audio_fifo_size(fifo) >= output_frame_size ||
               (finished && av_audio_fifo_size(fifo) > 0))
            /* Take one frame worth of audio samples from the FIFO buffer,
             * encode it and write it to the output file. */
            if (load_encode_and_write(fifo, output_format_context,
                                      output_codec_context))
                goto cleanup;

        /* If we are at the end of the input file and have encoded
         * all remaining samples, we can exit this loop and finish. */
        if (finished) {
            int data_written;
            /*
             * 课程注释：冲刷编码器：送 NULL，一直取到没有包出来为止（L13）。AAC 编码器有 1024 个采样的延迟，
             * 所以编码器输出的包数总比送进去的帧数多一个：216 帧 → 217 个包。多出来的那个是文件开头的“预热包”，
             * 它的 pts 是 -1024，MP4 靠编辑列表告诉播放器跳过它，所以它不算进文件的时长。
             */
            /* Flush the encoder as it may have delayed frames. */
            do {
                if (encode_audio_frame(NULL, output_format_context,
                                       output_codec_context, &data_written))
                    goto cleanup;
            } while (data_written);
            break;
        }
    }

    /* Write the trailer of the output file container. */
    if (write_output_file_trailer(output_format_context))
        goto cleanup;
    ret = 0;

cleanup:
    if (fifo)
        av_audio_fifo_free(fifo);
    swr_free(&resample_context);
    if (output_codec_context)
        avcodec_free_context(&output_codec_context);
    if (output_format_context) {
        /*
         * 课程注释：关闭输出文件。缓冲区里剩下的数据是这时候才真正写出去的，磁盘满了就在这里失败，所以返回值本来应该检查，
         * 这个示例没检查。transcode_aac_av.c 检查了（L16 的 transcode.c 的 patch 0072 补的也是这个）。
         */
        avio_closep(&output_format_context->pb);
        avformat_free_context(output_format_context);
    }
    if (input_codec_context)
        avcodec_free_context(&input_codec_context);
    if (input_format_context)
        avformat_close_input(&input_format_context);

    return ret;
}
