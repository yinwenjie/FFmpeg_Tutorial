/*
 * L07 解封装加解码：打印容器为每一路流记录的编码参数（AVCodecParameters）。
 *
 * 用法：codec_params <输入文件>
 *
 * 解码器开始工作之前需要知道的东西都在这里：编码格式、宽高或采样率，以及 extradata。
 * 程序还会打印每一路流第一个包开头的几个字节，用来对比同一种编码在不同容器里的存放方式。
 * 这个程序只读容器，不解码。
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

#define DUMP_BYTES 16

/* 按十六进制打印开头的若干字节。 */
static void dump_bytes(const uint8_t *data, int size)
{
    for (int i = 0; i < size && i < DUMP_BYTES; i++)
        printf(" %02x", data[i]);
    if (size > DUMP_BYTES)
        printf(" ...");
    printf("\n");
}

/* 打印一路流的编码参数。 */
static void print_params(const AVStream *st)
{
    const AVCodecParameters *par = st->codecpar;
    /* 这种编码格式的默认解码器。NULL 表示当前的 FFmpeg 不能解码它。 */
    const AVCodec *codec = avcodec_find_decoder(par->codec_id);

    printf("流 #%d：%s\n", st->index, av_get_media_type_string(par->codec_type));
    printf("  codec_id      %s，默认解码器 %s\n", avcodec_get_name(par->codec_id),
           codec ? codec->name : "（没有）");
    printf("  profile/level %s / %d\n",
           avcodec_profile_name(par->codec_id, par->profile) ?
           avcodec_profile_name(par->codec_id, par->profile) : "未知", par->level);
    printf("  bit_rate      %" PRId64 " b/s\n", par->bit_rate);

    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        /* 视频：format 是 enum AVPixelFormat。 */
        printf("  宽高          %dx%d\n", par->width, par->height);
        printf("  format        %s\n", par->format < 0 ? "未知" : av_get_pix_fmt_name(par->format));
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        /* 音频：format 是 enum AVSampleFormat。 */
        char layout[64];

        av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
        printf("  采样率        %d Hz\n", par->sample_rate);
        printf("  声道布局      %s\n", layout);
        printf("  format        %s\n", par->format < 0 ? "未知" : av_get_sample_fmt_name(par->format));
        printf("  frame_size    %d\n", par->frame_size);
    }

    /*
     * extradata：解码器初始化时需要、但不放在每个包里的数据。内容由编码格式规定。
     * 没有 extradata 时指针是 NULL，大小是 0。
     */
    printf("  extradata     %d 字节", par->extradata_size);
    if (par->extradata_size)
        printf("：");
    dump_bytes(par->extradata, par->extradata_size);
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt = NULL;
    AVPacket *pkt = NULL;
    int *seen = NULL;
    unsigned nb_seen = 0;
    int ret;

    if (argc != 2) {
        fprintf(stderr, "用法：%s <输入文件>\n", argv[0]);
        return 1;
    }

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

    printf("容器格式：%s\n\n", fmt->iformat->name);
    for (unsigned i = 0; i < fmt->nb_streams; i++)
        print_params(fmt->streams[i]);

    /* 读包，打印每一路流的第一个包开头的字节。每路流都见过一次就停。 */
    pkt  = av_packet_alloc();
    seen = calloc(fmt->nb_streams, sizeof(*seen));
    if (!pkt || !seen) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    printf("\n每一路流的第一个包：\n");
    while (nb_seen < fmt->nb_streams && (ret = av_read_frame(fmt, pkt)) >= 0) {
        if (!seen[pkt->stream_index]) {
            seen[pkt->stream_index] = 1;
            nb_seen++;
            printf("  流 #%d  %6d 字节：", pkt->stream_index, pkt->size);
            dump_bytes(pkt->data, pkt->size);
        }
        av_packet_unref(pkt);
    }
    /* 有的流可能一个包也没有，读到文件末尾属于正常情况。 */
    if (ret == AVERROR_EOF)
        ret = 0;
    if (ret < 0)
        fprintf(stderr, "读 %s 出错：%s\n", argv[1], av_err2str(ret));

end:
    free(seen);
    av_packet_free(&pkt);
    avformat_close_input(&fmt);
    return ret < 0 ? 1 : 0;
}
