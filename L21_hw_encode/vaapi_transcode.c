/*
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
 * @file Intel VAAPI-accelerated transcoding API usage example
 * @example vaapi_transcode.c
 *
 * Perform VAAPI-accelerated transcoding.
 * Usage: vaapi_transcode input_stream codec output_stream
 * e.g: - vaapi_transcode input.mp4 h264_vaapi output_h264.mp4
 *      - vaapi_transcode input.mp4 vp9_vaapi output_vp9.ivf
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/vaapi_transcode.c 的拷贝，代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它让显卡解码、显卡编码，帧从头到尾留在显存里，一次也不搬回内存：
 *     vaapi_transcode <输入> <编码器名> <输出>        例：vaapi_transcode in.mp4 h264_vaapi out.mp4
 * 注意：拷贝的不是提交 a337e8d 的原版，而是打了 2 个 patch（0101、0102）的版本，改动过的地方下面都标了编号：
 *     0101  编码器的时间基没有设：平均帧率、时长、码率都不对（实测 1080p 的码率 10.9 变 5.9 Mb/s 是修复前的虚高）
 *     0102  出错处理：读包、解码、写文件出错时退出码和清理不对，没有解码出一帧就崩溃
 * 和 L16 的软件转码相比，这里没有滤镜、没有格式转换：解码器输出的硬件帧直接交给编码器，二者共用同一个显存表面池。
 * 程序结构：
 *     open_input_file()  打开输入，建解码器，把设备交给解码器，get_format 里选 vaapi
 *     dec_enc()          送包、取帧；第一帧解出来时才建编码器（要用解码器的帧上下文），然后把帧送去编码
 *     encode_write()     送帧、取包、写文件
 *     main()             建设备、读包、冲刷解码器、冲刷编码器、写文件尾
 * 本机没有显卡：这份示例能编译，也能跑到“创建 VAAPI 设备”那一步然后报错退出；
 * 解码、编码的部分没有在本机运行过，README 里标了“待补”。
 */

#include <stdio.h>
#include <errno.h>

#include <libavutil/hwcontext.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

/*
 * 课程注释：输入和输出的格式上下文（L3、L15）；硬件设备；解码器和编码器的上下文；video_stream 是输入里视频流的序号；
 * ost 是输出的视频流；initialized 表示编码器和输出文件头已经建好（第一帧解出来才建，见 dec_enc）。
 */
static AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
static AVBufferRef *hw_device_ctx = NULL;
static AVCodecContext *decoder_ctx = NULL, *encoder_ctx = NULL;
static int video_stream = -1;
static AVStream *ost;
static int initialized = 0;

/*
 * 课程注释：和 L20 的 get_format 一样，选 vaapi 格式；挑不到就失败。
 * 和我们 hw_decode_av 的区别：这里没有“退回软件格式”，显卡解不了这个视频时程序直接结束。
 */
static enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }

    fprintf(stderr, "Unable to decode this file using VA-API.\n");
    return AV_PIX_FMT_NONE;
}

static int open_input_file(const char *filename)
{
    int ret;
    const AVCodec *decoder = NULL;
    AVStream *video = NULL;

    if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0) {
        fprintf(stderr, "Cannot open input file '%s', Error code: %s\n",
                filename, av_err2str(ret));
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) {
        fprintf(stderr, "Cannot find input stream information. Error code: %s\n",
                av_err2str(ret));
        return ret;
    }

    ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file. "
                "Error code: %s\n", av_err2str(ret));
        return ret;
    }
    video_stream = ret;

    if (!(decoder_ctx = avcodec_alloc_context3(decoder)))
        return AVERROR(ENOMEM);

    video = ifmt_ctx->streams[video_stream];
    if ((ret = avcodec_parameters_to_context(decoder_ctx, video->codecpar)) < 0) {
        fprintf(stderr, "avcodec_parameters_to_context error. Error code: %s\n",
                av_err2str(ret));
        return ret;
    }

    /* the timestamps of the decoded frames are in the time base of the stream,
     * and the frame rate is needed below to set up the encoder */
    /*
     * 课程注释（patch 0101）：解码出来的帧的 pts 用流的时间基（输入文件里的单位，例如 MP4 的 1/12800）表示；
     * 告诉解码器这个时间基，帧里的时间戳才有意义；帧率用来给编码器设时间基（见 dec_enc）。
     */
    decoder_ctx->pkt_timebase = video->time_base;
    decoder_ctx->framerate    = av_guess_frame_rate(ifmt_ctx, video, NULL);

    /*
     * 课程注释：把设备交给解码器，和 L20 的 hw_decoder_init 一样；解码器自己建帧池（解码出来的帧都在显存里）。
     */
    decoder_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    if (!decoder_ctx->hw_device_ctx) {
        fprintf(stderr, "A hardware device reference create failed.\n");
        return AVERROR(ENOMEM);
    }
    decoder_ctx->get_format    = get_vaapi_format;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0)
        fprintf(stderr, "Failed to open codec for decoding. Error code: %s\n",
                av_err2str(ret));

    return ret;
}

/*
 * 课程注释：送一帧给编码器，把能取到的包都取出来，换算时间戳，写进输出文件。
 * 编码器取出的包的时间戳用的是编码器的时间基（1 / 帧率），输出流的时间基由复用器决定（MP4 是 1/12800 一类的数），
 * 所以要 av_packet_rescale_ts()（L15）。
 */
static int encode_write(AVPacket *enc_pkt, AVFrame *frame)
{
    int ret = 0;

    av_packet_unref(enc_pkt);

    if ((ret = avcodec_send_frame(encoder_ctx, frame)) < 0) {
        fprintf(stderr, "Error during encoding. Error code: %s\n", av_err2str(ret));
        goto end;
    }
    while (1) {
        ret = avcodec_receive_packet(encoder_ctx, enc_pkt);
        if (ret)
            break;

        enc_pkt->stream_index = 0;
        /* the encoder does not set the duration of the packets: one frame
         * lasts one tick of the encoder time base (1 / frame rate) */
        /*
         * 课程注释（patch 0101）：编码器不给包设时长，复用器要写时长（MP4 的 stts）：一帧就是编码器时间基的 1 个单位。
         */
        if (enc_pkt->duration <= 0)
            enc_pkt->duration = 1;
        /*
         * 课程注释（patch 0101）：原版这里用的是输入流的时间基，而编码器的时间基没有设，所以换算是错的：
         * 平均帧率从 3125/124（应当是 25/1）、时长 4.96 秒（应当是 5.0）、码率 1.32 Mb/s（应当是 0.75）。这些数字是修复前后实测的，
         * 但修复前后都是在“用软件编码器代替硬件编码器”的替身环境里量的，README 第 3 步有说明。
         */
        av_packet_rescale_ts(enc_pkt, encoder_ctx->time_base,
                             ofmt_ctx->streams[0]->time_base);
        ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
        if (ret < 0) {
            fprintf(stderr, "Error during writing data to output file. "
                    "Error code: %s\n", av_err2str(ret));
            return -1;
        }
    }

end:
    /*
     * 课程注释（patch 0102）：读包循环以 AVERROR_EOF 结束是正常的，不是错误；原版会把它一路带到退出码。
     */
    if (ret == AVERROR_EOF)
        return 0;
    ret = ((ret == AVERROR(EAGAIN)) ? 0:-1);
    return ret;
}

/*
 * 课程注释：送一个包给解码器，取出所有的帧，每一帧送去编码。包传 NULL 是冲刷解码器。
 * 这是“全 GPU 转码”的核心：解码出来的硬件帧不搬回内存，直接送进编码器。
 */
static int dec_enc(AVPacket *pkt, const AVCodec *enc_codec)
{
    AVFrame *frame;
    int ret = 0;

    ret = avcodec_send_packet(decoder_ctx, pkt);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding. Error code: %s\n", av_err2str(ret));
        return ret;
    }

    while (ret >= 0) {
        if (!(frame = av_frame_alloc()))
            return AVERROR(ENOMEM);

        ret = avcodec_receive_frame(decoder_ctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding. Error code: %s\n", av_err2str(ret));
            goto fail;
        }

        /*
         * 课程注释：编码器要等第一帧解出来才能建，原因是帧上下文（显存表面池）是解码器建的，只有拿到第一帧才有。
         * 编码器直接引用解码器的 hw_frames_ctx：解码出来的表面和编码器要的表面是同一个池里的，这样帧才能“留在显存里”，不用上传也不用下载。
         * 这也带来一个限制（示例自己的注释写了）：中途分辨率变化时没法处理，因为编码器已经按第一帧的尺寸打开了。
         */
        if (!initialized) {
            /* we need to ref hw_frames_ctx of decoder to initialize encoder's codec.
               Only after we get a decoded frame, can we obtain its hw_frames_ctx */
            encoder_ctx->hw_frames_ctx = av_buffer_ref(decoder_ctx->hw_frames_ctx);
            if (!encoder_ctx->hw_frames_ctx) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            /* set AVCodecContext Parameters for encoder, here we keep them stay
             * the same as decoder.
             * xxx: now the sample can't handle resolution change case.
             */
            encoder_ctx->framerate = decoder_ctx->framerate;
            encoder_ctx->time_base = av_inv_q(decoder_ctx->framerate);
            encoder_ctx->pix_fmt   = AV_PIX_FMT_VAAPI;
            encoder_ctx->width     = decoder_ctx->width;
            encoder_ctx->height    = decoder_ctx->height;

            if ((ret = avcodec_open2(encoder_ctx, enc_codec, NULL)) < 0) {
                fprintf(stderr, "Failed to open encode codec. Error code: %s\n",
                        av_err2str(ret));
                goto fail;
            }

            if (!(ost = avformat_new_stream(ofmt_ctx, enc_codec))) {
                fprintf(stderr, "Failed to allocate stream for output format.\n");
                ret = AVERROR(ENOMEM);
                goto fail;
            }

            ost->time_base = encoder_ctx->time_base;
            ret = avcodec_parameters_from_context(ost->codecpar, encoder_ctx);
            if (ret < 0) {
                fprintf(stderr, "Failed to copy the stream parameters. "
                        "Error code: %s\n", av_err2str(ret));
                goto fail;
            }

            /* write the stream header */
            if ((ret = avformat_write_header(ofmt_ctx, NULL)) < 0) {
                fprintf(stderr, "Error while writing stream header. "
                        "Error code: %s\n", av_err2str(ret));
                goto fail;
            }

            initialized = 1;
        }

        /* the encoder expects timestamps in its own time base */
        /*
         * 课程注释（patch 0101）：帧的 pts 从流的时间基换算成编码器的时间基，编码器才知道每一帧在什么时候。
         */
        frame->pts      = av_rescale_q(frame->pts, decoder_ctx->pkt_timebase,
                                       encoder_ctx->time_base);
        frame->duration = av_rescale_q(frame->duration, decoder_ctx->pkt_timebase,
                                       encoder_ctx->time_base);

        if ((ret = encode_write(pkt, frame)) < 0)
            fprintf(stderr, "Error during encoding and writing.\n");

fail:
        av_frame_free(&frame);
    }
    return ret;
}

/*
 * 课程注释：主流程。建设备 -> 打开输入、建解码器 -> 找编码器、建输出 -> 读包、解码、编码、写文件。
 * patch 0102 改的是出错处理：几处出错路径不再丢掉错误码；冲刷解码器、冲刷编码器、写文件尾，每一步的错误都记下来（返回第一个）；
 * 一帧都没解码出来时不再对没有写过文件头的输出调 av_write_trailer()（会崩溃）；退出码统一成 1。
 */
int main(int argc, char **argv)
{
    const AVCodec *enc_codec;
    int ret = 0, err;
    AVPacket *dec_pkt;

    if (argc != 4) {
        fprintf(stderr, "Usage: %s <input file> <encode codec> <output file>\n"
                "The output format is guessed according to the file extension.\n"
                "\n", argv[0]);
        return 1;
    }

    ret = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0);
    if (ret < 0) {
        fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(ret));
        return 1;
    }

    dec_pkt = av_packet_alloc();
    if (!dec_pkt) {
        fprintf(stderr, "Failed to allocate decode packet\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = open_input_file(argv[1])) < 0)
        goto end;

    if (!(enc_codec = avcodec_find_encoder_by_name(argv[2]))) {
        fprintf(stderr, "Could not find encoder '%s'\n", argv[2]);
        ret = AVERROR_ENCODER_NOT_FOUND;
        goto end;
    }

    if ((ret = (avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, argv[3]))) < 0) {
        fprintf(stderr, "Failed to deduce output format from file extension. Error code: "
                "%s\n", av_err2str(ret));
        goto end;
    }

    if (!(encoder_ctx = avcodec_alloc_context3(enc_codec))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    ret = avio_open(&ofmt_ctx->pb, argv[3], AVIO_FLAG_WRITE);
    if (ret < 0) {
        fprintf(stderr, "Cannot open output file. "
                "Error code: %s\n", av_err2str(ret));
        goto end;
    }

    /* read all packets and only transcoding video */
    while (ret >= 0) {
        if ((ret = av_read_frame(ifmt_ctx, dec_pkt)) < 0)
            break;

        if (video_stream == dec_pkt->stream_index)
            ret = dec_enc(dec_pkt, enc_codec);

        av_packet_unref(dec_pkt);
    }
    if (ret == AVERROR_EOF)
        ret = 0;

    /* flush decoder; even after an error, finish the output with the frames
     * that were decoded, but report the first error */
    av_packet_unref(dec_pkt);
    err = dec_enc(dec_pkt, enc_codec);
    if (ret >= 0)
        ret = err;

    if (initialized) {
        /* flush encoder */
        err = encode_write(dec_pkt, NULL);
        if (ret >= 0)
            ret = err;

        /* write the trailer for output stream */
        err = av_write_trailer(ofmt_ctx);
        if (err < 0)
            fprintf(stderr, "Error while writing the trailer. Error code: %s\n",
                    av_err2str(err));
        if (ret >= 0)
            ret = err;
    } else if (ret >= 0) {
        /* the encoder and the output header are set up when the first frame is
         * decoded; without a frame there is nothing to flush or to finish
         * (calling av_write_trailer() without a header crashes) */
        fprintf(stderr, "No video frame could be decoded.\n");
        ret = AVERROR_INVALIDDATA;
    }

end:
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        err = avio_closep(&ofmt_ctx->pb);
        if (err < 0 && ret >= 0) {
            fprintf(stderr, "Error while closing the output file. Error code: %s\n",
                    av_err2str(err));
            ret = err;
        }
    }
    avformat_free_context(ofmt_ctx);
    avcodec_free_context(&decoder_ctx);
    avcodec_free_context(&encoder_ctx);
    av_buffer_unref(&hw_device_ctx);
    av_packet_free(&dec_pkt);
    return ret < 0 ? 1 : 0;
}
