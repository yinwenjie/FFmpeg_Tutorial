/*
 * L22 工程化：FFmpeg 的错误码。
 *
 * 用法：
 *   err_codes                打印常用错误码的表：名字、数值、av_strerror() 给的原文、遇到了该怎么办
 *   err_codes <数字>...      解释任意数字（十进制，或者 0x 开头的十六进制，可以带负号），例如 err_codes -541478725 -2 0xdfb9b0bb
 *   err_codes demo           演示 av_strerror() 的三个容易踩的地方：缓冲区太小、不认识的码、退出码被截成 8 位
 *
 * 错误码的约定：
 *   - 返回值 >= 0 是成功（有的函数用它表示数量、字节数），< 0 是错误；
 *   - 系统的错误（errno）转成 AVERROR(e)，也就是 -e，数值取决于平台（Linux 上 EAGAIN 是 11，macOS 上是 35）；
 *   - FFmpeg 自己定义的错误是 FFERRTAG(a, b, c, d)，把四个字符拼成一个整数取负，所以从数值里能“读”出名字：
 *     AVERROR_EOF 是 -0x20464f45，反过来 0x45 0x4f 0x46 0x20 就是 "EOF "。
 *   - 少数（AVERROR_EXPERIMENTAL、AVERROR_INPUT_CHANGED）是固定的魔数，不是四个字符。
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavutil/error.h>

typedef struct Entry {
    const char *name;
    int         code;
    const char *advice;     /* 遇到了该怎么办 */
} Entry;

#define E(c, advice) { #c, c, advice }

static const Entry table[] = {
    E(AVERROR_EOF,               "输入结束了，不算出错：读完了就结束循环，不要当失败处理"),
    E(AVERROR(EAGAIN),           "送包/取帧的接口：现在不能做这一步，先做另一步（send 返回它就先 receive，receive 返回它就再 send）"),
    E(AVERROR(EINVAL),           "参数不对：多半是自己传错了，或者参数之间不兼容（例如编码器要求的格式没设）"),
    E(AVERROR(ENOMEM),           "内存不够：不要继续，释放已有的东西退出"),
    E(AVERROR(ENOENT),           "文件或目录不存在"),
    E(AVERROR(EACCES),           "没有权限"),
    E(AVERROR(EIO),              "读写出错：磁盘、网络的底层错误"),
    E(AVERROR(EPIPE),            "管道或网络的另一头关了"),
    E(AVERROR(ETIMEDOUT),        "超时"),
    E(AVERROR(ECONNREFUSED),     "连接被拒绝：服务没开，或者端口不对"),
    E(AVERROR(ENOSYS),           "没实现：这个协议、这个操作不支持（例如 HTTP 不能列目录）"),
    E(AVERROR(ENOSPC),           "磁盘满了：写文件尾和关文件时最容易遇到，返回值要检查"),
    E(AVERROR_INVALIDDATA,       "数据坏了，或者根本不是这种格式：打不开文件时最常见；解码时可以丢掉这个包继续"),
    E(AVERROR_STREAM_NOT_FOUND,  "找不到流"),
    E(AVERROR_DECODER_NOT_FOUND, "没有这个解码器：编译 FFmpeg 时没带，或者名字写错了"),
    E(AVERROR_ENCODER_NOT_FOUND, "没有这个编码器：libx264 之类的外部编码器要在 configure 时 --enable"),
    E(AVERROR_DEMUXER_NOT_FOUND, "没有这个解复用器"),
    E(AVERROR_MUXER_NOT_FOUND,   "没有这个复用器，或者猜不出输出格式：换一个文件扩展名"),
    E(AVERROR_PROTOCOL_NOT_FOUND, "没有这个协议：https:// 要 configure 带 TLS 库"),
    E(AVERROR_FILTER_NOT_FOUND,  "没有这个滤镜"),
    E(AVERROR_BSF_NOT_FOUND,     "没有这个码流过滤器"),
    E(AVERROR_OPTION_NOT_FOUND,  "选项名不对：av_opt_set() 和选项字典最后一定要检查有没有剩下没被用到的"),
    E(AVERROR_BUFFER_TOO_SMALL,  "你给的缓冲区太小：加大再试"),
    E(AVERROR_INPUT_CHANGED,     "输入的参数变了，需要重新配置：滤镜图要重建（reconfig_graph.c）"),
    E(AVERROR_OUTPUT_CHANGED,    "输出的参数变了，需要重新配置"),
    E(AVERROR_EXIT,              "要求立即退出（例如中断回调返回了非 0）：不要重试"),
    E(AVERROR_EXTERNAL,          "外部库（libx264 之类）出错：详细原因在日志里，返回值里没有"),
    E(AVERROR_UNKNOWN,           "不知道是什么错，多半来自外部库：看日志"),
    E(AVERROR_EXPERIMENTAL,      "实验性功能：要明确设置 strict=experimental 才能用"),
    E(AVERROR_PATCHWELCOME,      "还没实现，欢迎提交补丁"),
    E(AVERROR_BUG,               "FFmpeg 自己的 bug：把日志和复现办法报告给社区"),
    E(AVERROR_BUG2,              "同上（旧的写法）"),
    E(AVERROR_HTTP_BAD_REQUEST,  "HTTP 400"),
    E(AVERROR_HTTP_UNAUTHORIZED, "HTTP 401：要认证"),
    E(AVERROR_HTTP_FORBIDDEN,    "HTTP 403：不让访问"),
    E(AVERROR_HTTP_NOT_FOUND,    "HTTP 404：地址不对"),
    E(AVERROR_HTTP_TOO_MANY_REQUESTS, "HTTP 429：请求太频繁，过一会再试"),
    E(AVERROR_HTTP_OTHER_4XX,    "HTTP 的其他 4xx 错误：具体是哪个在日志里"),
    E(AVERROR_HTTP_SERVER_ERROR, "HTTP 的 5xx 错误：服务器出错，可以稍后重试"),
};

/* 如果 -code 是四个字符拼出来的（FFERRTAG），把它们写成可读的样子。 */
static const char *tag_text(int code, char *buf, size_t size)
{
    const uint32_t v = (uint32_t)(-(int64_t)code);

    if (code >= 0 || v == 0)
        return "";
    buf[0] = 0;
    for (int i = 0; i < 4; i++) {
        const unsigned char ch = (v >> (8 * i)) & 0xff;
        size_t n = strlen(buf);

        if (ch >= 0x20 && ch < 0x7f)
            snprintf(buf + n, size - n, "%c", ch);
        else
            snprintf(buf + n, size - n, "\\x%02x", ch);
    }
    return buf;
}

static int is_tag(int code)
{
    const uint32_t v = (uint32_t)(-(int64_t)code);

    if (code >= 0)
        return 0;
    /* 至少要有三个可见字符（0xF8 开头的那一批第一个字节不可见） */
    int visible = 0;

    for (int i = 0; i < 4; i++) {
        const unsigned char ch = (v >> (8 * i)) & 0xff;

        visible += ch >= 0x20 && ch < 0x7f;
    }
    return visible >= 3;
}

static void explain(int code)
{
    char msg[AV_ERROR_MAX_STRING_SIZE], tag[32];
    const int known = av_strerror(code, msg, sizeof(msg)) == 0;

    printf("%d（0x%08x）\n", code, (unsigned)code);
    printf("  av_strerror：%s%s\n", msg, known ? "" : "  ← 返回负数：不认识这个码，上面只是“Error number … occurred”");
    if (code < 0 && code > -256)
        printf("  看起来是 AVERROR(%d)，也就是 errno %d：%s\n", -code, -code, strerror(-code));
    if (is_tag(code))
        printf("  四个字符拼出来的码：\"%s\"\n", tag_text(code, tag, sizeof(tag)));
    printf("  如果把它当成 main() 的返回值，进程的退出码是 %d（只取低 8 位）\n", code & 0xff);
}

static void list_table(void)
{
    char msg[AV_ERROR_MAX_STRING_SIZE], tag[32];

    printf("%-34s %12s  %-8s  %s\n", "名字", "数值", "四个字符", "av_strerror() 的原文");
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        const Entry *e = &table[i];

        av_strerror(e->code, msg, sizeof(msg));
        printf("%-34s %12d  %-8s  %s\n", e->name, e->code, is_tag(e->code) ? tag_text(e->code, tag, sizeof(tag)) : "", msg);
        printf("%-34s %12s  %-8s  → %s\n", "", "", "", e->advice);
    }
}

static void demo(void)
{
    char small[8], big[AV_ERROR_MAX_STRING_SIZE];
    int ret;

    printf("1. 缓冲区太小：av_strerror() 把原文截断，仍然以 '\\0' 结尾，返回值照样是 0\n");
    ret = av_strerror(AVERROR_EOF, small, sizeof(small));
    printf("   缓冲区 %zu 字节：返回 %d，得到 \"%s\"\n", sizeof(small), ret, small);
    ret = av_strerror(AVERROR_INVALIDDATA, big, sizeof(big));
    printf("   缓冲区 %d 字节：返回 %d，得到 \"%s\"（%zu 个字符，够用）\n", AV_ERROR_MAX_STRING_SIZE, ret, big, strlen(big));
    printf("   AV_ERROR_MAX_STRING_SIZE 是 %d：库里最长的一条原文放得下\n\n", AV_ERROR_MAX_STRING_SIZE);

    printf("2. 不认识的码：返回负数，缓冲区里是 \"Error number %%d occurred\"\n");
    ret = av_strerror(-12345678, big, sizeof(big));
    printf("   av_strerror(-12345678)：返回 %d，得到 \"%s\"\n", ret, big);
    ret = av_strerror(1, big, sizeof(big));
    printf("   av_strerror(1)：返回 %d，得到 \"%s\"（正数不是错误码，但它也不会崩）\n\n", ret, big);

    printf("3. av_err2str(e) 是个宏，展开成 av_make_error_string((char[64]){0}, 64, e)，缓冲区是一个复合字面量：\n");
    printf("   在 C 里可以直接放在 printf 的参数里：%s\n", av_err2str(AVERROR(ENOENT)));
    printf("   在 C++ 里：g++ 13 编译不过（taking address of temporary array），clang++ 18 当作扩展接受；要写可移植的代码就自己准备缓冲区，README 里有写法。\n");
    printf("   它的有效期到所在的语句结束，不能存起来以后再用：\n");
    printf("     const char *s = av_err2str(e);   // s 马上悬空\n\n");

    printf("4. main() 返回负的错误码：进程的退出码只取低 8 位\n");
    printf("   return AVERROR(ENOENT)  → 退出码 %d\n", AVERROR(ENOENT) & 0xff);
    printf("   return AVERROR_EXIT     → 退出码 %d\n", AVERROR_EXIT & 0xff);
    printf("   return AVERROR_EOF      → 退出码 %d\n", AVERROR_EOF & 0xff);
    printf("   return AVERROR_EXTERNAL → 退出码 %d\n", AVERROR_EXTERNAL & 0xff);
    printf("   这些数字脚本里没法用，而且互相撞车：EOF、EXIT、EXTERNAL 都以 'E' 开头，退出码都是 %d，\n", AVERROR_EOF & 0xff);
    printf("   “读完了”和“出错了”分不开（官方示例 transcode_aac.c 就这样，见 patch 0095）。\n");
    printf("   约定：成功 0，一般错误 1，用法错误 2；要区分原因就自己定一张表，并写在用法里。\n");
}

int main(int argc, char **argv)
{
    if (argc == 1) {
        list_table();
        return 0;
    }
    if (!strcmp(argv[1], "demo")) {
        demo();
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        char *end;
        long long v = strtoll(argv[i], &end, 0);

        if (*end || end == argv[i]) {
            fprintf(stderr, "不是数字：%s\n", argv[i]);
            return 1;
        }
        /* 十六进制按 32 位无符号数写（0xdfb9b0bb），转成有符号 */
        if (v > INT32_MAX && v <= UINT32_MAX)
            v = (int32_t)(uint32_t)v;
        if (v < INT32_MIN || v > INT32_MAX) {
            fprintf(stderr, "超出 32 位整数的范围：%s\n", argv[i]);
            return 1;
        }
        explain((int)v);
    }
    return 0;
}
