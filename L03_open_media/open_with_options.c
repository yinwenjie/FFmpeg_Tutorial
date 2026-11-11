/*
 * L03 实验：指定输入格式，并通过选项字典给解封装器传参数。
 *
 * 用法：open_with_options <输入文件> <格式名，传 - 表示自动识别> [选项=值 ...]
 *
 * 例：L01 导出的原始数据没有文件头，必须告诉 FFmpeg 格式和参数才能打开：
 *   open_with_options out/demo.yuv rawvideo video_size=1920x1080 pixel_format=yuv420p framerate=30
 *   open_with_options out/demo.pcm s16le sample_rate=48000 ch_layout=stereo
 *
 * AVDictionary 在这里是“选项”，在 show_metadata 里是“元数据”，是同一种数据结构的两种用途。
 */

#include <stdio.h>
#include <string.h>

#include <libavformat/avformat.h>
#include <libavutil/dict.h>

int main(int argc, char **argv)
{
    AVFormatContext *fmt_ctx = NULL;
    const AVInputFormat *iformat = NULL;
    AVDictionary *options = NULL;
    const AVDictionaryEntry *e = NULL;
    int ret;

    if (argc < 3) {
        fprintf(stderr, "用法：%s <输入文件> <格式名，传 - 表示自动识别> [选项=值 ...]\n",
                argv[0]);
        return 1;
    }

    /* 按短名字查找解封装器。名字就是 ffmpeg -demuxers 列表里的那一列。 */
    if (strcmp(argv[2], "-") != 0) {
        iformat = av_find_input_format(argv[2]);
        if (!iformat) {
            fprintf(stderr, "没有名为 %s 的输入格式，可以用 ffmpeg -demuxers 查看全部名字\n",
                    argv[2]);
            return 1;
        }
    }

    /* 把命令行上的 “选项=值” 逐个放进字典。 */
    for (int i = 3; i < argc; i++) {
        ret = av_dict_parse_string(&options, argv[i], "=", " ", 0);
        if (ret < 0) {
            fprintf(stderr, "无法解析 %s：%s\n", argv[i], av_err2str(ret));
            av_dict_free(&options);
            return 1;
        }
    }

    /*
     * 第 3 个参数强制指定格式，第 4 个参数传入选项字典的地址。
     * 函数返回后，被认出并使用的选项会从字典里删掉，剩下的就是没有被识别的。
     */
    ret = avformat_open_input(&fmt_ctx, argv[1], iformat, &options);
    if (ret < 0) {
        fprintf(stderr, "打不开 %s：%s\n", argv[1], av_err2str(ret));
        av_dict_free(&options);
        return 1;
    }

    /*
     * 选项名拼错时 FFmpeg 不会报错，只是悄悄忽略。
     * 所以调用之后一定要检查字典里还剩下什么，这是工程代码里的固定动作。
     */
    while ((e = av_dict_iterate(options, e)))
        fprintf(stderr, "警告：选项 %s=%s 没有被识别，已被忽略\n", e->key, e->value);
    av_dict_free(&options);

    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "读不到流信息：%s\n", av_err2str(ret));
        avformat_close_input(&fmt_ctx);
        return 1;
    }

    av_dump_format(fmt_ctx, 0, argv[1], 0);

    avformat_close_input(&fmt_ctx);
    return 0;
}
