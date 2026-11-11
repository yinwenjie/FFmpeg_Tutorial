/*
 * L02 hello_ffmpeg：检查 FFmpeg 开发环境是否可用。
 *
 * 做三件事：
 *   1. 对比“编译时用的头文件版本”和“运行时加载的动态库版本”；
 *   2. 打印这份 FFmpeg 的许可证和 configure 选项；
 *   3. 统计它支持多少封装格式、编解码器、滤镜、协议，
 *      并确认后面课程要用到的几个组件确实存在。
 *
 * 全部检查通过时返回 0，否则返回 1，方便在脚本和 CI 里使用。
 */
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* 版本号的主版本变化意味着 ABI 不兼容，头文件和运行库必须一致。 */
static int check_versions(void)
{
    const struct {
        const char *name;
        unsigned    header;  /* 编译时：头文件里的宏 */
        unsigned    runtime; /* 运行时：实际加载的库返回的值 */
    } libs[] = {
        { "libavutil",     LIBAVUTIL_VERSION_INT,     avutil_version()     },
        { "libavcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version()    },
        { "libavformat",   LIBAVFORMAT_VERSION_INT,   avformat_version()   },
        { "libavdevice",   LIBAVDEVICE_VERSION_INT,   avdevice_version()   },
        { "libavfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version()   },
        { "libswscale",    LIBSWSCALE_VERSION_INT,    swscale_version()    },
        { "libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version() },
    };
    int ok = 1;

    /* 表头含中文，printf 的宽度按字节计算会对不齐，所以直接写好空格。 */
    printf("库             头文件       运行库\n");
    for (size_t i = 0; i < ARRAY_SIZE(libs); i++) {
        unsigned h = libs[i].header, r = libs[i].runtime;
        int same_major = AV_VERSION_MAJOR(h) == AV_VERSION_MAJOR(r);

        printf("%-14s %3u.%3u.%3u  %3u.%3u.%3u%s\n", libs[i].name,
               AV_VERSION_MAJOR(h), AV_VERSION_MINOR(h), AV_VERSION_MICRO(h),
               AV_VERSION_MAJOR(r), AV_VERSION_MINOR(r), AV_VERSION_MICRO(r),
               same_major ? "" : "  <-- 主版本不一致");
        if (!same_major)
            ok = 0;
    }
    return ok;
}

static void print_counts(void)
{
    void *it;
    int demuxers = 0, muxers = 0, decoders = 0, encoders = 0, filters = 0;
    int in_protocols = 0, out_protocols = 0;
    const AVCodec *codec;

    it = NULL;
    while (av_demuxer_iterate(&it))
        demuxers++;
    it = NULL;
    while (av_muxer_iterate(&it))
        muxers++;
    it = NULL;
    while ((codec = av_codec_iterate(&it))) {
        if (av_codec_is_decoder(codec))
            decoders++;
        if (av_codec_is_encoder(codec))
            encoders++;
    }
    it = NULL;
    while (av_filter_iterate(&it))
        filters++;
    it = NULL;
    while (avio_enum_protocols(&it, 0))
        in_protocols++;
    it = NULL;
    while (avio_enum_protocols(&it, 1))
        out_protocols++;

    printf("解封装格式 %d，封装格式 %d\n", demuxers, muxers);
    printf("解码器 %d，编码器 %d\n", decoders, encoders);
    printf("滤镜 %d\n", filters);
    printf("输入协议 %d，输出协议 %d\n", in_protocols, out_protocols);
}

/* 后续课程依赖的组件。缺少任何一个都说明 configure 选项或依赖库不对。 */
static int check_components(void)
{
    const struct {
        const char *what;
        int         found;
    } items[] = {
        { "H.264 解码器 h264",          avcodec_find_decoder(AV_CODEC_ID_H264) != NULL },
        { "H.264 编码器 libx264 (L13)", avcodec_find_encoder_by_name("libx264") != NULL },
        { "AAC 编码器 aac (L14)",       avcodec_find_encoder_by_name("aac") != NULL },
        { "MJPEG 编码器 mjpeg (L13)",   avcodec_find_encoder_by_name("mjpeg") != NULL },
        { "MP4 封装 mp4 (L15)",         av_guess_format("mp4", NULL, NULL) != NULL },
        { "滤镜 scale (L12)",           avfilter_get_by_name("scale") != NULL },
        { "滤镜 drawtext (L12)",        avfilter_get_by_name("drawtext") != NULL },
    };
    int ok = 1;

    for (size_t i = 0; i < ARRAY_SIZE(items); i++) {
        printf("[%s] %s\n", items[i].found ? " OK " : "缺失", items[i].what);
        if (!items[i].found)
            ok = 0;
    }
    return ok;
}

int main(void)
{
    int ok = 1;

    printf("FFmpeg 版本：%s\n", av_version_info());
    printf("许可证：%s\n\n", avutil_license());

    ok &= check_versions();

    printf("\nconfigure 选项：\n%s\n\n", avutil_configuration());

    print_counts();
    printf("\n");
    ok &= check_components();

    printf("\n%s\n", ok ? "环境检查通过。" : "环境检查未通过，请对照 README 排查。");
    return ok ? 0 : 1;
}
