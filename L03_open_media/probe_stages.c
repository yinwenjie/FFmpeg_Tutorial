/*
 * L03 实验：avformat_open_input() 和 avformat_find_stream_info() 各自做了什么。
 *
 * 用法：probe_stages <输入文件>
 *
 * 程序在两次调用之后各打印一次流参数，并打印到那一刻为止从文件里读了多少字节。
 * 拿 MP4、MKV、TS 和裸流分别试一遍，就能看出：
 *   - 有完整文件头的格式，open 之后参数基本齐了；
 *   - 没有文件头的格式，参数要靠 find_stream_info 读取并分析一段数据才能得到。
 */

#include <inttypes.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

static void print_stage(const AVFormatContext *fmt_ctx, const char *stage)
{
    printf("-- %s --\n", stage);
    /* bytes_read 是 I/O 层的只读统计量：到目前为止一共读了多少字节。 */
    printf("  已读取 %" PRId64 " 字节，nb_streams = %u，duration = ",
           fmt_ctx->pb ? fmt_ctx->pb->bytes_read : 0, fmt_ctx->nb_streams);
    if (fmt_ctx->duration == AV_NOPTS_VALUE)
        printf("N/A\n");
    else
        printf("%.3f 秒\n", fmt_ctx->duration / (double)AV_TIME_BASE);

    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++) {
        const AVStream *st = fmt_ctx->streams[i];
        const AVCodecParameters *par = st->codecpar;

        printf("  #%u %-5s %-10s ", i, av_get_media_type_string(par->codec_type),
               avcodec_get_name(par->codec_id));

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            const char *pix_fmt = av_get_pix_fmt_name(par->format);

            printf("%dx%d  format=%s  avg_frame_rate=%d/%d",
                   par->width, par->height, pix_fmt ? pix_fmt : "未知",
                   st->avg_frame_rate.num, st->avg_frame_rate.den);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            const char *sample_fmt = av_get_sample_fmt_name(par->format);
            char layout[64];

            av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
            printf("%d Hz  format=%s  ch_layout=%s",
                   par->sample_rate, sample_fmt ? sample_fmt : "未知", layout);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt_ctx = NULL;
    int ret;

    if (argc != 2) {
        fprintf(stderr, "用法：%s <输入文件>\n", argv[0]);
        return 1;
    }

    ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        return 1;
    }
    printf("识别出的容器格式：%s\n", fmt_ctx->iformat->name);
    print_stage(fmt_ctx, "avformat_open_input 之后");

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不到流信息：%s\n", av_err2str(ret));
        avformat_close_input(&fmt_ctx);
        return 1;
    }
    print_stage(fmt_ctx, "avformat_find_stream_info 之后");

    avformat_close_input(&fmt_ctx);
    return 0;
}
