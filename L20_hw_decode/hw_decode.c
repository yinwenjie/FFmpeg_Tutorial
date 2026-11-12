/*
 * Copyright (c) 2017 Jun Zhao
 * Copyright (c) 2017 Kaixuan Liu
 *
 * HW Acceleration API (video decoding) decode sample
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
 * @file HW-accelerated decoding API usage.example
 * @example hw_decode.c
 *
 * Perform HW-accelerated decoding with output frames from HW video
 * surfaces.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/hw_decode.c 的拷贝，代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它让显卡解码一个视频，把解出来的帧从显存搬回内存，写成原始像素文件：
 *     hw_decode <设备类型> <输入文件> <输出文件>        例：hw_decode vaapi small_360p25.mp4 out.nv12
 * 注意：拷贝的不是提交 a337e8d 的原版，而是打了 3 个 patch（0096 到 0098）的版本，改动过的地方下面都标了编号：
 *     0096  出错时泄漏所有资源，退出码是 255，解码开始以后退出码永远是 0
 *     0097  创建硬件设备失败时只说一句“失败”，没有错误码，也不说原因
 *     0098  fwrite() 的符号比较警告；缓冲区大小出错时没检查
 * 程序结构（和 L07 的软件解码比，多了“设备”和“搬回内存”两件事）：
 *     hw_decoder_init()  创建硬件设备（显卡的一个会话），交给解码器
 *     get_hw_format()    解码器问“用哪种像素格式”，回答硬件格式（例如 vaapi），解码器就把帧放在显存里
 *     decode_write()     送包、取帧；取到的帧在显存里，用 av_hwframe_transfer_data() 搬回内存再写出
 *     main()             找解码器支持的硬件配置、开解码器、读包解码、冲刷
 * 本机没有显卡：这份示例能编译，也能跑到“创建硬件设备”那一步，然后报错退出（README 第 2 步）；
 * 真正解码的部分没有在本机运行过，README 里标了“待补”。自己写程序时参考本目录的 hw_decode_av.c，它没有显卡时会退回软件解码。
 */

#include <errno.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/avassert.h>
#include <libavutil/imgutils.h>

/*
 * 课程注释：三个全局变量。hw_device_ctx 是硬件设备（显卡的一个会话）的引用；
 * hw_pix_fmt 是“放在显存里的帧”的像素格式，例如 vaapi、cuda、qsv，它不是 nv12 这样的真实像素排列，
 * 只是一个标记：帧的 data[] 里放的是显存里表面的句柄，而不是内存里的像素地址，所以不能直接当图像读；
 * output_file 是输出文件。
 */
static AVBufferRef *hw_device_ctx = NULL;
static enum AVPixelFormat hw_pix_fmt;
static FILE *output_file = NULL;

/*
 * 课程注释：创建硬件设备，交给解码器。第 3 个参数（设备名）是 NULL，意思是用默认设备（第 4 个参数是选项，也是 NULL）：
 * VAAPI 是 /dev/dri/renderD128，也可以传别的路径（有两块显卡时）；cuda 是第 0 块显卡。
 * 创建失败是最常见的第一个问题：没有显卡、没有驱动、没有读写 /dev/dri 的权限（要在 render 用户组里）、
 * 在没有直通显卡的虚拟机或容器里。patch 0097 以前这里只说一句“失败”，原因要提高日志级别才看得到。
 * 然后 av_buffer_ref() 给解码器一个引用（引用计数加一）：设备要等解码器和我们两边都不用了才会真正释放。
 */
static int hw_decoder_init(AVCodecContext *ctx, const enum AVHWDeviceType type)
{
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&hw_device_ctx, type,
                                      NULL, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device: %s\n"
                "Raise the log level (av_log_set_level(AV_LOG_VERBOSE)) to see "
                "the reason reported by the hardware backend.\n", av_err2str(err));
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    if (!ctx->hw_device_ctx)
        return AVERROR(ENOMEM);

    return err;
}

/*
 * 课程注释：get_format 回调。解码器开始解第一帧之前问我们：“这个视频你想用哪种像素格式？”，把能用的格式列成一个
 * 以 -1 结尾的数组：如果硬件能解这个 profile，硬件格式（比如 vaapi）排在前面，后面跟着软件格式（yuv420p）。
 * 我们在里面找 main() 里查到的 hw_pix_fmt，找到就选它，解码器就在显卡上解、帧留在显存里。
 * 找不到（比如这块显卡不支持 10 位的 HEVC，或者视频的 profile 太新）时示例返回 NONE，解码直接失败；
 * 另一种做法是退回软件格式，让 CPU 来解（hw_decode_av.c 就是这样做的）。
 */
static enum AVPixelFormat get_hw_format(AVCodecContext *ctx,
                                        const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != -1; p++) {
        if (*p == hw_pix_fmt)
            return *p;
    }

    fprintf(stderr, "Failed to get HW surface format.\n");
    return AV_PIX_FMT_NONE;
}

/*
 * 课程注释：送一个包（packet 是 NULL 表示冲刷），把解出来的帧全部取出来写到文件。
 * 取帧的循环和 L07 一样：EAGAIN 是“还要再送包”，EOF 是“冲刷完了”，都是正常结束。
 * 每一轮都 av_frame_alloc() 两个帧再释放，不是好做法（自己的程序里放到循环外面，用 av_frame_unref() 复用），示例里图个简单。
 */
static int decode_write(AVCodecContext *avctx, AVPacket *packet)
{
    AVFrame *frame = NULL, *sw_frame = NULL;
    AVFrame *tmp_frame = NULL;
    uint8_t *buffer = NULL;
    int size;
    int ret = 0;

    ret = avcodec_send_packet(avctx, packet);
    if (ret < 0) {
        fprintf(stderr, "Error during decoding\n");
        return ret;
    }

    while (1) {
        if (!(frame = av_frame_alloc()) || !(sw_frame = av_frame_alloc())) {
            fprintf(stderr, "Can not alloc frame\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }

        ret = avcodec_receive_frame(avctx, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_frame_free(&frame);
            av_frame_free(&sw_frame);
            return 0;
        } else if (ret < 0) {
            fprintf(stderr, "Error while decoding\n");
            goto fail;
        }

        /*
         * 课程注释：frame->format 是硬件格式，说明这一帧在显存里。av_hwframe_transfer_data() 把它拷回内存，
         * 放到 sw_frame 里。sw_frame->format 没有设置，函数就选这种硬件帧支持的第一种内存格式：
         * VAAPI 一般是 nv12（YUV 4:2:0，Y 平面加一个 UV 交错的平面），兑现 L9 说过的“硬件解码出来的是 nv12”。
         * 想要别的格式（比如 yuv420p）有两个办法：事先设好 sw_frame->format（必须是硬件支持的格式之一，
         * 用 av_hwframe_transfer_get_formats() 查），或者拷回来以后用 libswscale 转（L9）。
         * 这一步要等显卡把这帧解完，再把整帧通过 PCIe 总线拷回内存：它是同步的，也是硬件解码最贵的一步之一。
         * 如果后面接的是显卡上的处理（缩放、编码，L21），就不要搬回来，帧一直留在显存里。
         */
        if (frame->format == hw_pix_fmt) {
            /* retrieve data from GPU to CPU */
            if ((ret = av_hwframe_transfer_data(sw_frame, frame, 0)) < 0) {
                fprintf(stderr, "Error transferring the data to system memory\n");
                goto fail;
            }
            tmp_frame = sw_frame;
        } else
            tmp_frame = frame;

        /* 课程注释：把按行距排列的帧拷成紧密排列（对齐 1 字节）的一块内存再写出，原始像素文件里没有行距的填充，和 L5 一样。 */
        size = av_image_get_buffer_size(tmp_frame->format, tmp_frame->width,
                                        tmp_frame->height, 1);
        if (size < 0) {
            fprintf(stderr, "Can not get the size of the frame\n");
            ret = size;
            goto fail;
        }
        buffer = av_malloc(size);
        if (!buffer) {
            fprintf(stderr, "Can not alloc buffer\n");
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        ret = av_image_copy_to_buffer(buffer, size,
                                      (const uint8_t * const *)tmp_frame->data,
                                      (const int *)tmp_frame->linesize, tmp_frame->format,
                                      tmp_frame->width, tmp_frame->height, 1);
        if (ret < 0) {
            fprintf(stderr, "Can not copy image to buffer\n");
            goto fail;
        }

        if (fwrite(buffer, 1, size, output_file) != (size_t)size) {
            fprintf(stderr, "Failed to dump raw data.\n");
            ret = AVERROR(EIO);
            goto fail;
        }

    fail:
        av_frame_free(&frame);
        av_frame_free(&sw_frame);
        av_freep(&buffer);
        if (ret < 0)
            return ret;
    }
}

int main(int argc, char *argv[])
{
    AVFormatContext *input_ctx = NULL;
    int video_stream, ret;
    AVStream *video = NULL;
    AVCodecContext *decoder_ctx = NULL;
    const AVCodec *decoder = NULL;
    AVPacket *packet = NULL;
    enum AVHWDeviceType type;
    int i;

    /*
     * 课程注释：patch 0096。原版里每一个启动错误都是中途 return -1：包、格式上下文、解码器上下文、硬件设备全部泄漏，
     * 退出码是 255；而开始解码以后，最后无条件 return 0，读包、解码、写文件出错也是成功。
     * 现在失败都跳到 end 统一释放，失败返回 1，成功返回 0，和其他示例一致。
     */
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <device type> <input file> <output file>\n", argv[0]);
        return 1;
    }

    /* 课程注释：设备类型的名字 → 枚举。名字是这份 FFmpeg 编译进来的，下面不支持时会列出全部（本机：vaapi、drm）。 */
    type = av_hwdevice_find_type_by_name(argv[1]);
    if (type == AV_HWDEVICE_TYPE_NONE) {
        fprintf(stderr, "Device type %s is not supported.\n", argv[1]);
        fprintf(stderr, "Available device types:");
        while((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE)
            fprintf(stderr, " %s", av_hwdevice_get_type_name(type));
        fprintf(stderr, "\n");
        return 1;
    }

    packet = av_packet_alloc();
    if (!packet) {
        fprintf(stderr, "Failed to allocate AVPacket\n");
        return 1;
    }

    /* open the input file */
    ret = avformat_open_input(&input_ctx, argv[2], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot open input file '%s': %s\n", argv[2], av_err2str(ret));
        goto end;
    }

    ret = avformat_find_stream_info(input_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Cannot find input stream information.\n");
        goto end;
    }

    /* find the video stream information */
    ret = av_find_best_stream(input_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        fprintf(stderr, "Cannot find a video stream in the input file\n");
        goto end;
    }
    video_stream = ret;

    /*
     * 课程注释：查“这个解码器支持哪些硬件配置”。avcodec_get_hw_config() 一项一项返回，返回 NULL 就是没有了。
     * 每一项说明：设备类型（device_type）、这种配置下帧的像素格式（pix_fmt）、使用的方法（methods）。
     * 同一个 H.264 解码器在 FFmpeg 里可以有好几项：vaapi、cuda、qsv、vdpau、drm、vulkan……，只有编译时带了、机器上也有才能用。
     * HW_DEVICE_CTX 这个方法的意思是：我们给解码器一个设备（设备上下文），它自己建显存里的帧池，这是最常用的一种。
     * 另一种 HW_FRAMES_CTX 要我们自己建帧上下文（L21 的 vaapi_encode 会用到）。
     * 本目录的 hw_info 把所有解码器的这张表都打印出来。
     */
    for (i = 0;; i++) {
        const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
        if (!config) {
            fprintf(stderr, "Decoder %s does not support device type %s.\n",
                    decoder->name, av_hwdevice_get_type_name(type));
            ret = AVERROR(ENOSYS);
            goto end;
        }
        if (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
            config->device_type == type) {
            hw_pix_fmt = config->pix_fmt;
            break;
        }
    }

    if (!(decoder_ctx = avcodec_alloc_context3(decoder))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    video = input_ctx->streams[video_stream];
    ret = avcodec_parameters_to_context(decoder_ctx, video->codecpar);
    if (ret < 0)
        goto end;

    /* 课程注释：顺序：先设 get_format 和设备，再 avcodec_open2()。设备要在打开解码器之前给它。 */
    decoder_ctx->get_format  = get_hw_format;

    if ((ret = hw_decoder_init(decoder_ctx, type)) < 0)
        goto end;

    if ((ret = avcodec_open2(decoder_ctx, decoder, NULL)) < 0) {
        fprintf(stderr, "Failed to open codec for stream #%u\n", video_stream);
        goto end;
    }

    /* open the file to dump raw data */
    output_file = fopen(argv[3], "w+b");
    if (!output_file) {
        fprintf(stderr, "Cannot open output file '%s'\n", argv[3]);
        ret = AVERROR(errno);
        goto end;
    }

    /* actual decoding and dump the raw data */
    while (ret >= 0) {
        if ((ret = av_read_frame(input_ctx, packet)) < 0)
            break;

        if (video_stream == packet->stream_index)
            ret = decode_write(decoder_ctx, packet);

        av_packet_unref(packet);
    }
    if (ret == AVERROR_EOF)
        ret = 0;

    /* 课程注释：冲刷。显卡的解码器里同样有延迟的帧（B 帧重排，L7、L8），不冲刷就丢了最后几帧。 */
    /* flush the decoder */
    if (ret >= 0)
        ret = decode_write(decoder_ctx, NULL);

end:
    if (output_file && fclose(output_file) && ret >= 0) {
        fprintf(stderr, "Failed to write the output file\n");
        ret = AVERROR(EIO);
    }
    av_packet_free(&packet);
    avcodec_free_context(&decoder_ctx);
    avformat_close_input(&input_ctx);
    av_buffer_unref(&hw_device_ctx);

    return ret < 0 ? 1 : 0;
}
