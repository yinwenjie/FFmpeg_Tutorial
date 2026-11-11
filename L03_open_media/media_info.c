/*
 * L03 打开媒体文件：打印容器和每一路流的参数。
 *
 * 用法：media_info <输入文件或 URL>
 *
 * 输出里每一行左边的名字都是结构体的成员名：
 *   “容器”部分来自 AVFormatContext，
 *   “流”部分来自 AVStream 和它的 codecpar（AVCodecParameters）。
 * 可以和下面这条命令的结果逐项核对：
 *   ffprobe -v error -show_format -show_streams <输入>
 */

#include <inttypes.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

/* 打印一个字典的全部内容。字典为 NULL 或为空时什么也不打印。 */
static void print_dict(const AVDictionary *dict)
{
    const AVDictionaryEntry *e = NULL;

    if (av_dict_count(dict) == 0)
        return;
    printf("  metadata\n");
    while ((e = av_dict_iterate(dict, e)))
        printf("      %s = %s\n", e->key, e->value);
}

/*
 * 打印一个时间值。ts 是整数，单位是 tb 秒，所以 秒数 = ts × tb。
 * FFmpeg 用 AV_NOPTS_VALUE 表示“不知道”，不能把它当成普通数字去换算。
 */
static void print_time(const char *name, int64_t ts, AVRational tb)
{
    if (ts == AV_NOPTS_VALUE)
        printf("  %-20s N/A\n", name);
    else
        printf("  %-20s %" PRId64 "  (%.3f 秒)\n", name, ts, ts * av_q2d(tb));
}

static void print_rational(const char *name, AVRational r)
{
    printf("  %-20s %d/%d\n", name, r.num, r.den);
}

/* disposition 是一组标志位，例如 default（默认轨）、attached_pic（封面图）。 */
static void print_disposition(int disposition)
{
    printf("  %-20s", "disposition");
    if (!disposition)
        printf(" 0");
    for (int bit = 0; bit < 31; bit++) {
        if (disposition & (1 << bit)) {
            const char *name = av_disposition_to_string(1 << bit);
            printf(" %s", name ? name : "?");
        }
    }
    printf("\n");
}

static void print_container(const AVFormatContext *fmt_ctx)
{
    /* AVFormatContext 里的时间以 AV_TIME_BASE（1/1000000 秒，即微秒）为单位。 */
    const AVRational us = { 1, AV_TIME_BASE };

    printf("== 容器 ==\n");
    printf("  %-20s %s\n", "url", fmt_ctx->url);
    printf("  %-20s %s\n", "iformat->name", fmt_ctx->iformat->name);
    printf("  %-20s %s\n", "iformat->long_name", fmt_ctx->iformat->long_name);
    printf("  %-20s %u\n", "nb_streams", fmt_ctx->nb_streams);
    printf("  %-20s %u\n", "nb_programs", fmt_ctx->nb_programs);
    printf("  %-20s %u\n", "nb_chapters", fmt_ctx->nb_chapters);
    print_time("start_time", fmt_ctx->start_time, us);
    print_time("duration", fmt_ctx->duration, us);
    printf("  %-20s %" PRId64 "\n", "bit_rate", fmt_ctx->bit_rate);
    /* 文件大小不在 AVFormatContext 里，要问 I/O 层。直播流等输入没有大小，返回负值。 */
    printf("  %-20s %" PRId64 "\n", "avio_size(pb)",
           fmt_ctx->pb ? avio_size(fmt_ctx->pb) : -1);
    print_dict(fmt_ctx->metadata);
}

static void print_stream(const AVStream *st)
{
    /* 与编码有关的参数集中放在 codecpar 里，后面打开解码器时也是把它交给解码器。 */
    const AVCodecParameters *par = st->codecpar;
    const char *profile = avcodec_profile_name(par->codec_id, par->profile);

    printf("\n== 流 #%d：%s ==\n", st->index,
           av_get_media_type_string(par->codec_type));

    printf("  %-20s 0x%x\n", "id", st->id);
    printf("  %-20s %s\n", "codec_id", avcodec_get_name(par->codec_id));
    printf("  %-20s %s\n", "profile", profile ? profile : "N/A");
    printf("  %-20s %s\n", "codec_tag", av_fourcc2str(par->codec_tag));
    printf("  %-20s %" PRId64 "\n", "bit_rate", par->bit_rate);
    printf("  %-20s %d\n", "extradata_size", par->extradata_size);

    switch (par->codec_type) {
    case AVMEDIA_TYPE_VIDEO: {
        /* par->format 对视频是 enum AVPixelFormat，对音频是 enum AVSampleFormat。 */
        const char *pix_fmt = av_get_pix_fmt_name(par->format);

        printf("  %-20s %d\n", "width", par->width);
        printf("  %-20s %d\n", "height", par->height);
        printf("  %-20s %s\n", "format", pix_fmt ? pix_fmt : "N/A");
        printf("  %-20s %d\n", "level", par->level);
        print_rational("sample_aspect_ratio", st->sample_aspect_ratio);
        print_rational("avg_frame_rate", st->avg_frame_rate);
        print_rational("r_frame_rate", st->r_frame_rate);
        break;
    }
    case AVMEDIA_TYPE_AUDIO: {
        const char *sample_fmt = av_get_sample_fmt_name(par->format);
        char layout[64];

        av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
        printf("  %-20s %d\n", "sample_rate", par->sample_rate);
        printf("  %-20s %s\n", "format", sample_fmt ? sample_fmt : "N/A");
        printf("  %-20s %s\n", "ch_layout", layout);
        printf("  %-20s %d\n", "nb_channels", par->ch_layout.nb_channels);
        printf("  %-20s %d\n", "frame_size", par->frame_size);
        break;
    }
    default:
        break;
    }

    /* 每一路流有自己的时间基，流上的时间值都以它为单位。L04 专门讲这个。 */
    print_rational("time_base", st->time_base);
    print_time("start_time", st->start_time, st->time_base);
    print_time("duration", st->duration, st->time_base);
    printf("  %-20s %" PRId64 "\n", "nb_frames", st->nb_frames);
    print_disposition(st->disposition);
    print_dict(st->metadata);
}

/* 节目（program）把若干路流编成一组，主要出现在 MPEG-TS 里：一个节目就是一个电视频道。 */
static void print_program(const AVProgram *prog)
{
    printf("\n== 节目 id=%d ==\n", prog->id);
    printf("  %-20s", "stream_index");
    for (unsigned i = 0; i < prog->nb_stream_indexes; i++)
        printf(" %u", prog->stream_index[i]);
    printf("\n");
    print_dict(prog->metadata);
}

int main(int argc, char **argv)
{
    AVFormatContext *fmt_ctx = NULL;
    const char *url;
    int ret;

    if (argc != 2) {
        fprintf(stderr, "用法：%s <输入文件或 URL>\n", argv[0]);
        return 1;
    }
    url = argv[1];

    /* FFmpeg 的函数失败时返回负的错误码，av_err2str() 把它转换成文字。 */
    ret = avformat_open_input(&fmt_ctx, url, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s（错误码 %d）\n", url, av_err2str(ret), ret);
        return 1;
    }

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不到流信息：%s\n", av_err2str(ret));
        goto end;
    }

    print_container(fmt_ctx);
    for (unsigned i = 0; i < fmt_ctx->nb_streams; i++)
        print_stream(fmt_ctx->streams[i]);
    for (unsigned i = 0; i < fmt_ctx->nb_programs; i++)
        print_program(fmt_ctx->programs[i]);

    /*
     * 文件里可能有多路视频、多路音频。av_find_best_stream() 替我们挑一路：
     * 先看 disposition（默认轨优先），再看探测阶段读到了多少帧，最后比较码率。
     * 文件里没有这种类型的流时返回负值。
     */
    printf("\n== av_find_best_stream ==\n");
    printf("  %-20s %d\n", "video",
           av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0));
    printf("  %-20s %d\n", "audio",
           av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0));

    /*
     * 上面这些信息，FFmpeg 自带一个函数可以一次打印出来：
     * ffmpeg -i 和 ffprobe 开头的那段 “Input #0, ...” 就是它的输出。
     * 它通过 av_log 写到 stderr，所以先把 stdout 刷新，避免两路输出顺序错乱。
     */
    printf("\n== av_dump_format ==\n");
    fflush(stdout);
    av_dump_format(fmt_ctx, 0, url, 0);

    ret = 0;
end:
    /* 无论成功还是失败，只要 open 成功过，就必须 close。 */
    avformat_close_input(&fmt_ctx);
    return ret < 0 ? 1 : 0;
}
