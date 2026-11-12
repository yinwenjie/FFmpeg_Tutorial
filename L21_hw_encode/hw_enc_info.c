/*
 * L21 硬件编码：这份 FFmpeg、这台机器，能做哪些硬件编码？
 *
 * 用法：hw_enc_info [选项]
 *   codec=名字        只看这一个编码器（例如 codec=h264_vaapi），不写就列出这份 FFmpeg 里所有的硬件编码器
 *   try=1             对每种编译进来的设备类型真的创建一次设备；创建成功的再打印它的限制（硬件帧格式、能上传的内存格式、尺寸范围）
 *   device=路径       创建设备时用的设备名（VAAPI 是 /dev/dri/renderD128 这样的路径），不写就用默认设备
 *   verbose=1         把日志级别提到 verbose，创建失败时能看到硬件后端报告的原因
 *
 * 退出码：0 正常打印完；1 用法错误或者找不到 codec= 指定的编码器。（有没有设备不影响退出码。）
 *
 * 和 L20 的 hw_info 是一对。解码时问“这个解码器能用哪些设备”；编码时多一个问题：
 * 编码器要的输入是“显存里的帧”，所以要知道它的输入格式（硬件格式）、我们要自己建的帧上下文该用什么内存格式（sw_format）。
 *   1. FFmpeg 编译时带了哪几种设备类型？          （av_hwdevice_iterate_types）
 *   2. 有哪些硬件编码器，各自要什么？             （遍历所有编码器，avcodec_get_hw_config、avcodec_get_supported_config）
 *   3. 机器上有没有这种设备、能不能打开？         （av_hwdevice_ctx_create）
 *   4. 设备能不能接受这个尺寸和格式的帧？         （av_hwdevice_get_hwframe_constraints）
 * 前两层不需要显卡就能查；后两层要看机器。
 */
#define _POSIX_C_SOURCE 200809L

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

/* 名字里带这些后缀的编码器，是名字带后缀的“专用”硬件编码器。 */
static int is_hw_name(const char *name)
{
    static const char *suffix[] = {
        "_vaapi", "_qsv", "_nvenc", "_amf", "_v4l2m2m", "_videotoolbox", "_mediacodec", "_vulkan", "_cuvid", "_omx", "_mf",
    };

    for (size_t i = 0; i < sizeof(suffix) / sizeof(suffix[0]); i++) {
        size_t n = strlen(suffix[i]), len = strlen(name);

        if (len > n && !strcmp(name + len - n, suffix[i]))
            return 1;
    }
    return 0;
}

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

/* 编码器接受的输入像素格式。 */
static void print_input_formats(const AVCodec *c)
{
    const enum AVPixelFormat *fmts = NULL;
    int n = 0;

    printf("      输入格式：");
    if (avcodec_get_supported_config(NULL, c, AV_CODEC_CONFIG_PIX_FORMAT, 0, (const void **)&fmts, &n) < 0 || !fmts) {
        printf(" （没有列出）\n");
        return;
    }
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        printf(" %s", pix_name(*p));
    printf("\n");
}

/* 第 2 层：一个编码器要什么。返回 1 表示有这个编码器。 */
static int print_encoder(const AVCodec *c)
{
    int has_cfg = 0;

    printf("   %-18s %s\n", c->name, c->long_name ? c->long_name : "");
    print_input_formats(c);
    for (int i = 0;; i++) {
        const AVCodecHWConfig *cfg = avcodec_get_hw_config(c, i);

        if (!cfg)
            break;
        printf("      硬件配置：%-8s 帧格式 %-8s 方法 ", av_hwdevice_get_type_name(cfg->device_type), pix_name(cfg->pix_fmt));
        print_methods(cfg->methods);
        printf("\n");
        has_cfg = 1;
    }
    if (!has_cfg)
        printf("      硬件配置：（没有：要么是软件编码器，要么是自己管设备的专用编码器，不走 avcodec_get_hw_config 这套）\n");
    printf("      标记：%s%s\n", (c->capabilities & AV_CODEC_CAP_HARDWARE) ? "硬件编码器 " : "",
           (c->capabilities & AV_CODEC_CAP_HYBRID) ? "可能用硬件也可能用软件" : "");
    return 1;
}

static int print_encoders(const char *only)
{
    const AVCodec *c;
    void *it = NULL;
    int n = 0;

    printf("\n2. 硬件编码器（名字带后缀的，以及编码器自己说有硬件配置的）：\n");
    if (only) {
        c = avcodec_find_encoder_by_name(only);
        if (!c) {
            printf("   这份 FFmpeg 里没有编码器 %s\n", only);
            return -1;
        }
        return print_encoder(c);
    }
    while ((c = av_codec_iterate(&it))) {
        if (!av_codec_is_encoder(c))
            continue;
        if (is_hw_name(c->name) || avcodec_get_hw_config(c, 0))
            n += print_encoder(c);
    }
    if (!n)
        printf("   （一个也没有）\n");
    return n;
}

static void print_dri(void)
{
    printf("\n3. 机器上的设备：\n");
    if (access("/dev/dri", F_OK) != 0)
        printf("   /dev/dri 不存在：没有显卡，或者这是没有直通显卡的虚拟机、容器。VAAPI 和 DRM 肯定用不了。\n");
    else
        printf("   /dev/dri 存在（用 ls -l /dev/dri 看有哪些设备和读写权限；有权限的才能用，要加入 render 用户组）。\n");
}

static void print_formats(const char *title, const enum AVPixelFormat *f)
{
    printf("      %s：", title);
    for (; f && *f != AV_PIX_FMT_NONE; f++)
        printf(" %s", pix_name(*f));
    printf("\n");
}

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
            print_formats("可以上传的内存格式（建帧上下文的 sw_format 要从这里选）", c->valid_sw_formats);
            printf("      尺寸范围：%dx%d 到 %dx%d\n", c->min_width, c->min_height, c->max_width, c->max_height);
            av_hwframe_constraints_free(&c);
        }
        av_buffer_unref(&dev);
    }
}

int main(int argc, char **argv)
{
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
            fprintf(stderr, "用法：%s [codec=名字] [try=1] [device=路径] [verbose=1]\n", argv[0]);
            return 1;
        }
    }
    if (verbose)
        av_log_set_level(AV_LOG_VERBOSE);

    print_device_types();
    if (print_encoders(only) < 0)
        return 1;
    print_dri();
    if (try_create)
        try_devices(device);
    else
        printf("\n（加 try=1 会对每种设备类型真的创建一次设备，看有没有、能不能用，并问设备的限制）\n");
    return 0;
}
