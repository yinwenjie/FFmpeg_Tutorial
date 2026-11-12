/*
 * L15 比特流滤镜：把 MP4 里的 H.264 视频取出来，写成可以直接播放的裸码流（.h264）。
 *
 * 用法：extract_h264 <输入文件> <输出.h264>
 *
 * 为什么不能直接把包写进文件？H.264 的码流有两种封装方式：
 *   Annex B：每个 NAL 单元前面有起始码 00 00 00 01，SPS、PPS 在码流里（关键帧前面）。裸码流（.h264）、
 *            MPEG-TS 用这种。L13 里 libx264 写出的就是这种。
 *   AVCC：   每个 NAL 单元前面是它的长度（4 字节），SPS、PPS 不在码流里，放在文件头的 extradata 里。
 *            MP4、MKV 用这种。
 * 从 MP4 里读出来的包是 AVCC 格式的，直接连起来写进文件，播放器认不出来。
 * 要先经过一个“比特流滤镜”（bitstream filter）h264_mp4toannexb：它只改写包的字节，不解码。
 * 它是 libavcodec 提供的，用法和 L11 的滤镜图有点像：送进去，取出来，最后冲刷。
 *   av_bsf_send_packet() 送一个包（送 NULL 表示结束），av_bsf_receive_packet() 取处理好的包。
 * 命令行里对应的是 ffmpeg -i in.mp4 -c:v copy -bsf:v h264_mp4toannexb -an out.h264。
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

/* 取出滤镜处理好的所有包，写进文件。 */
static int write_filtered(AVBSFContext *bsf, AVPacket *pkt, FILE *out, long *count)
{
    int ret;

    while ((ret = av_bsf_receive_packet(bsf, pkt)) >= 0) {
        if (fwrite(pkt->data, 1, pkt->size, out) != (size_t)pkt->size) {
            fprintf(stderr, "写输出文件失败\n");
            av_packet_unref(pkt);
            return AVERROR(EIO);
        }
        (*count)++;
        av_packet_unref(pkt);
    }
    /* EAGAIN：滤镜要更多的输入；EOF：滤镜已经结束。这两种都不是错误。 */
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    fprintf(stderr, "取滤镜输出失败：%s\n", av_err2str(ret));
    return ret;
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    const AVBitStreamFilter *filter;
    AVBSFContext *bsf = NULL;
    AVPacket *pkt = NULL;
    FILE *out = NULL;
    long nb_in = 0, nb_out = 0;
    int stream_index, ret;

    if (argc != 3) {
        fprintf(stderr, "用法：%s <输入文件> <输出.h264>\n", argv[0]);
        return 1;
    }

    ret = avformat_open_input(&fmt, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0)
        goto end;

    stream_index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (stream_index < 0) {
        fprintf(stderr, "文件里没有视频流\n");
        ret = stream_index;
        goto end;
    }
    if (fmt->streams[stream_index]->codecpar->codec_id != AV_CODEC_ID_H264) {
        fprintf(stderr, "视频不是 H.264，是 %s\n",
                avcodec_get_name(fmt->streams[stream_index]->codecpar->codec_id));
        ret = AVERROR(EINVAL);
        goto end;
    }

    /*
     * 创建滤镜并初始化。滤镜需要知道输入流的参数：extradata 里有 SPS 和 PPS，
     * 滤镜要把它们插到每个关键帧的前面。
     */
    filter = av_bsf_get_by_name("h264_mp4toannexb");
    if (!filter) {
        fprintf(stderr, "找不到比特流滤镜 h264_mp4toannexb\n");
        ret = AVERROR_BSF_NOT_FOUND;
        goto end;
    }
    ret = av_bsf_alloc(filter, &bsf);
    if (ret < 0)
        goto end;
    ret = avcodec_parameters_copy(bsf->par_in, fmt->streams[stream_index]->codecpar);
    if (ret < 0)
        goto end;
    bsf->time_base_in = fmt->streams[stream_index]->time_base;
    ret = av_bsf_init(bsf);
    if (ret < 0) {
        fprintf(stderr, "初始化滤镜失败：%s\n", av_err2str(ret));
        goto end;
    }

    pkt = av_packet_alloc();
    out = fopen(argv[2], "wb");
    if (!pkt || !out) {
        if (!out)
            fprintf(stderr, "打不开输出文件 %s\n", argv[2]);
        ret = pkt ? AVERROR(errno) : AVERROR(ENOMEM);
        goto end;
    }

    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index != stream_index) {
            av_packet_unref(pkt);
            continue;
        }
        nb_in++;
        ret = av_bsf_send_packet(bsf, pkt);     /* 送进去以后，包里的数据归滤镜管 */
        if (ret < 0) {
            fprintf(stderr, "送包失败：%s\n", av_err2str(ret));
            goto end;
        }
        ret = write_filtered(bsf, pkt, out, &nb_out);
        if (ret < 0)
            goto end;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读文件失败：%s\n", av_err2str(ret));
        goto end;
    }

    /* 文件读完了：送 NULL 告诉滤镜输入结束，取出它可能还留着的包。 */
    ret = av_bsf_send_packet(bsf, NULL);
    if (ret < 0)
        goto end;
    ret = write_filtered(bsf, pkt, out, &nb_out);
    if (ret < 0)
        goto end;

    ret = fclose(out);
    out = NULL;
    if (ret) {
        fprintf(stderr, "写输出文件失败\n");
        ret = AVERROR(EIO);
        goto end;
    }
    printf("读入 %ld 个包，写出 %ld 个包到 %s\n", nb_in, nb_out, argv[2]);
    ret = 0;

end:
    if (out)
        fclose(out);
    av_packet_free(&pkt);
    av_bsf_free(&bsf);
    avformat_close_input(&fmt);
    return ret < 0 ? 1 : 0;
}
