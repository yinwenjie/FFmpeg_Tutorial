/*
 * L11 滤镜图基础：查看 libavfilter 里有哪些滤镜，以及一个滤镜的输入、输出和选项。
 *
 * 用法：filter_info [滤镜名]
 *
 * 例子：
 *   filter_info              统计这个 FFmpeg 里的滤镜
 *   filter_info volume       看 volume 滤镜的输入、输出和选项
 *   filter_info abuffer      看入口滤镜（没有输入）
 *   filter_info amix         看一个输入个数不固定的滤镜
 *
 * 这个程序不处理任何数据，只读滤镜的“定义”（AVFilter）。
 * 命令行里对应的是 ffmpeg -filters 和 ffmpeg -h filter=volume。
 */

#include <stdio.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>

/* 打印一组 pad（输入口或输出口）。 */
static void print_pads(const AVFilter *filter, int is_output)
{
    const AVFilterPad *pads = is_output ? filter->outputs : filter->inputs;
    const unsigned count = avfilter_filter_pad_count(filter, is_output);
    const int dynamic = filter->flags &
                        (is_output ? AVFILTER_FLAG_DYNAMIC_OUTPUTS : AVFILTER_FLAG_DYNAMIC_INPUTS);

    printf("%s：%u 个%s\n", is_output ? "输出" : "输入", count,
           dynamic ? "（个数不固定，由参数决定，初始化时才建立）" : "");
    for (unsigned i = 0; i < count; i++)
        printf("  #%u  %-10s %s\n", i, avfilter_pad_get_name(pads, i),
               av_get_media_type_string(avfilter_pad_get_type(pads, i)));
}

/* 选项的类型，只列出滤镜里常见的几种。 */
static const char *type_name(enum AVOptionType type)
{
    switch (type) {
    case AV_OPT_TYPE_FLAGS:      return "标志";
    case AV_OPT_TYPE_INT:
    case AV_OPT_TYPE_INT64:
    case AV_OPT_TYPE_UINT64:     return "整数";
    case AV_OPT_TYPE_DOUBLE:
    case AV_OPT_TYPE_FLOAT:      return "小数";
    case AV_OPT_TYPE_STRING:     return "字符串";
    case AV_OPT_TYPE_RATIONAL:   return "分数";
    case AV_OPT_TYPE_BOOL:       return "开关";
    case AV_OPT_TYPE_DURATION:   return "时长";
    case AV_OPT_TYPE_SAMPLE_FMT: return "采样格式";
    case AV_OPT_TYPE_PIXEL_FMT:  return "像素格式";
    case AV_OPT_TYPE_CHLAYOUT:   return "声道布局";
    default:                     return "其他";
    }
}

/*
 * 打印滤镜的选项。选项挂在滤镜的 priv_class 上，用 av_opt_next() 逐个取。
 * AV_OPT_TYPE_CONST 不是选项，是某个选项可以取的命名值（例如 volume 的 precision 可以写 fixed、float、double），
 * 缩进打印在它所属的选项下面。
 */
static void print_options(const AVFilter *filter)
{
    const AVOption *opt = NULL;
    int count = 0;

    if (!filter->priv_class) {
        printf("选项：无\n");
        return;
    }
    printf("选项：\n");
    while ((opt = av_opt_next(&filter->priv_class, opt))) {
        if (opt->type == AV_OPT_TYPE_CONST) {
            printf("      可取 %-12s %s\n", opt->name, opt->help ? opt->help : "");
            continue;
        }
        printf("  %-14s %-8s %s\n", opt->name, type_name(opt->type), opt->help ? opt->help : "");
        count++;
    }
    if (!count)
        printf("  无\n");
}

/* 统计全部滤镜。av_filter_iterate() 逐个返回滤镜的定义，用法和 L02 见过的 av_codec_iterate() 一样。 */
static void print_summary(void)
{
    const AVFilter *filter;
    void *iter = NULL;
    int total = 0, sources = 0, sinks = 0, audio = 0, video = 0;

    while ((filter = av_filter_iterate(&iter))) {
        const unsigned nb_in  = avfilter_filter_pad_count(filter, 0);
        const unsigned nb_out = avfilter_filter_pad_count(filter, 1);
        const int dyn_in  = filter->flags & AVFILTER_FLAG_DYNAMIC_INPUTS;
        const int dyn_out = filter->flags & AVFILTER_FLAG_DYNAMIC_OUTPUTS;

        total++;
        if (!nb_in && !dyn_in)
            sources++;                  /* 没有输入：只产生数据，例如 abuffer、sine、testsrc */
        if (!nb_out && !dyn_out)
            sinks++;                    /* 没有输出：只接收数据，例如 abuffersink、nullsink */
        /* 按第一个输出的类型粗略分成音频滤镜和视频滤镜。 */
        if (nb_out) {
            const enum AVMediaType type = avfilter_pad_get_type(filter->outputs, 0);

            if (type == AVMEDIA_TYPE_AUDIO)
                audio++;
            else if (type == AVMEDIA_TYPE_VIDEO)
                video++;
        }
    }
    printf("这个 FFmpeg 里有 %d 个滤镜\n", total);
    printf("  第一个输出是音频的 %d 个，是视频的 %d 个\n", audio, video);
    printf("  没有输入的（源）%d 个，没有输出的（汇）%d 个\n", sources, sinks);
}

int main(int argc, char **argv)
{
    const AVFilter *filter;

    if (argc > 2) {
        fprintf(stderr, "用法：%s [滤镜名]\n", argv[0]);
        return 1;
    }
    if (argc == 1) {
        print_summary();
        return 0;
    }

    /* 按名字找滤镜的定义。找不到通常是名字写错了，或者编译 FFmpeg 时没有带上它。 */
    filter = avfilter_get_by_name(argv[1]);
    if (!filter) {
        fprintf(stderr, "没有名叫 %s 的滤镜\n", argv[1]);
        return 1;
    }

    printf("滤镜 %s：%s\n", filter->name, filter->description ? filter->description : "");
    print_pads(filter, 0);
    print_pads(filter, 1);
    print_options(filter);
    return 0;
}
