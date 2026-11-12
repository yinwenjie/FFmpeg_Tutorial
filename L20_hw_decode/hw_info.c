/*
 * L20 硬件解码：这份 FFmpeg、这台机器，能做哪些硬件解码？
 *
 * 用法：hw_info [选项]
 *   try=1             对每种编译进来的设备类型真的创建一次设备，打印结果；创建成功的再打印它的限制（支持的格式、尺寸）
 *   device=路径       创建设备时用的设备名（VAAPI 是 /dev/dri/renderD128 这样的路径），不写就用默认设备
 *   verbose=1         把日志级别提到 verbose，创建失败时能看到硬件后端报告的原因
 *   codec=名字        只看这一个解码器的硬件配置表（例如 codec=h264），不写就看常见的一批
 *
 * 退出码：0 正常打印完；1 用法错误。（有没有设备不影响退出码：没有显卡也是一个有效的结论。）
 *
 * 为什么要有这个程序：写硬件解码的程序，第一件事是搞清楚四层有没有：
 *   1. FFmpeg 编译时带了哪几种设备类型？            （--enable-vaapi 等，av_hwdevice_iterate_types）
 *   2. 这个解码器支持哪几种？                        （avcodec_get_hw_config）
 *   3. 机器上有没有这种设备、能不能打开？             （/dev/dri 里有没有、av_hwdevice_ctx_create）
 *   4. 设备支持这个视频的格式、尺寸吗？               （av_hwdevice_get_hwframe_constraints，真正解码时才最终确定）
 * 前两层是 FFmpeg 这份库的事，不需要显卡就能查；后两层要看机器。
 */
#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>

static const char *pix_name(enum AVPixelFormat f)
{
    const char *n = av_get_pix_fmt_name(f);

    return n ? n : "?";
}

/* methods 的四个位，用名字打印出来。 */
static void print_methods(int methods)
{
    static const struct { int flag; const char *name; } t[] = {
        { AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX, "设备上下文" },
        { AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX, "帧上下文" },
        { AV_CODEC_HW_CONFIG_METHOD_INTERNAL,       "内部" },
        { AV_CODEC_HW_CONFIG_METHOD_AD_HOC,         "需额外设置" },
    };
    int first = 1;

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (methods & t[i].flag) {
            printf("%s%s", first ? "" : "+", t[i].name);
            first = 0;
        }
    }
    if (first)
        printf("-");
}

/* 第 1 层：编译进来的设备类型。返回个数。 */
static int print_device_types(void)
{
    enum AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;
    int n = 0;

    printf("1. 这份 FFmpeg 编译进来的硬件设备类型（%s）：\n  ", av_version_info());
    while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
        printf(" %s", av_hwdevice_get_type_name(t));
        n++;
    }
    printf("%s\n", n ? "" : " （一个也没有）");
    return n;
}

/* 第 2 层：一个解码器的硬件配置表。返回这个解码器的配置项数。 */
static int print_hw_configs(const char *name)
{
    const AVCodec *codec = avcodec_find_decoder_by_name(name);
    int n = 0;

    if (!codec) {
        printf("  %-10s （这份 FFmpeg 里没有这个解码器）\n", name);
        return 0;
    }
    for (int i = 0;; i++) {
        const AVCodecHWConfig *c = avcodec_get_hw_config(codec, i);

        if (!c)
            break;
        printf("  %-10s %-8s 帧格式 %-8s 方法 ", n ? "" : name, av_hwdevice_get_type_name(c->device_type),
               pix_name(c->pix_fmt));
        print_methods(c->methods);
        printf("\n");
        n++;
    }
    if (!n)
        printf("  %-10s （没有硬件配置：要么只能软件解码，要么是自己管设备的专用解码器）\n", name);
    return n;
}

/* 专用的硬件解码器：名字带后缀（h264_cuvid、h264_qsv……），是单独的解码器，不走 avcodec_get_hw_config 这条路。 */
static void print_dedicated(void)
{
    static const char *names[] = {
        "h264_cuvid", "hevc_cuvid", "vp9_cuvid", "av1_cuvid",
        "h264_qsv", "hevc_qsv", "vp9_qsv", "av1_qsv", "mpeg2_qsv",
        "h264_v4l2m2m", "hevc_v4l2m2m", "vp9_v4l2m2m",
        "h264_mediacodec", "hevc_mediacodec",
    };
    int have = 0;

    printf("   有的：");
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (avcodec_find_decoder_by_name(names[i])) {
            printf(" %s", names[i]);
            have++;
        }
    }
    printf("%s\n", have ? "" : " （一个也没有）");
}

/* 第 3 层之一：/dev/dri 里有什么、能不能读写。 */
static void print_dri(void)
{
    DIR *d = opendir("/dev/dri");
    struct dirent *e;
    int n = 0;

    printf("3. 机器上的设备：\n");
    if (!d) {
        printf("   /dev/dri 不存在：没有显卡，或者这是没有直通显卡的虚拟机、容器。VAAPI 和 DRM 肯定用不了。\n");
        return;
    }
    while ((e = readdir(d))) {
        char path[300];

        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "/dev/dri/%s", e->d_name);
        printf("   %-22s 读写权限：%s\n", path, access(path, R_OK | W_OK) == 0 ? "有" : "没有（要加入 render 用户组）");
        n++;
    }
    closedir(d);
    if (!n)
        printf("   /dev/dri 是空的。\n");
}

static void print_formats(const char *title, const enum AVPixelFormat *f)
{
    printf("      %s：", title);
    for (; f && *f != AV_PIX_FMT_NONE; f++)
        printf(" %s", pix_name(*f));
    printf("\n");
}

/* 第 3、4 层：真的创建设备，成功了再问它的限制。 */
static void try_devices(const char *device)
{
    enum AVHWDeviceType t = AV_HWDEVICE_TYPE_NONE;

    printf("\n4. 逐个创建设备（device=%s）：\n", device ? device : "默认");
    while ((t = av_hwdevice_iterate_types(t)) != AV_HWDEVICE_TYPE_NONE) {
        AVBufferRef *dev = NULL;
        AVHWFramesConstraints *c;
        int ret = av_hwdevice_ctx_create(&dev, t, device, NULL, 0);

        printf("   %-8s ", av_hwdevice_get_type_name(t));
        if (ret < 0) {
            printf("创建失败：%s（%d）\n", av_err2str(ret), ret);
            continue;
        }
        printf("创建成功\n");
        c = av_hwdevice_get_hwframe_constraints(dev, NULL);
        if (!c) {
            printf("      查不到限制\n");
        } else {
            print_formats("硬件帧格式", c->valid_hw_formats);
            print_formats("能传回内存的格式", c->valid_sw_formats);
            printf("      尺寸范围：%dx%d 到 %dx%d\n", c->min_width, c->min_height, c->max_width, c->max_height);
            av_hwframe_constraints_free(&c);
        }
        av_buffer_unref(&dev);
    }
}

int main(int argc, char **argv)
{
    static const char *common[] = {
        "h264", "hevc", "vp8", "vp9", "av1", "mpeg2video", "mpeg4", "vc1", "mjpeg",
    };
    const char *device = NULL, *only = NULL;
    int try_create = 0, verbose = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "try=1"))
            try_create = 1;
        else if (!strcmp(argv[i], "verbose=1"))
            verbose = 1;
        else if (!strncmp(argv[i], "device=", 7))
            device = argv[i] + 7;
        else if (!strncmp(argv[i], "codec=", 6))
            only = argv[i] + 6;
        else {
            fprintf(stderr, "用法：%s [try=1] [device=路径] [verbose=1] [codec=名字]\n", argv[0]);
            return 1;
        }
    }
    if (verbose)
        av_log_set_level(AV_LOG_VERBOSE);

    print_device_types();

    printf("\n2. 解码器的硬件配置表（avcodec_get_hw_config，不需要显卡就能查）：\n");
    if (only) {
        print_hw_configs(only);
    } else {
        for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++)
            print_hw_configs(common[i]);
    }
    printf("   专用的硬件解码器（单独的解码器，名字带后缀）：\n");
    print_dedicated();

    printf("\n");
    print_dri();

    if (try_create)
        try_devices(device);
    else
        printf("\n（加 try=1 会对每种设备类型真的创建一次设备，看有没有、能不能用）\n");
    return 0;
}
