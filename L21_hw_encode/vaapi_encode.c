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
 * @file Intel VAAPI-accelerated encoding API usage example
 * @example vaapi_encode.c
 *
 * Perform VAAPI-accelerated encoding. Read input from an NV12 raw
 * file, and write the H.264 encoded data to an output raw file.
 * Usage: vaapi_encode 1920 1080 input.yuv output.h264
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/vaapi_encode.c 的拷贝，代码一行未改，只加了以“课程注释”开头的中文注释。
 * 它读一个 NV12 原始像素文件，每一帧先“上传”到显存（内存 -> 显存），让显卡的 H.264 编码器编码，写成裸 H.264 码流：
 *     vaapi_encode <宽> <高> <输入.nv12> <输出.h264>        例：vaapi_encode 640 360 in.nv12 out.h264
 * 注意：拷贝的不是提交 a337e8d 的原版，而是打了 2 个 patch（0099、0100）的版本，改动过的地方下面都标了编号：
 *     0099  按“宽 x 高”直接往 AVFrame 里读数据，忽略了每一行末尾的填充（linesize 大于宽）：宽不是 32 或 64 的倍数时，画面是歪的
 *     0100  出错时的退出码五花八门（崩溃、0、183、254、255），改成统一的 1
 * 和 L20 的方向相反：L20 是把帧从显存“下载”回内存，这里是把内存里的帧“上传”到显存。程序结构：
 *     set_hwframe_ctx()  自己建“帧上下文”（显存里的表面池），交给编码器：解码时这个由解码器自己建，编码时要我们自己建
 *     main()             建设备、开编码器、读一帧 -> 拿一个显存表面 -> 上传 -> 编码，最后冲刷
 *     encode_write()     送帧、取包、写文件（和 L13 的软件编码一样）
 * 本机没有显卡：这份示例能编译，也能跑到“创建 VAAPI 设备”那一步然后报错退出；
 * 上传、编码的部分没有在本机运行过，README 里标了“待补”。自己写程序时参考本目录的 hw_encode_av.c。
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>

/*
 * 课程注释：width、height 是输入帧的宽和高，从命令行来；hw_device_ctx 是硬件设备（显卡的一个会话，L20 讲过）。
 */
static int width, height;
static AVBufferRef *hw_device_ctx = NULL;

/*
 * 课程注释：给编码器建“帧上下文”（AVHWFramesContext）：显存里的一个表面池。
 * 解码时（L20）解码器自己建池，我们只给设备；编码时编码器需要知道“送进来的帧是什么样”，这要我们自己建、自己交给它：
 *     format     = AV_PIX_FMT_VAAPI   帧的硬件格式（标记：这一帧在 VAAPI 的表面里）
 *     sw_format  = AV_PIX_FMT_NV12    表面里像素的排列方式（内存里对应的格式）：上传时来源帧必须是它
 *     width/height                     表面的尺寸
 *     initial_pool_size = 20          池里一开始分配的表面个数。示例随手写了 20：池太小时 av_hwframe_get_buffer() 会取不到，
 *                                     编码器自己也会扣住一些帧（参考帧、重排）。真正需要多少，没有在本机验证（README 里“待补”）
 * av_hwframe_ctx_init() 才真正在显存里分配这些表面，所以失败（显存不够、尺寸不支持）是在这里发现的。
 */
static int set_hwframe_ctx(AVCodecContext *ctx, AVBufferRef *hw_device_ctx)
{
    AVBufferRef *hw_frames_ref;
    AVHWFramesContext *frames_ctx = NULL;
    int err = 0;

    if (!(hw_frames_ref = av_hwframe_ctx_alloc(hw_device_ctx))) {
        fprintf(stderr, "Failed to create VAAPI frame context.\n");
        return -1;
    }
    frames_ctx = (AVHWFramesContext *)(hw_frames_ref->data);
    frames_ctx->format    = AV_PIX_FMT_VAAPI;
    frames_ctx->sw_format = AV_PIX_FMT_NV12;
    frames_ctx->width     = width;
    frames_ctx->height    = height;
    frames_ctx->initial_pool_size = 20;
    if ((err = av_hwframe_ctx_init(hw_frames_ref)) < 0) {
        fprintf(stderr, "Failed to initialize VAAPI frame context."
                "Error code: %s\n",av_err2str(err));
        av_buffer_unref(&hw_frames_ref);
        return err;
    }
    ctx->hw_frames_ctx = av_buffer_ref(hw_frames_ref);
    if (!ctx->hw_frames_ctx)
        err = AVERROR(ENOMEM);

    av_buffer_unref(&hw_frames_ref);
    return err;
}

/*
 * 课程注释：送一帧给编码器，把能取到的包都取出来写进文件（和 L13 的软件编码一样）。
 * frame 传 NULL 表示“没有更多的帧了”，是冲刷。送进来的是硬件帧，包是普通的内存包：压缩后的数据本来就是字节流。
 * 返回值：>= 0 表示正常；AVERROR(EAGAIN) 表示编码器要更多的帧（正常）；AVERROR_EOF 表示冲刷结束；其他是错误。
 */
static int encode_write(AVCodecContext *avctx, AVFrame *frame, FILE *fout)
{
    int ret = 0;
    AVPacket *enc_pkt;

    if (!(enc_pkt = av_packet_alloc()))
        return AVERROR(ENOMEM);

    if ((ret = avcodec_send_frame(avctx, frame)) < 0) {
        fprintf(stderr, "Error code: %s\n", av_err2str(ret));
        goto end;
    }
    while (1) {
        ret = avcodec_receive_packet(avctx, enc_pkt);
        if (ret)
            break;

        enc_pkt->stream_index = 0;
        ret = fwrite(enc_pkt->data, enc_pkt->size, 1, fout);
        av_packet_unref(enc_pkt);
        if (!ret) {
            ret = AVERROR(errno);
            break;
        }
    }

end:
    av_packet_free(&enc_pkt);
    return ret;
}

/*
 * 课程注释：主流程。
 *     建设备 -> 找编码器 h264_vaapi -> 配参数 -> 建帧上下文 -> 开编码器
 *     循环：读一帧到内存 -> 从池里拿一个显存表面 -> 上传 -> 编码
 *     冲刷编码器。
 */
int main(int argc, char *argv[])
{
    int size, err;
    uint8_t *in_buf = NULL;
    uint8_t *in_data[4];
    int in_linesize[4];
    FILE *fin = NULL, *fout = NULL;
    AVFrame *sw_frame = NULL, *hw_frame = NULL;
    AVCodecContext *avctx = NULL;
    const AVCodec *codec = NULL;
    const char *enc_name = "h264_vaapi";

    if (argc < 5) {
        fprintf(stderr, "Usage: %s <width> <height> <input file> <output file>\n", argv[0]);
        /*
         * 课程注释（patch 0100）：出错时的退出码。原版用法错误、打不开文件是 return -1，退出码 255；设备创建失败是 return err，
         * AVERROR_EXTERNAL 的低 8 位，退出码 187；其他出错路径各返回各的错误码。从 main 返回的整数只有低 8 位有意义（L17 讲过），
         * 调用方分不清这些数字各是什么意思，所以统一成 1，原因写在 stderr。
         */
        return 1;
    }

    width  = atoi(argv[1]);
    height = atoi(argv[2]);
    /*
     * 课程注释（patch 0099）：一帧“紧密排列”的 NV12 有多少字节：宽 x 高（Y）加宽 x 高 / 2（UV 交错的一个平面）= 宽 x 高 x 1.5。
     * 最后一个参数 1 是对齐：每一行没有填充。原版写的是 width * height，然后分两次读 size 和 size / 2，
     * 这和下面“怎么往 AVFrame 里读”是同一件事的两半。
     */
    size   = av_image_get_buffer_size(AV_PIX_FMT_NV12, width, height, 1);
    if (size <= 0) {
        fprintf(stderr, "Invalid frame size %dx%d\n", width, height);
        return 1;
    }

    if (!(fin = fopen(argv[3], "rb"))) {
        fprintf(stderr, "Fail to open input file : %s\n", strerror(errno));
        return 1;
    }
    if (!(fout = fopen(argv[4], "w+b"))) {
        fprintf(stderr, "Fail to open output file : %s\n", strerror(errno));
        err = -1;
        goto close;
    }

    /*
     * 课程注释：创建 VAAPI 设备。第 3 个参数（设备名）是 NULL，用默认设备（VAAPI 是 /dev/dri/renderD128 起往后试）；
     * 有两块显卡时要自己传设备名（L20 第 2 步）。没有显卡时在这里失败：AVERROR_EXTERNAL（Generic error in an external library）。
     */
    err = av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                 NULL, NULL, 0);
    if (err < 0) {
        fprintf(stderr, "Failed to create a VAAPI device. Error code: %s\n", av_err2str(err));
        goto close;
    }

    if (!(codec = avcodec_find_encoder_by_name(enc_name))) {
        fprintf(stderr, "Could not find encoder.\n");
        err = -1;
        goto close;
    }

    if (!(avctx = avcodec_alloc_context3(codec))) {
        err = AVERROR(ENOMEM);
        goto close;
    }

    avctx->width     = width;
    avctx->height    = height;
    /*
     * 课程注释：编码器的参数。time_base 是帧的时间单位（这里 1/25 秒），framerate 是帧率。
     * 下面的 pix_fmt 不是 nv12，而是 AV_PIX_FMT_VAAPI：硬件编码器收的是“显存里的帧”，所以编码器的输入格式是硬件格式，
     * 内存里的帧要先上传（下面的 av_hwframe_transfer_data）才能送进来。
     */
    avctx->time_base = (AVRational){1, 25};
    avctx->framerate = (AVRational){25, 1};
    avctx->sample_aspect_ratio = (AVRational){1, 1};
    avctx->pix_fmt   = AV_PIX_FMT_VAAPI;

    /*
     * 课程注释：帧上下文必须在 avcodec_open2() 之前交给编码器：编码器打开时要从里面读出宽、高、表面格式。
     */
    /* set hw_frames_ctx for encoder's AVCodecContext */
    if ((err = set_hwframe_ctx(avctx, hw_device_ctx)) < 0) {
        fprintf(stderr, "Failed to set hwframe context.\n");
        goto close;
    }

    if ((err = avcodec_open2(avctx, codec, NULL)) < 0) {
        fprintf(stderr, "Cannot open video encoder codec. Error code: %s\n", av_err2str(err));
        goto close;
    }

    /*
     * 课程注释（patch 0099）：输入文件里的 NV12 是紧密排列的：Y 平面每行正好 width 字节，后面紧跟 UV 平面。
     * 而 av_frame_get_buffer(frame, 0) 分配出来的帧，每一行末尾可能有填充（linesize 大于 width，为了对齐，常见是 32 或 64 的倍数）。
     * 所以不能把文件里的数据直接读进 data[0]：先读进这块紧密排列的缓冲区，再用 av_image_copy2() 一行一行拷到帧里。
     * nv12_linesize 程序演示了不这样做的后果（README 第 2 步）。
     */
    /* the input file holds packed NV12 frames: lines are exactly "width" bytes long */
    if (!(in_buf = av_malloc(size))) {
        err = AVERROR(ENOMEM);
        goto close;
    }
    av_image_fill_arrays(in_data, in_linesize, in_buf, AV_PIX_FMT_NV12, width, height, 1);

    while (1) {
        if (!(sw_frame = av_frame_alloc())) {
            err = AVERROR(ENOMEM);
            goto close;
        }
        /* read data into software frame, and transfer them into hw frame */
        sw_frame->width  = width;
        sw_frame->height = height;
        sw_frame->format = AV_PIX_FMT_NV12;
        if ((err = av_frame_get_buffer(sw_frame, 0)) < 0)
            goto close;
        /* the lines of an AVFrame are padded to a multiple of the alignment
         * chosen by av_frame_get_buffer(), so read the whole frame and copy it
         * line by line instead of reading straight into the planes */
        if (fread(in_buf, size, 1, fin) != 1) {
            err = ferror(fin) ? AVERROR(EIO) : 0;
            break;
        }
        av_image_copy2(sw_frame->data, sw_frame->linesize, in_data, in_linesize,
                       AV_PIX_FMT_NV12, width, height);

        if (!(hw_frame = av_frame_alloc())) {
            err = AVERROR(ENOMEM);
            goto close;
        }
        /*
         * 课程注释：从帧上下文的池子里拿一个空的显存表面，放进 hw_frame（hw_frame->format 是 vaapi，data 里是表面的句柄）。
         * 池子里的表面用完了、又没有归还（帧没被释放），这里就会失败：用完的帧要 av_frame_free()（下面循环末尾）。
         */
        if ((err = av_hwframe_get_buffer(avctx->hw_frames_ctx, hw_frame, 0)) < 0) {
            fprintf(stderr, "Error code: %s.\n", av_err2str(err));
            goto close;
        }
        if (!hw_frame->hw_frames_ctx) {
            err = AVERROR(ENOMEM);
            goto close;
        }
        /*
         * 课程注释：上传：把内存里的 sw_frame 拷到显存里的 hw_frame。方向和 L20 相反：第 1 个参数是目的帧，第 2 个是来源帧；
         * 目的帧是硬件帧时就是“上传”，来源帧的格式必须是帧上下文的 sw_format（这里 nv12）。
         * 每一帧都要经总线拷一次：640x360 的一帧是 345,600 字节，1920x1080 的一帧是 3,110,400 字节（L20 讲过）。
         * 示例没有给帧设 pts：输出是裸 H.264 码流，用不上时间戳；要装进 MP4 就得设（L13、L15）。
         */
        if ((err = av_hwframe_transfer_data(hw_frame, sw_frame, 0)) < 0) {
            fprintf(stderr, "Error while transferring frame data to surface."
                    "Error code: %s.\n", av_err2str(err));
            goto close;
        }

        err = encode_write(avctx, hw_frame, fout);
        if (err != AVERROR(EAGAIN) && err < 0) {
            fprintf(stderr, "Failed to encode.\n");
            goto close;
        }
        av_frame_free(&hw_frame);
        av_frame_free(&sw_frame);
    }

    /*
     * 课程注释（patch 0100）：输入读完（或出错）才到这里。读输入出错时不冲刷，直接报错退出。
     */
    /* flush encoder (unless reading the input failed) */
    if (err >= 0) {
        err = encode_write(avctx, NULL, fout);
        if (err == AVERROR_EOF)
            err = 0;
    }

close:
    if (fin)
        fclose(fin);
    if (fout)
        fclose(fout);
    av_freep(&in_buf);
    av_frame_free(&sw_frame);
    av_frame_free(&hw_frame);
    avcodec_free_context(&avctx);
    av_buffer_unref(&hw_device_ctx);

    return err < 0 ? 1 : 0;
}
