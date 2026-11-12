/*
 * L13 视频编码：把一个 Y4M 文件（未压缩的 yuv420p 视频）编码成压缩码流。
 *
 * 用法：encode_yuv <输入.y4m> <输出文件> <编码器名> [选项=值 ...]
 *
 * 例子：
 *   encode_yuv out/src.y4m out/a.264 libx264
 *   encode_yuv out/src.y4m out/b.264 libx264 b=500k
 *   encode_yuv out/src.y4m out/c.264 libx264 crf=28 preset=fast g=50 bf=0
 *   encode_yuv out/src.y4m out/d.m2v mpeg2video b=2M g=12 bf=2
 *   encode_yuv out/src.y4m out/e.264 libx264 colorspace=bt709 color_range=tv
 *
 * “选项=值”的名字和 ffmpeg 命令行相同（去掉前面的减号）：b 是码率，g 是关键帧间隔，
 * bf 是连续 B 帧的最大个数，preset、crf 是 libx264 自己的选项。
 * 所有选项放进一个字典（AVDictionary），在 avcodec_open2() 时一起交给编码器。
 *
 * 输出是裸码流（没有容器）：H.264 是 Annex B 格式，ffplay 和 ffprobe 可以直接打开。
 * 把码流装进 MP4 之类的容器是 L15 的内容。
 *
 * 和官方示例 encode_video.c 的区别：
 *   1. 输入是真实的画面，尺寸和帧率从 Y4M 文件头里读；
 *   2. 编码参数不写死在代码里，用选项字典传，写错的选项会报出来；
 *   3. 每个包打印 pts、dts、大小和帧类型，结束时统计平均码率；
 *   4. 检查写文件的结果，出错时释放资源并返回非 0 的退出码。
 *
 * 默认用单线程编码，这样每台机器上的结果相同；加 threads=0 用全部的核。
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>

/* 统计用的计数器。 */
typedef struct Stats {
    int     nb_frames;      /* 送进编码器的帧数 */
    int     nb_packets;     /* 写出的包数 */
    int     nb_key;         /* 其中关键帧的个数 */
    int     nb_flushed;     /* 冲刷编码器时才拿到的包数 */
    int64_t bytes;          /* 写出的总字节数 */
} Stats;

/*
 * 读 Y4M 的文件头。文件头是一行文本，例如：
 *   YUV4MPEG2 W640 H360 F25:1 Ip A1:1 C420mpeg2
 * 用空格分开，每一项的第一个字母说明它是什么：W 宽，H 高，F 帧率，C 色度采样方式。
 * 本程序只接受 8 位的 4:2:0（没有 C 项时默认就是它）。
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
            /* C420、C420jpeg、C420mpeg2、C420paldv 都是 8 位 4:2:0，区别只在色度采样的位置。
             * C420p10 之类是高位深，C422、C444 是别的采样方式，这里不支持。 */
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

/*
 * 读一帧。每一帧先是一行以 FRAME 开头的文本，然后是 Y、U、V 三个平面，行与行之间没有填充。
 * AVFrame 里每行的长度是 linesize，可能比宽度大（L05 讲过），所以要一行一行地读。
 * 返回 1：读到一帧；0：文件结束；负数：出错。
 */
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
        /* 4:2:0 的色度平面宽和高都是亮度的一半（奇数时向上取整）。 */
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
 * 帧类型。编码器在包上附带一份“质量统计”的附加数据（side data），其中第 5 个字节是帧类型。
 * 不是每个编码器都提供，没有时返回 '?'。
 */
static char packet_picture_type(const AVPacket *pkt)
{
    size_t size;
    const uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_STATS, &size);

    if (!sd || size < 5)
        return '?';
    return av_get_picture_type_char(sd[4]);
}

/*
 * 送一帧给编码器，再把编码器已经编好的包全部取出来写进文件。
 * frame 为 NULL 表示输入结束，编码器会把留在内部的帧全部编完交出来（冲刷）。
 *
 * 结构和 L05 的解码正好相反：解码是“送包、取帧”，编码是“送帧、取包”，
 * 返回值的含义相同：AVERROR(EAGAIN) 是“要更多输入才有输出”，AVERROR_EOF 是“全部取完了”。
 */
static int encode(AVCodecContext *enc, const AVFrame *frame, AVPacket *pkt, FILE *out, Stats *st)
{
    int ret = avcodec_send_frame(enc, frame);

    if (ret < 0) {
        fprintf(stderr, "向编码器送帧失败：%s\n", av_err2str(ret));
        return ret;
    }
    if (frame)
        st->nb_frames++;

    while (1) {
        ret = avcodec_receive_packet(enc, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            return 0;
        if (ret < 0) {
            fprintf(stderr, "编码失败：%s\n", av_err2str(ret));
            return ret;
        }

        printf("包 %3d  pts %4" PRId64 "  dts %4" PRId64 "  %6d 字节  %c%s\n",
               st->nb_packets, pkt->pts, pkt->dts, pkt->size, packet_picture_type(pkt),
               (pkt->flags & AV_PKT_FLAG_KEY) ? "  关键帧" : "");

        if (fwrite(pkt->data, 1, pkt->size, out) != (size_t)pkt->size) {
            fprintf(stderr, "写输出文件失败：%s\n", strerror(errno));
            av_packet_unref(pkt);
            return AVERROR(EIO);
        }

        st->nb_packets++;
        st->bytes += pkt->size;
        if (pkt->flags & AV_PKT_FLAG_KEY)
            st->nb_key++;
        if (!frame)
            st->nb_flushed++;
        av_packet_unref(pkt);
    }
}

int main(int argc, char **argv)
{
    const char *in_name, *out_name, *codec_name;
    const AVCodec *codec;
    const AVDictionaryEntry *entry = NULL;
    AVCodecContext *enc = NULL;
    AVDictionary *opts = NULL;
    AVFrame *frame = NULL;
    AVPacket *pkt = NULL;
    AVRational frame_rate;
    FILE *in = NULL, *out = NULL;
    Stats st = { 0 };
    int width, height;
    int ret;

    if (argc < 4) {
        fprintf(stderr, "用法：%s <输入.y4m> <输出文件> <编码器名> [选项=值 ...]\n", argv[0]);
        return 1;
    }
    in_name    = argv[1];
    out_name   = argv[2];
    codec_name = argv[3];

    /* 命令行上的“选项=值”放进字典。字典只是字符串到字符串的表，这一步不检查选项对不对。 */
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
    ret = read_y4m_header(in, &width, &height, &frame_rate);
    if (ret < 0)
        goto end;

    /* 按名字找编码器。同一种格式可以有多个编码器（H.264 有 libx264、h264_vaapi 等），所以按名字找最明确。 */
    codec = avcodec_find_encoder_by_name(codec_name);
    if (!codec) {
        fprintf(stderr, "没有名叫 %s 的编码器\n", codec_name);
        ret = AVERROR_ENCODER_NOT_FOUND;
        goto end;
    }
    if (codec->type != AVMEDIA_TYPE_VIDEO) {
        fprintf(stderr, "%s 不是视频编码器\n", codec_name);
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
     * 必须由程序告诉编码器的参数：画面尺寸、像素格式、时间基。
     * 解码时这些是从码流里读出来的；编码时没有地方可读，只能我们给。
     *
     * 时间基取帧率的倒数，这样第 n 帧的 pts 就是 n。
     * framerate 只是告诉编码器“大约每秒多少帧”，供码率控制使用。
     */
    enc->width     = width;
    enc->height    = height;
    enc->pix_fmt   = AV_PIX_FMT_YUV420P;
    enc->time_base = av_inv_q(frame_rate);
    enc->framerate = frame_rate;

    /*
     * 线程数。很多编码器用多线程时，结果的大小、延迟的帧数会随线程数略有不同，
     * 而自动选的线程数取决于机器有几个核。为了让每台机器上跑出和 README 相同的数字，这里默认用单线程。
     * 想用全部的核，在命令行加 threads=0（0 表示让编码器自己决定），它会覆盖这里的值。
     */
    enc->thread_count = 1;

    /*
     * 打开编码器，同时把选项字典交给它。
     * 字典里的每一项先在 AVCodecContext 的通用选项里找（b、g、bf、colorspace……），
     * 再在这个编码器自己的私有选项里找（libx264 的 preset、crf……）。
     * 被认出来的项会从字典里删掉；函数返回之后字典里剩下的，就是没有被认出来的。
     */
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

    printf("编码器 %s：%dx%d，%s，帧率 %d/%d，时间基 %d/%d\n",
           codec->name, enc->width, enc->height, av_get_pix_fmt_name(enc->pix_fmt),
           enc->framerate.num, enc->framerate.den, enc->time_base.num, enc->time_base.den);
    /*
     * 打开之后，上下文里是实际生效的通用参数。
     * 码率为 0、关键帧间隔为负数表示我们没有指定，由编码器按自己的默认方式决定（libx264 就是这样）。
     * has_b_frames 是编码器算出来的“解码时要为 B 帧重排留几帧”，就是 L08 里见过的那个值。
     */
    if (enc->bit_rate > 0)
        printf("目标码率 %" PRId64 " b/s，", enc->bit_rate);
    else
        printf("目标码率未指定，");
    if (enc->gop_size >= 0)
        printf("关键帧间隔 %d，", enc->gop_size);
    else
        printf("关键帧间隔未指定，");
    printf("重排延迟 %d 帧\n", enc->has_b_frames);
    printf("色彩矩阵 %s，色彩范围 %s\n",
           av_color_space_name(enc->colorspace), av_color_range_name(enc->color_range));

    out = fopen(out_name, "wb");
    if (!out) {
        fprintf(stderr, "打不开输出文件 %s：%s\n", out_name, strerror(errno));
        ret = AVERROR(errno);
        goto end;
    }

    /* 准备一帧的缓冲区。格式、尺寸和编码器的一致。 */
    frame->format = enc->pix_fmt;
    frame->width  = enc->width;
    frame->height = enc->height;
    ret = av_frame_get_buffer(frame, 0);
    if (ret < 0)
        goto end;

    while (1) {
        /*
         * 编码器可能还留着上一帧的引用（它要拿前面的帧做参考）。
         * 这时不能直接往缓冲区里写新画面，av_frame_make_writable() 会在需要时换一块新的缓冲区。
         */
        ret = av_frame_make_writable(frame);
        if (ret < 0)
            goto end;

        ret = read_y4m_frame(in, frame);
        if (ret < 0)
            goto end;
        if (ret == 0)
            break;

        /* 时间戳以编码器的时间基为单位。时间基是 1/帧率，所以就是帧的序号。 */
        frame->pts = st.nb_frames;

        ret = encode(enc, frame, pkt, out, &st);
        if (ret < 0)
            goto end;
    }

    /* 输入结束：冲刷编码器。 */
    ret = encode(enc, NULL, pkt, out, &st);
    if (ret < 0)
        goto end;

    /* fclose() 也可能失败（最后一块数据这时才真正写出去），所以要检查。 */
    ret = fclose(out);
    out = NULL;
    if (ret) {
        fprintf(stderr, "写输出文件失败：%s\n", strerror(errno));
        ret = AVERROR(EIO);
        goto end;
    }

    printf("送进 %d 帧，写出 %d 个包，其中关键帧 %d 个，冲刷时写出 %d 个\n",
           st.nb_frames, st.nb_packets, st.nb_key, st.nb_flushed);
    if (st.nb_frames) {
        const double seconds = st.nb_frames * av_q2d(enc->time_base);

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
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc);
    av_dict_free(&opts);
    return ret < 0 ? 1 : 0;
}
