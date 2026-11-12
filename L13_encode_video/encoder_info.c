/*
 * L13 视频编码：查看一个视频编码器接受哪些像素格式、有哪些自己的选项。
 *
 * 用法：encoder_info <编码器名>
 *
 * 例子：
 *   encoder_info libx264
 *   encoder_info mpeg2video
 *   encoder_info mjpeg
 *   encoder_info png
 *
 * 这个程序不编码任何数据，只读编码器的“定义”（AVCodec）。
 * 命令行里对应的是 ffmpeg -h encoder=libx264。
 */

#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>

/*
 * 打印编码器接受的像素格式。
 * avcodec_get_supported_config() 返回一个数组和它的长度；
 * 数组为 NULL 表示编码器没有声明限制（任何格式都可以试）。
 */
static int print_pixel_formats(const AVCodec *codec)
{
    const enum AVPixelFormat *formats = NULL;
    int count = 0;
    int ret = avcodec_get_supported_config(NULL, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0,
                                           (const void **)&formats, &count);

    if (ret < 0) {
        fprintf(stderr, "查询像素格式失败：%s\n", av_err2str(ret));
        return ret;
    }
    printf("接受的像素格式：");
    if (!formats) {
        printf("没有声明限制\n");
        return 0;
    }
    for (int i = 0; i < count; i++)
        printf("%s%s", i ? " " : "", av_get_pix_fmt_name(formats[i]));
    printf("\n");
    return 0;
}

/*
 * 打印编码器的私有选项，方法和 L11 的 filter_info 一样：选项挂在 priv_class 上，用 av_opt_next() 逐个取。
 * AV_OPT_TYPE_CONST 不是选项，是某个选项可以取的命名值，这里只数个数，不逐个打印。
 * 通用选项（b、g、bf 等）属于 AVCodecContext，所有编码器共用，不在这个列表里。
 */
static void print_private_options(const AVCodec *codec)
{
    const AVOption *opt = NULL;
    const char *last_name = "";
    int count = 0;

    if (!codec->priv_class) {
        printf("私有选项：无\n");
        return;
    }
    printf("私有选项：\n");
    while ((opt = av_opt_next(&codec->priv_class, opt))) {
        if (opt->type == AV_OPT_TYPE_CONST)
            continue;
        /* 同一个选项可能有别名，连续重名的只打印一次。 */
        if (!strcmp(opt->name, last_name))
            continue;
        if (!(opt->flags & AV_OPT_FLAG_ENCODING_PARAM))
            continue;
        printf("  %-18s %s\n", opt->name, opt->help ? opt->help : "");
        last_name = opt->name;
        count++;
    }
    printf("共 %d 个\n", count);
}

int main(int argc, char **argv)
{
    const AVCodec *codec;

    if (argc != 2) {
        fprintf(stderr, "用法：%s <编码器名>\n", argv[0]);
        return 1;
    }

    codec = avcodec_find_encoder_by_name(argv[1]);
    if (!codec) {
        fprintf(stderr, "没有名叫 %s 的编码器\n", argv[1]);
        return 1;
    }
    if (codec->type != AVMEDIA_TYPE_VIDEO) {
        fprintf(stderr, "%s 不是视频编码器\n", argv[1]);
        return 1;
    }

    printf("编码器 %s：%s\n", codec->name, codec->long_name ? codec->long_name : "");
    printf("编码格式：%s\n", avcodec_get_name(codec->id));
    /* AV_CODEC_CAP_DELAY：编码器内部会留帧，输入结束时必须冲刷才能拿全输出。 */
    printf("内部会留帧（结束时必须冲刷）：%s\n", (codec->capabilities & AV_CODEC_CAP_DELAY) ? "是" : "否");
    printf("硬件编码器：%s\n", (codec->capabilities & AV_CODEC_CAP_HARDWARE) ? "是" : "否");

    if (print_pixel_formats(codec) < 0)
        return 1;
    print_private_options(codec);
    return 0;
}
