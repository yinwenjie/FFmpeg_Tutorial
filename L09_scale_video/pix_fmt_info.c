/*
 * L09 像素格式转换与缩放：打印像素格式的内存布局。
 *
 * 用法：pix_fmt_info                      列出常用像素格式的对照表
 *       pix_fmt_info <像素格式> [宽x高]    打印一种格式的细节，默认按 640x360 计算大小
 *
 * 例子：
 *   pix_fmt_info
 *   pix_fmt_info yuv420p
 *   pix_fmt_info nv12 1920x1080
 *
 * 像素格式的全部信息都在 AVPixFmtDescriptor 里（libavutil/pixdesc.h），
 * 每个平面一行有多少字节、有多少行，由 libavutil/imgutils.h 里的函数计算。
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libavutil/parseutils.h>
#include <libavutil/pixdesc.h>

/* 一帧图像紧凑存放（行与行之间没有填充）时每个平面的大小。 */
static int plane_sizes(enum AVPixelFormat fmt, int w, int h, int linesizes[4], size_t sizes[4])
{
    ptrdiff_t ls[4];
    int ret;

    ret = av_image_fill_linesizes(linesizes, fmt, w);
    if (ret < 0)
        return ret;
    for (int i = 0; i < 4; i++)
        ls[i] = linesizes[i];
    return av_image_fill_plane_sizes(sizes, fmt, h, ls);
}

/* 色度采样的习惯叫法。 */
static const char *subsampling_name(const AVPixFmtDescriptor *desc)
{
    if (desc->flags & AV_PIX_FMT_FLAG_RGB)
        return "-";
    if (desc->nb_components < 3)
        return "-";
    if (desc->log2_chroma_w == 0 && desc->log2_chroma_h == 0)
        return "4:4:4";
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 0)
        return "4:2:2";
    if (desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1)
        return "4:2:0";
    if (desc->log2_chroma_w == 2 && desc->log2_chroma_h == 0)
        return "4:1:1";
    return "其他";
}

static void print_table(void)
{
    static const enum AVPixelFormat list[] = {
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12,   AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUYV422,
        AV_PIX_FMT_YUV444P, AV_PIX_FMT_YUV420P10LE, AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_RGB24,   AV_PIX_FMT_BGR24,  AV_PIX_FMT_RGBA,    AV_PIX_FMT_BGRA,
    };
    const int w = 640, h = 360;

    printf("%-13s %-5s %-6s %-8s %-9s %s\n", "名字", "平面", "采样", "每像素位", "每分量位", "640x360 一帧的字节数");
    for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); i++) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(list[i]);

        printf("%-11s %4d   %-5s %6d %8d %12d\n", desc->name, av_pix_fmt_count_planes(list[i]),
               subsampling_name(desc), av_get_bits_per_pixel(desc), desc->comp[0].depth,
               av_image_get_buffer_size(list[i], w, h, 1));
    }
}

static int print_one(const char *name, int w, int h)
{
    const enum AVPixelFormat fmt = av_get_pix_fmt(name);
    const AVPixFmtDescriptor *desc;
    /* 分量的名字：RGB 格式是 R、G、B、A，其余是 Y、U、V、A。 */
    const char *comp_names;
    int linesizes[4], ret, nb_planes;
    size_t sizes[4];

    if (fmt == AV_PIX_FMT_NONE) {
        fprintf(stderr, "不认识的像素格式：%s（用 ffmpeg -pix_fmts 可以列出全部名字）\n", name);
        return AVERROR(EINVAL);
    }
    desc       = av_pix_fmt_desc_get(fmt);
    nb_planes  = av_pix_fmt_count_planes(fmt);
    comp_names = (desc->flags & AV_PIX_FMT_FLAG_RGB) ? "RGBA" : "YUVA";

    printf("像素格式 %s\n", desc->name);
    printf("  分量 %d 个，平面 %d 个，平均每像素 %d 位\n", desc->nb_components, nb_planes,
           av_get_bits_per_pixel(desc));
    if (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) {
        printf("  硬件格式：数据在显卡里，不在内存里，没有平面可言（L20 讲）\n");
        return 0;
    }
    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB) && desc->nb_components >= 3)
        printf("  色度采样：%s（色度的宽是亮度的 1/%d，高是亮度的 1/%d）\n", subsampling_name(desc),
               1 << desc->log2_chroma_w, 1 << desc->log2_chroma_h);
    printf("  特点：%s%s%s%s%s\n",
           (desc->flags & AV_PIX_FMT_FLAG_PLANAR)  ? "planar（至少有两个分量各占一个平面） " : "packed（分量交错放在一起） ",
           (desc->flags & AV_PIX_FMT_FLAG_RGB)     ? "RGB " : "",
           (desc->flags & AV_PIX_FMT_FLAG_ALPHA)   ? "带透明通道 " : "",
           (desc->flags & AV_PIX_FMT_FLAG_BE)      ? "大端 " : "",
           (desc->flags & AV_PIX_FMT_FLAG_PAL)     ? "调色板 " : "");

    /*
     * 每个分量在哪个平面、怎么取：
     * plane 是平面下标，offset 是这个分量在一个像素里的起始字节，
     * step 是同一分量相邻两个值之间隔多少字节，depth 是有效位数。
     */
    for (int i = 0; i < desc->nb_components; i++) {
        const AVComponentDescriptor *c = &desc->comp[i];

        printf("  分量 %c：在平面 %d，起始偏移 %d 字节，步长 %d 字节，%d 位\n", comp_names[i],
               c->plane, c->offset, c->step, c->depth);
    }

    ret = plane_sizes(fmt, w, h, linesizes, sizes);
    if (ret < 0) {
        fprintf(stderr, "算不出 %dx%d 的大小：%s\n", w, h, av_err2str(ret));
        return ret;
    }
    printf("%dx%d 的一帧（不带填充）：\n", w, h);
    for (int i = 0; i < nb_planes; i++)
        printf("  平面 %d：每行 %d 字节，%zu 行，共 %zu 字节\n", i, linesizes[i],
               linesizes[i] ? sizes[i] / linesizes[i] : 0, sizes[i]);
    printf("  合计 %d 字节\n", av_image_get_buffer_size(fmt, w, h, 1));
    return 0;
}

int main(int argc, char **argv)
{
    int w = 640, h = 360;

    if (argc == 1) {
        print_table();
        return 0;
    }
    if (argc > 3) {
        fprintf(stderr, "用法：%s [<像素格式> [宽x高]]\n", argv[0]);
        return 1;
    }
    if (argc == 3 && av_parse_video_size(&w, &h, argv[2]) < 0) {
        fprintf(stderr, "尺寸要写成 宽x高，例如 1920x1080\n");
        return 1;
    }
    return print_one(argv[1], w, h) < 0 ? 1 : 0;
}
