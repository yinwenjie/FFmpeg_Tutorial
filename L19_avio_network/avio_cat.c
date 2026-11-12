/*
 * avio_cat.c —— 课程 L19：通过协议层“读一个 URL、写到另一个 URL”
 *
 * 像 cat 或者 curl，但读写都走 libavformat 的协议层，用的是 avio_open2()。
 * 一个 URL 可以是文件名、file:路径、pipe:0 / pipe:1、http://、tcp:// ……，同一套代码对它们一视同仁，
 * 这就是协议层的意义：avformat_open_input() 和 avformat_alloc_output_context2() + avio_open() 底下用的也是它。
 * 可以用来观察：
 *   - 同一个程序读本地文件、读 HTTP、读管道，行为有什么不同（能不能 seek、一共多大、MIME 类型）；
 *   - HTTP 出错时 libavformat 给的错误码（404、403 ……）；
 *   - seek 在 HTTP 上是怎么实现的（服务器支持 Range 才行）；
 *   - 选项怎么传给协议（headers、user_agent、timeout ……）。
 *
 * 用法：
 *   avio_cat <输入URL> [输出URL] [名字=值 ...]
 *   avio_cat -p                 列出这个 libavformat 支持的协议（输入的、输出的）
 *
 *   输出URL 不写就是 pipe:1，也就是标准输出。
 *
 *   skip=N        先 seek 到输入的第 N 个字节（SEEK_SET）再读，默认 0 不 seek
 *   count=N       最多复制 N 个字节，默认复制到末尾
 *   bufsize=N     复制用的缓冲区的大小，默认 32768
 *   info=0|1      把输入的信息（协议名、大小、能不能 seek、MIME 类型）和复制的结果写到 stderr，默认 0
 *   其他 名字=值  当作协议选项交给输入，比如 user_agent=abc、headers="X-Test: 1\r\n"、timeout=2000000、seekable=0
 *
 * 退出码：0 正常；1 用法错误；2 输入打不开；3 seek 失败；4 读出错；5 输出打不开或写出错。
 */

/* -std=c11 下要这一行，才能用 POSIX 的 clock_gettime */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

/* 列出所有协议。avio_enum_protocols() 是协议层唯一公开的接口：协议层本身不是公开 API，只能这样问有哪些。 */
static void list_protocols(void)
{
    for (int output = 0; output <= 1; output++) {
        void *opaque = NULL;
        const char *name;
        int n = 0;

        printf("%s：", output ? "可以写的" : "可以读的");
        while ((name = avio_enum_protocols(&opaque, output)))
            printf("%s%s", n++ ? " " : "", name);
        printf("\n");
    }
}

/* 把输入的情况写到 stderr */
static void print_info(AVIOContext *in, const char *url)
{
    int64_t size = avio_size(in);
    uint8_t *mime = NULL;

    fprintf(stderr, "协议：%s\n", avio_find_protocol_name(url));
    if (size < 0)
        fprintf(stderr, "大小：未知（%s）\n", av_err2str((int)size));
    else
        fprintf(stderr, "大小：%" PRId64 " 字节\n", size);
    /* seekable 是位掩码：AVIO_SEEKABLE_NORMAL 是能按字节 seek，AVIO_SEEKABLE_TIME 是能按时间 seek */
    fprintf(stderr, "能 seek：%s\n", (in->seekable & AVIO_SEEKABLE_NORMAL) ? "是" : "否");
    /* 只有 http 一类的协议有 mime_type 这个选项，别的协议 av_opt_get() 返回负数 */
    if (av_opt_get(in, "mime_type", AV_OPT_SEARCH_CHILDREN, &mime) >= 0 && mime) {
        fprintf(stderr, "MIME 类型：%s\n", mime);
        av_freep(&mime);
    }
}

/* "名字=值"：= 前面只有字母、数字、下划线。http://x/?a=1 这样的 URL 不是选项（前面有 : 和 /） */
static int is_option(const char *s)
{
    const char *p = s;

    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')
        p++;
    return p > s && *p == '=';
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *in_url, *out_url = "pipe:1";
    AVIOContext *in = NULL, *out = NULL;
    AVDictionary *options = NULL;
    int64_t skip = 0, count = -1, total = 0;
    int bufsize = 32768, info = 0, first_extra = 2;
    uint8_t *buf = NULL;
    int ret, exit_code = 0;
    double t0;

    if (argc == 2 && !strcmp(argv[1], "-p")) {
        list_protocols();
        return 0;
    }
    if (argc < 2 || argv[1][0] == '-') {
        fprintf(stderr, "用法：%s <输入URL> [输出URL] [skip=N] [count=N] [bufsize=N] [info=0|1] [名字=值 ...]\n"
                        "      %s -p\n", argv[0], argv[0]);
        return 1;
    }

    in_url = argv[1];
    /* 第二个参数不是“名字=值”就是输出 URL */
    if (argc > 2 && !is_option(argv[2])) {
        out_url = argv[2];
        first_extra = 3;
    }

    for (int i = first_extra; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *name, *val;

        if (!is_option(argv[i])) {
            fprintf(stderr, "参数要写成 名字=值：%s\n", argv[i]);
            return 1;
        }
        name = av_strndup(argv[i], eq - argv[i]);
        val  = eq + 1;
        if (!strcmp(name, "skip"))
            skip = strtoll(val, NULL, 0);
        else if (!strcmp(name, "count"))
            count = strtoll(val, NULL, 0);
        else if (!strcmp(name, "bufsize"))
            bufsize = atoi(val);
        else if (!strcmp(name, "info"))
            info = atoi(val);
        else
            av_dict_set(&options, name, val, 0);
        av_free(name);
    }
    if (bufsize <= 0 || skip < 0) {
        fprintf(stderr, "bufsize 要大于 0，skip 不能是负数\n");
        return 1;
    }

    avformat_network_init();
    buf = av_malloc(bufsize);
    if (!buf) {
        fprintf(stderr, "内存不够\n");
        return 1;
    }

    /*
     * 打开输入。第 3 个参数是中断回调（这里不用），第 4 个是选项：avio_open2() 用掉的选项会从字典里删掉，
     * 剩下的是它不认识的。HTTP 出错时返回的是 AVERROR_HTTP_NOT_FOUND 这一类的错误码，av_err2str() 能打印成
     * "Server returned 404 Not Found"。
     */
    ret = avio_open2(&in, in_url, AVIO_FLAG_READ, NULL, &options);
    if (ret < 0) {
        fprintf(stderr, "打不开输入 %s：%s\n", in_url, av_err2str(ret));
        exit_code = 2;
        goto end;
    }
    {
        const AVDictionaryEntry *e = NULL;
        while ((e = av_dict_iterate(options, e)))
            fprintf(stderr, "警告：选项 %s 没有被使用\n", e->key);
    }
    if (info)
        print_info(in, in_url);

    if (skip) {
        /* 文件可以 seek；HTTP 要服务器支持 Range 才行；管道、不支持 Range 的 HTTP 会失败（ESPIPE） */
        int64_t pos = avio_seek(in, skip, SEEK_SET);
        if (pos < 0) {
            fprintf(stderr, "seek 到 %" PRId64 " 失败：%s\n", skip, av_err2str((int)pos));
            exit_code = 3;
            goto end;
        }
    }

    ret = avio_open2(&out, out_url, AVIO_FLAG_WRITE, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "打不开输出 %s：%s\n", out_url, av_err2str(ret));
        exit_code = 5;
        goto end;
    }

    t0 = now();
    while (count < 0 || total < count) {
        int want = bufsize, n;

        if (count >= 0 && count - total < want)
            want = (int)(count - total);
        n = avio_read(in, buf, want);
        if (n == AVERROR_EOF)           /* 读到末尾 */
            break;
        if (n < 0) {
            fprintf(stderr, "读出错：%s\n", av_err2str(n));
            exit_code = 4;
            break;
        }
        avio_write(out, buf, n);
        if (out->error) {               /* avio_write() 没有返回值，错误在 AVIOContext 的 error 字段里 */
            fprintf(stderr, "写出错：%s\n", av_err2str(out->error));
            exit_code = 5;
            break;
        }
        total += n;
    }
    avio_flush(out);
    if (!exit_code && out->error) {
        fprintf(stderr, "写出错：%s\n", av_err2str(out->error));
        exit_code = 5;
    }
    if (info)
        fprintf(stderr, "复制了 %" PRId64 " 字节，用时 %.3f 秒\n", total, now() - t0);

end:
    /* 输出要 avio_closep()：关闭时才会把缓冲区里剩下的数据写出去，它的返回值也是写出的结果 */
    if (out) {
        ret = avio_closep(&out);
        if (ret < 0 && !exit_code) {
            fprintf(stderr, "关闭输出时出错：%s\n", av_err2str(ret));
            exit_code = 5;
        }
    }
    avio_closep(&in);
    av_freep(&buf);
    av_dict_free(&options);
    avformat_network_deinit();
    return exit_code;
}
