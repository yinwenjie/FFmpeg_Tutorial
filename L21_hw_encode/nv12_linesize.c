/*
 * L21：为什么不能直接往 AVFrame 里读像素？——官方示例 vaapi_encode.c 的 patch 0099。
 *
 * 用法：
 *   nv12_linesize table [宽 ...]
 *       打印 av_frame_get_buffer(frame, 0) 给出的每一行的字节数 linesize，和“宽”比较。默认列出一批常见的宽。
 *   nv12_linesize naive|fixed <宽> <高> <输入.nv12> <输出.nv12>
 *       把一个紧密排列的 NV12 文件逐帧读进 AVFrame，再从 AVFrame 里原样写出来（也是紧密排列）。
 *       naive：官方原版的读法，直接 fread() 到 data[0]、data[1]，假设每一行恰好“宽”个字节；
 *       fixed：patch 0099 的读法，先读进一块紧密排列的缓冲区，再用 av_image_copy2() 一行一行拷进去。
 *       输入和输出应当一模一样。fixed 总是一模一样；naive 只有当 linesize 恰好等于宽时才一样。
 *
 * 退出码：0 成功；1 参数错误、读写出错、输入文件的大小不是整数帧。
 *
 * 为什么 linesize 会大于宽：av_frame_get_buffer(frame, 0) 为了让 CPU 的向量指令（SSE、AVX）能高效地读，
 * 把每一行的起点都对齐到一个值（align 是 0 的时候 FFmpeg 自己选，取决于这台机器的 CPU，常见是 32 或 64）。
 * 宽恰好是对齐值的倍数（320、640、1280、1920）时 linesize 等于宽，直接读碰巧不出错；
 * 宽不是倍数（352、1366……）时每一行末尾有填充，直接读就把每行多出来的字节挤到了下一行，画面是歪的。
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/cpu.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>

static int parse_dim(const char *s, int *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < 2 || v > 16384)
        return -1;
    *out = (int)v;
    return 0;
}

static int cmd_table(int argc, char **argv)
{
    static const int def[] = { 176, 320, 352, 640, 720, 854, 1000, 1280, 1366, 1920 };
    int widths[64], n = 0;

    for (int i = 2; i < argc && n < 64; i++) {
        if (parse_dim(argv[i], &widths[n]) < 0) {
            fprintf(stderr, "宽要是 2 到 16384 之间的整数：%s\n", argv[i]);
            return 1;
        }
        n++;
    }
    if (!n) {
        memcpy(widths, def, sizeof(def));
        n = sizeof(def) / sizeof(def[0]);
    }
    printf("这台机器上 av_cpu_max_align() = %zu\n", av_cpu_max_align());
    printf("%6s %12s %12s %8s\n", "宽", "linesize[0]", "linesize[1]", "每行填充");
    for (int i = 0; i < n; i++) {
        AVFrame *f = av_frame_alloc();
        int ret;

        if (!f)
            return 1;
        f->format = AV_PIX_FMT_NV12;
        f->width  = widths[i];
        f->height = 64;
        ret = av_frame_get_buffer(f, 0);
        if (ret < 0) {
            fprintf(stderr, "分配 %d 宽的帧失败\n", widths[i]);
            av_frame_free(&f);
            return 1;
        }
        printf("%6d %12d %12d %8d%s\n", widths[i], f->linesize[0], f->linesize[1], f->linesize[0] - widths[i],
               f->linesize[0] == widths[i] ? "    （等于宽：直接读碰巧不出错）" : "");
        av_frame_free(&f);
    }
    return 0;
}

static int cmd_copy(int naive, int argc, char **argv)
{
    int w, h, size, ret = 1, frames = 0;
    FILE *in = NULL, *out = NULL;
    AVFrame *f = NULL;
    uint8_t *packed = NULL, *pdata[4];
    int plinesize[4];

    if (argc != 6) {
        fprintf(stderr, "用法：%s naive|fixed <宽> <高> <输入.nv12> <输出.nv12>\n", argv[0]);
        return 1;
    }
    if (parse_dim(argv[2], &w) < 0 || parse_dim(argv[3], &h) < 0) {
        fprintf(stderr, "宽和高要是 2 到 16384 之间的整数：%s %s\n", argv[2], argv[3]);
        return 1;
    }
    size = av_image_get_buffer_size(AV_PIX_FMT_NV12, w, h, 1);
    if (size <= 0)
        return 1;
    if (!(in = fopen(argv[4], "rb"))) {
        fprintf(stderr, "打不开输入文件 %s：%s\n", argv[4], strerror(errno));
        return 1;
    }
    if (!(out = fopen(argv[5], "wb"))) {
        fprintf(stderr, "打不开输出文件 %s：%s\n", argv[5], strerror(errno));
        goto end;
    }
    packed = av_malloc(size);
    f = av_frame_alloc();
    if (!packed || !f)
        goto end;
    av_image_fill_arrays(pdata, plinesize, packed, AV_PIX_FMT_NV12, w, h, 1);
    f->format = AV_PIX_FMT_NV12;
    f->width  = w;
    f->height = h;
    if (av_frame_get_buffer(f, 0) < 0)
        goto end;
    /* 官方原版没有这一步：av_frame_get_buffer() 分配的内存没有清零，naive 读不满的行尾和帧尾是没初始化的内存，
     * 每次运行内容都可能不一样。这里先清零，让演示的结果可重复（valgrind 也不会报“用了没初始化的数据”）。 */
    for (int i = 0; i < AV_NUM_DATA_POINTERS && f->buf[i]; i++)
        memset(f->buf[i]->data, 0, f->buf[i]->size);

    for (;;) {
        if (naive) {
            /* 官方原版：Y 平面读 宽x高 字节，UV 平面读 宽x高/2 字节，都直接读进帧的平面里。 */
            size_t n = fread(f->data[0], 1, (size_t)w * h, in);

            if (n == 0 && !ferror(in))
                break;
            if (n != (size_t)w * h || fread(f->data[1], 1, (size_t)w * h / 2, in) != (size_t)w * h / 2) {
                fprintf(stderr, "输入文件的大小不是整数帧（一帧 %d 字节）\n", size);
                goto end;
            }
        } else {
            size_t n = fread(packed, 1, size, in);

            if (n == 0 && !ferror(in))
                break;
            if (n != (size_t)size) {
                fprintf(stderr, "输入文件的大小不是整数帧（一帧 %d 字节）\n", size);
                goto end;
            }
            av_image_copy2(f->data, f->linesize, pdata, plinesize, AV_PIX_FMT_NV12, w, h);
        }
        /* 从帧里原样取出来：av_image_copy_to_buffer 按 linesize 一行一行读，写成紧密排列 */
        if (av_image_copy_to_buffer(packed, size, (const uint8_t *const *)f->data, f->linesize, AV_PIX_FMT_NV12, w, h, 1) < 0)
            goto end;
        if (fwrite(packed, 1, size, out) != (size_t)size) {
            fprintf(stderr, "写输出文件出错：%s\n", strerror(errno));
            goto end;
        }
        frames++;
    }
    if (ferror(in)) {
        fprintf(stderr, "读输入文件出错：%s\n", strerror(errno));
        goto end;
    }
    printf("%s：%d 帧，%dx%d，linesize[0] = %d（宽 %d）\n", naive ? "naive" : "fixed", frames, w, h, f->linesize[0], w);
    ret = 0;

end:
    if (out && fclose(out) != 0 && ret == 0) {
        fprintf(stderr, "写输出文件出错：%s\n", strerror(errno));
        ret = 1;
    }
    if (in)
        fclose(in);
    av_freep(&packed);
    av_frame_free(&f);
    return ret;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "table"))
        return cmd_table(argc, argv);
    if (argc >= 2 && (!strcmp(argv[1], "naive") || !strcmp(argv[1], "fixed")))
        return cmd_copy(!strcmp(argv[1], "naive"), argc, argv);
    fprintf(stderr, "用法：\n  %s table [宽 ...]\n  %s naive|fixed <宽> <高> <输入.nv12> <输出.nv12>\n", argv[0], argv[0]);
    return 1;
}
