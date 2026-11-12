/*
 * L22 工程化：你编译用的 FFmpeg 和运行时加载的 FFmpeg 是不是同一套？是什么版本、什么许可证、带了什么？
 *
 * 用法：version_info        打印全部信息
 *       version_info -q     只在有问题时打印，没问题时什么也不输出；退出码 0 没问题，1 有问题（可以放进构建脚本或 CI）
 *
 * 为什么要关心：
 *   - 编译时用的是头文件（-I 指定的目录）里的声明，运行时加载的是 .so（LD_LIBRARY_PATH、rpath 或系统目录里找到的）。
 *     两者可以不是一套：机器上同时装了系统自带的 FFmpeg 和自己编译的，PKG_CONFIG_PATH 指向一个，LD_LIBRARY_PATH 指向另一个，
 *     程序能编译、能启动，直到调用到一个新版本才有的函数（undefined symbol），或者结构体的布局对不上（崩溃或者输出乱码）。
 *   - 同一个大版本号（major）里，库保证二进制兼容（只加不改）：运行时的库比头文件新可以；比头文件旧不行（可能缺函数）。
 *     大版本号不同，就是不兼容。
 *   - 许可证决定了你能不能发布自己的程序：LGPL 的库可以动态链接进闭源程序；带了 --enable-gpl 的库整个就是 GPL；
 *     --enable-nonfree 的库不能再分发。
 */
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

typedef struct Lib {
    const char *name;
    unsigned    compiled;           /* 编译时：头文件里的 LIBxxx_VERSION_INT */
    unsigned    loaded;             /* 运行时：库里 xxx_version() 返回的 */
    const char *license;
    const char *configuration;
} Lib;

static void fmt_version(char *buf, size_t size, unsigned v)
{
    snprintf(buf, size, "%u.%u.%u", AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v));
}

int main(int argc, char **argv)
{
    const Lib libs[] = {
        { "libavutil",     LIBAVUTIL_VERSION_INT,     avutil_version(),     avutil_license(),     avutil_configuration() },
        { "libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version(), swresample_license(), swresample_configuration() },
        { "libswscale",    LIBSWSCALE_VERSION_INT,    swscale_version(),    swscale_license(),    swscale_configuration() },
        { "libavcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version(),    avcodec_license(),    avcodec_configuration() },
        { "libavformat",   LIBAVFORMAT_VERSION_INT,   avformat_version(),   avformat_license(),   avformat_configuration() },
        { "libavfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version(),   avfilter_license(),   avfilter_configuration() },
    };
    const int quiet = argc > 1 && !strcmp(argv[1], "-q");
    const char *conf = avcodec_configuration();
    int problems = 0;

    if (argc > 1 && !quiet) {
        fprintf(stderr, "用法：%s [-q]\n", argv[0]);
        return 1;
    }

    if (!quiet) {
        printf("%-14s %-12s %-12s %s\n", "库", "编译时头文件", "运行时的库", "结论");
    }
    for (size_t i = 0; i < sizeof(libs) / sizeof(libs[0]); i++) {
        const Lib *l = &libs[i];
        char c[32], r[32];
        const char *verdict = "一致";
        int bad = 0;

        fmt_version(c, sizeof(c), l->compiled);
        fmt_version(r, sizeof(r), l->loaded);
        if (AV_VERSION_MAJOR(l->compiled) != AV_VERSION_MAJOR(l->loaded)) {
            verdict = "大版本不同：不兼容，头文件和库不是一套";
            bad = 1;
        } else if (l->loaded < l->compiled) {
            verdict = "运行时的库比头文件旧：可能缺少头文件里声明的函数";
            bad = 1;
        } else if (l->loaded > l->compiled) {
            verdict = "运行时的库比头文件新：兼容，没问题";
        }
        problems += bad;
        if (!quiet || bad)
            printf("%-14s %-12s %-12s %s\n", l->name, c, r, verdict);
    }
    if (quiet)
        return problems ? 1 : 0;

    /* 版本号的一个整数形式：major << 16 | minor << 8 | micro，所以可以直接比较大小 */
    printf("\n版本字符串（av_version_info）：%s\n", av_version_info());
    printf("许可证（avcodec_license）：%s\n", avcodec_license());
    printf("  含 GPL 部分：%s；含 nonfree：%s；LGPL 版本 3：%s\n",
           strstr(conf, "--enable-gpl") ? "是（整个库按 GPL 发布）" : "否",
           strstr(conf, "--enable-nonfree") ? "是（不能再分发）" : "否",
           strstr(conf, "--enable-version3") ? "是" : "否");
    printf("configure 参数（avcodec_configuration）：\n  ");
    for (const char *p = conf; *p; p++) {
        putchar(*p);
        if (*p == ' ' && (p - conf) % 100 > 90)
            printf("\n  ");
    }
    printf("\n");

    /*
     * 按版本号决定用哪个接口：头文件里的版本够新才编译这一段。
     * 版本号来自 doc/APIchanges，每一条写着“在哪个库的哪个版本加入”：
     *   2026-10-18 - lavf 63.11.100 - Add avio_open_dir2().
     *   2026-10-xx - lavfi 12.11.100 - Add avfilter_graph_set_max_buffered_frames().
     * 这两个是写这一讲时 master 上最新的接口，旧版本里没有。
     */
    printf("\n按版本号选择接口（编译时决定）：\n");
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(63, 11, 100)
    printf("  avio_open_dir2()：头文件有，可以用（lavf 63.11.100 加入，比 avio_open_dir() 多一个 options 参数）\n");
#else
    printf("  avio_open_dir2()：头文件里没有，只能用 avio_open_dir()\n");
#endif
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(12, 11, 100)
    printf("  avfilter_graph_set_max_buffered_frames()：头文件有，可以用（lavfi 12.11.100 加入，取代 AVFilterGraph.max_buffered_frames）\n");
#else
    printf("  avfilter_graph_set_max_buffered_frames()：头文件里没有，要直接设 AVFilterGraph.max_buffered_frames\n");
#endif

    /* 编译时有没有，和运行时的库里有没有，是两回事：头文件有、库里没有，链接或加载时才出错。上面的表就是用来发现这种情况的。 */
    printf("\n运行时能查到的编码器（有没有取决于 configure 带了什么）：\n");
    {
        static const char *names[] = { "aac", "libx264", "libx265", "libfdk_aac", "libmp3lame", "libvpx-vp9", "h264_vaapi" };

        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
            printf("  %-12s %s\n", names[i], avcodec_find_encoder_by_name(names[i]) ? "有" : "没有");
    }
    printf("\n编译进来的硬件设备类型（有这个类型不等于机器上有设备，L20 会用到）：\n ");
    {
        enum AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
        int n = 0;

        while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
            printf(" %s", av_hwdevice_get_type_name(t));
            n++;
        }
        printf("%s\n", n ? "" : " （一个也没有）");
    }
    return problems ? 1 : 0;
}
