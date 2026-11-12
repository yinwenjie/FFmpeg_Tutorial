/*
 * L06 音频解码：采样格式和声道布局的几个工具函数。
 *
 * 用法：sample_formats
 *
 * 这个程序不读任何文件，只做计算和打印。四个小节分别回答四个问题：
 * FFmpeg 里有哪些采样格式，一段 PCM 有多少字节，一帧音频有多长，声道布局是什么。
 */

#include <inttypes.h>
#include <stdio.h>

#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>

/* 打印一种声道布局：名字、声道数，以及每个位置上是哪个声道。 */
static void print_layout(const AVChannelLayout *layout)
{
    char name[64];

    /* av_channel_layout_describe() 把布局写成字符串，例如 stereo、5.1。 */
    av_channel_layout_describe(layout, name, sizeof(name));
    printf("  %-10s %d 个声道，掩码 0x%03" PRIx64 "，顺序：", name, layout->nb_channels,
           layout->order == AV_CHANNEL_ORDER_NATIVE ? layout->u.mask : 0);

    for (int i = 0; i < layout->nb_channels; i++) {
        char ch[16];

        /* 第 i 个位置上是哪个声道。交错存放时，每个采样时刻的数据就按这个顺序排列。 */
        av_channel_name(ch, sizeof(ch), av_channel_layout_channel_from_index(layout, i));
        printf("%s%s", i ? " " : "", ch);
    }
    printf("\n");
}

int main(void)
{
    printf("== 1. 采样格式 ==\n");
    printf("  名字   字节  排列    packed 形式  planar 形式\n");
    /*
     * 6 种数据类型（u8、s16、s32、flt、dbl、s64），每种有 packed 和 planar 两种排列，
     * 名字以 p 结尾的是 planar。“字节”是一个采样占的字节数，不乘声道数。
     * 最后的 dsd 是 DSD 位流专用的格式，本课程不涉及。
     *
     * 循环没有用 AV_SAMPLE_FMT_NB 作上限。头文件里注明动态链接时不要用它：
     * 库升级之后格式可能变多，而 AV_SAMPLE_FMT_NB 的值在编译程序时就固定了。
     * 这里改用“取不到名字”作为结束条件。
     */
    for (enum AVSampleFormat fmt = 0; av_get_sample_fmt_name(fmt); fmt++) {
        printf("  %-6s %-5d %-7s %-12s %s\n",
               av_get_sample_fmt_name(fmt),
               av_get_bytes_per_sample(fmt),
               av_sample_fmt_is_planar(fmt) ? "planar" : "packed",
               av_get_sample_fmt_name(av_get_packed_sample_fmt(fmt)),
               av_get_sample_fmt_name(av_get_planar_sample_fmt(fmt)));
    }

    /*
     * PCM 没有压缩，数据量可以直接算出来：
     * 每秒字节数 = 采样率 × 声道数 × 每采样字节数。
     */
    printf("\n== 2. PCM 的数据量 ==\n");
    {
        const struct {
            const char *what;
            int sample_rate, channels;
            enum AVSampleFormat fmt;
        } cases[] = {
            { "电话语音  8000 Hz 单声道 s16", 8000,  1, AV_SAMPLE_FMT_S16 },
            { "CD       44100 Hz 双声道 s16", 44100, 2, AV_SAMPLE_FMT_S16 },
            { "解码器输出 44100 Hz 双声道 flt", 44100, 2, AV_SAMPLE_FMT_FLT },
            { "影院      48000 Hz 5.1   s32", 48000, 6, AV_SAMPLE_FMT_S32 },
        };

        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            const int bytes_per_sec = cases[i].sample_rate * cases[i].channels *
                                      av_get_bytes_per_sample(cases[i].fmt);

            printf("  %s：每秒 %7d 字节 = %7.1f kb/s，一分钟 %5.1f MB\n", cases[i].what,
                   bytes_per_sec, bytes_per_sec * 8 / 1000.0, bytes_per_sec * 60 / 1e6);
        }
    }

    /*
     * 音频也是一帧一帧处理的。一帧里每个声道有 nb_samples 个采样，这个数由编码格式决定。
     * 一帧的时长 = nb_samples / 采样率。
     * av_samples_get_buffer_size() 计算存放这些采样需要多少字节，第一个参数带回一个平面的大小。
     * 最后一个参数 1 表示不做对齐，得到的是准确的数据量。
     */
    printf("\n== 3. 一帧音频 ==\n");
    {
        const struct {
            const char *codec;
            int nb_samples, sample_rate;
            enum AVSampleFormat fmt;
        } cases[] = {
            { "MP2",  1152, 44100, AV_SAMPLE_FMT_S16P },
            { "AAC",  1024, 44100, AV_SAMPLE_FMT_FLTP },
            { "AAC",  1024, 48000, AV_SAMPLE_FMT_FLTP },
            { "Opus",  960, 48000, AV_SAMPLE_FMT_FLTP },
            { "FLAC", 4096, 44100, AV_SAMPLE_FMT_S16  },
        };

        printf("  双声道时：\n");
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            int plane_size;
            const int total = av_samples_get_buffer_size(&plane_size, 2, cases[i].nb_samples,
                                                         cases[i].fmt, 1);

            printf("  %-4s %5d Hz %-4s 一帧 %4d 个采样 = %5.2f 毫秒，共 %5d 字节，%d 个平面，每个平面 %5d 字节\n",
                   cases[i].codec, cases[i].sample_rate, av_get_sample_fmt_name(cases[i].fmt),
                   cases[i].nb_samples, cases[i].nb_samples * 1000.0 / cases[i].sample_rate,
                   total, av_sample_fmt_is_planar(cases[i].fmt) ? 2 : 1, plane_size);
        }
    }

    /*
     * 声道布局回答两个问题：有几个声道，每个声道对应哪个扬声器。
     * 只知道声道数是不够的：同样是 6 个声道，5.1 和 5.1(side) 的后两个声道位置不同。
     */
    printf("\n== 4. 声道布局 ==\n");
    {
        const char *names[] = { "mono", "stereo", "2.1", "5.1", "5.1(side)", "7.1" };
        AVChannelLayout layout = { 0 }, a = { 0 }, b = { 0 };

        for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            /* 用名字构造一个布局。AVChannelLayout 用完要 uninit。 */
            if (av_channel_layout_from_string(&layout, names[i]) < 0) {
                fprintf(stderr, "不认识的声道布局：%s\n", names[i]);
                return 1;
            }
            print_layout(&layout);
            av_channel_layout_uninit(&layout);
        }

        /* 只知道声道数时，用 av_channel_layout_default() 取这个声道数最常用的布局。 */
        printf("  只知道声道数时的默认布局：");
        for (int n = 1; n <= 8; n++) {
            char name[64];

            av_channel_layout_default(&layout, n);
            av_channel_layout_describe(&layout, name, sizeof(name));
            printf("%s%d → %s", n > 1 ? "，" : "", n, name);
            av_channel_layout_uninit(&layout);
        }
        printf("\n");

        /* 比较两个布局用 av_channel_layout_compare()，返回 0 表示相同。不要只比较声道数。 */
        av_channel_layout_from_string(&a, "5.1");
        av_channel_layout_from_string(&b, "5.1(side)");
        printf("  5.1 和 5.1(side) 声道数都是 %d，av_channel_layout_compare() 返回 %d（%s）\n",
               a.nb_channels, av_channel_layout_compare(&a, &b),
               av_channel_layout_compare(&a, &b) ? "不同" : "相同");
        av_channel_layout_uninit(&a);
        av_channel_layout_uninit(&b);
    }

    return 0;
}
