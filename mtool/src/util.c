/*
 * mtool 的公共代码：错误信息、打开输入、时间解析、进度、结构化输出、日志、版本检查。
 * 每一块都是前面课程里讲过的，注释里写了出处。
 */
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "fflog.h"      /* L22 的日志回调，原样拷进来的 */
#include "mtool.h"

/* ================================ 错误和提示 ================================ */

static void vprint_msg(const char *prefix, int averr, const char *fmt, va_list ap)
{
    char msg[512], err[AV_ERROR_MAX_STRING_SIZE];

    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (averr < 0) {
        const char *hint = mt_errhint(averr);

        av_strerror(averr, err, sizeof(err));
        fprintf(stderr, "mtool：%s：%s：%s%s%s%s\n", prefix, msg, err,
                hint ? "（" : "", hint ? hint : "", hint ? "）" : "");
    } else {
        fprintf(stderr, "mtool：%s：%s\n", prefix, msg);
    }
}

void mt_err(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprint_msg("错误", 0, fmt, ap);
    va_end(ap);
}

void mt_averr(int averr, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprint_msg("错误", averr, fmt, ap);
    va_end(ap);
}

void mt_warn(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprint_msg("警告", 0, fmt, ap);
    va_end(ap);
}

int mt_usage_err(const char *cmd, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprint_msg("用法错误", 0, fmt, ap);
    va_end(ap);
    fprintf(stderr, "试试 mtool %s --help\n", cmd);
    return MT_USAGE;
}

/* 把最常见的几种失败翻译成一句人话。返回 NULL 表示没有更多可说的。 */
const char *mt_errhint(int averr)
{
    if (averr == AVERROR_INVALIDDATA)        return "文件损坏，或者不是媒体文件";
    if (averr == AVERROR(ENOENT))            return "文件或目录不存在";
    if (averr == AVERROR(EACCES))            return "没有权限";
    if (averr == AVERROR(EISDIR))            return "这是一个目录，不是文件";
    if (averr == AVERROR(ENOSPC))            return "磁盘空间不够";
    if (averr == AVERROR(EPIPE))             return "对方关闭了连接";
    if (averr == AVERROR(ECONNREFUSED))      return "连不上，对方没有在监听";
    if (averr == AVERROR(ETIMEDOUT))         return "超时";
    if (averr == AVERROR_PROTOCOL_NOT_FOUND) return "这份 FFmpeg 没有编进这个协议";
    if (averr == AVERROR_ENCODER_NOT_FOUND)  return "这份 FFmpeg 没有这个编码器";
    if (averr == AVERROR_DECODER_NOT_FOUND)  return "这份 FFmpeg 没有这个解码器";
    if (averr == AVERROR_MUXER_NOT_FOUND)    return "不认识这个输出格式";
    if (averr == AVERROR_EOF)                return "文件提前结束";
    return NULL;
}

/* ================================ 打开输入 ================================ */

int mt_open_input(AVFormatContext **ctx, const char *path)
{
    int ret = avformat_open_input(ctx, path, NULL, NULL);

    if (ret < 0) {
        mt_averr(ret, "打不开 %s", path);
        return ret;
    }
    ret = avformat_find_stream_info(*ctx, NULL);
    if (ret < 0) {
        mt_averr(ret, "读不出 %s 的流信息", path);
        avformat_close_input(ctx);
        return ret;
    }
    if ((*ctx)->nb_streams == 0) {
        mt_err("%s 里没有找到任何音视频流", path);
        avformat_close_input(ctx);
        return AVERROR_INVALIDDATA;
    }
    return 0;
}

/* ================================ 时间 ================================ */

int mt_parse_time(const char *s, double *seconds)
{
    double v = 0;
    int parts = 0;

    if (!s || !*s)
        return -1;
    while (*s) {
        char *end;
        double x = strtod(s, &end);

        if (end == s || x < 0 || !isfinite(x))
            return -1;
        v = v * 60 + x;
        parts++;
        if (*end == ':') {
            if (parts >= 3)
                return -1;
            s = end + 1;
        } else if (*end == '\0') {
            break;
        } else {
            return -1;
        }
    }
    *seconds = v;
    return 0;
}

/* ================================ 进度 ================================ */

void mt_progress_start(Progress *p, const char *label, double total_seconds)
{
    memset(p, 0, sizeof(*p));
    p->label = label;
    p->total = total_seconds;
    p->t0    = p->last = av_gettime_relative();
    p->tty   = isatty(fileno(stderr));
}

static void progress_line(const Progress *p, double done, int64_t frames, int final)
{
    const double elapsed = (av_gettime_relative() - p->t0) / 1e6;
    const double speed   = elapsed > 0 ? done / elapsed : 0;
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "%s ", p->label);

    if (p->total > 0)
        n += snprintf(buf + n, sizeof(buf) - n, "%5.1f%%", FFMIN(done / p->total, 1.0) * 100);
    n += snprintf(buf + n, sizeof(buf) - n, "  已处理 %.1f 秒", done);
    if (frames > 0)
        n += snprintf(buf + n, sizeof(buf) - n, "，%" PRId64 " 帧", frames);
    n += snprintf(buf + n, sizeof(buf) - n, "，速度 %.2fx", speed);
    if (!final && p->total > 0 && speed > 0 && done < p->total)
        snprintf(buf + n, sizeof(buf) - n, "，剩余约 %.0f 秒", (p->total - done) / speed);
    if (p->tty && !final)
        fprintf(stderr, "\r%-78s", buf);
    else
        fprintf(stderr, "%s%s\n", p->tty && p->shown ? "\r" : "", buf);
}

void mt_progress_update(Progress *p, double done, int64_t frames)
{
    const int64_t now = av_gettime_relative();

    if (!p->tty || now - p->last < 500000)      /* 不是终端就不刷新，免得日志里全是进度 */
        return;
    p->last  = now;
    p->shown = 1;
    progress_line(p, done, frames, 0);
}

void mt_progress_end(Progress *p, double done, int64_t frames)
{
    progress_line(p, done, frames, 1);
}

/* ================================ 结构化输出 ================================ */

void out_init(Out *o, FILE *f, int json)
{
    memset(o, 0, sizeof(*o));
    o->f     = f;
    o->json  = json;
    o->first[0] = 1;
}

static void out_indent(const Out *o)
{
    for (int i = 0; i < o->depth; i++)
        fputs("  ", o->f);
}

static void json_string(FILE *f, const char *s);

/* 写一项之前：JSON 要补逗号和换行，文本只缩进。key 为 NULL 时（数组里的元素）文本模式用 名字[序号]。 */
static void out_item_begin(Out *o, const char *key, const char *text_fallback, int scalar)
{
    if (o->json) {
        if (!o->first[o->depth])
            fputc(',', o->f);
        if (o->depth > 0 || !o->first[0])
            fputc('\n', o->f);
        out_indent(o);
        if (key) {
            json_string(o->f, key);
            fputs(": ", o->f);
        }
    } else {
        out_indent(o);
        if (key || text_fallback)
            fprintf(o->f, "%s:%s", key ? key : text_fallback, scalar ? " " : "");
    }
    o->first[o->depth] = 0;
}

static void out_push(Out *o, int is_array, const char *name)
{
    o->depth++;
    o->first[o->depth]    = 1;
    o->is_array[o->depth] = is_array;
    o->arr_idx[o->depth]  = 0;
    o->arr_name[o->depth] = name;
}

void out_obj_begin(Out *o, const char *key)
{
    char label[96] = "";

    if (!key && o->depth > 0 && o->is_array[o->depth])
        snprintf(label, sizeof(label), "%s[%d]", o->arr_name[o->depth], o->arr_idx[o->depth]++);
    if (o->json) {
        out_item_begin(o, o->is_array[o->depth] ? NULL : key, NULL, 0);
        fputc('{', o->f);
    } else {
        out_item_begin(o, key, label[0] ? label : NULL, 0);
        if (key || label[0])
            fputc('\n', o->f);
    }
    out_push(o, 0, key);
}

void out_obj_end(Out *o)
{
    const int empty = o->first[o->depth];

    o->depth--;
    if (o->json) {
        if (!empty) {
            fputc('\n', o->f);
            out_indent(o);
        }
        fputc('}', o->f);
        if (o->depth == 0)
            fputc('\n', o->f);
    }
}

void out_arr_begin(Out *o, const char *key)
{
    out_item_begin(o, key, NULL, 0);
    if (o->json)
        fputc('[', o->f);
    else
        fputc('\n', o->f);
    out_push(o, 1, key);
}

void out_arr_end(Out *o)
{
    const int empty = o->first[o->depth];

    o->depth--;
    if (o->json) {
        if (!empty) {
            fputc('\n', o->f);
            out_indent(o);
        }
        fputc(']', o->f);
    }
}

/* JSON 字符串：转义引号、反斜杠和控制字符；不是合法 UTF-8 的字节换成 ?（元数据里什么都可能有）。 */
static void json_string(FILE *f, const char *s)
{
    const uint8_t *p = (const uint8_t *)s, *end = p + strlen(s);

    fputc('"', f);
    while (p < end) {
        const uint8_t *q = p;
        uint32_t c;

        if (*p < 0x80) {
            c = *p++;
            if (c == '"' || c == '\\')       fprintf(f, "\\%c", c);
            else if (c == '\n')               fputs("\\n", f);
            else if (c == '\t')               fputs("\\t", f);
            else if (c < 0x20)                fprintf(f, "\\u%04x", c);
            else                              fputc(c, f);
            continue;
        }
        if (av_utf8_decode((int32_t *)&c, &q, end, 0) < 0 || q == p) {
            fputc('?', f);
            p++;
        } else {
            fwrite(p, 1, q - p, f);
            p = q;
        }
    }
    fputc('"', f);
}

static void out_scalar_begin(Out *o, const char *key)
{
    out_item_begin(o, o->is_array[o->depth] ? NULL : key, NULL, 1);
}

void out_str(Out *o, const char *key, const char *val)
{
    out_scalar_begin(o, key);
    if (o->json)
        json_string(o->f, val ? val : "");
    else
        fputs(val ? val : "", o->f);
    if (!o->json)
        fputc('\n', o->f);
}

void out_int(Out *o, const char *key, int64_t val)
{
    out_scalar_begin(o, key);
    fprintf(o->f, "%" PRId64 "%s", val, o->json ? "" : "\n");
}

void out_dbl(Out *o, const char *key, double val, int prec)
{
    out_scalar_begin(o, key);
    if (o->json && !isfinite(val))
        fputs("null", o->f);            /* JSON 里没有 NaN 和无穷 */
    else
        fprintf(o->f, "%.*f", prec, val);
    if (!o->json)
        fputc('\n', o->f);
}

void out_bool(Out *o, const char *key, int val)
{
    out_scalar_begin(o, key);
    if (o->json)
        fputs(val ? "true" : "false", o->f);
    else
        fputs(val ? "是" : "否", o->f);
    if (!o->json)
        fputc('\n', o->f);
}

/* ================================ 日志 ================================ */

int mt_log_init(int level, int keep_log)
{
    FFLogOptions opt = {
        .level      = level,
        .keep_level = keep_log ? AV_LOG_DEBUG : level,
        .keep_lines = 40,
    };

    return fflog_init(&opt);
}

void mt_log_close(void)
{
    fflog_close();
}

void mt_log_dump_on_error(void)
{
    long total, errors, warnings;

    fflog_counts(&total, &errors, &warnings);
    fprintf(stderr, "库一共输出了 %ld 行日志，其中错误 %ld 行、警告 %ld 行。\n", total, errors, warnings);
    fflog_dump_recent(stderr, "出错之前库在做什么");
}

/* ================================ 版本 ================================ */

typedef struct LibVer {
    const char *name;
    unsigned    compiled, runtime;
} LibVer;

static int lib_versions(LibVer *v)
{
    int n = 0;

    v[n++] = (LibVer){ "libavutil",     LIBAVUTIL_VERSION_INT,     avutil_version() };
    v[n++] = (LibVer){ "libavcodec",    LIBAVCODEC_VERSION_INT,    avcodec_version() };
    v[n++] = (LibVer){ "libavformat",   LIBAVFORMAT_VERSION_INT,   avformat_version() };
    v[n++] = (LibVer){ "libavfilter",   LIBAVFILTER_VERSION_INT,   avfilter_version() };
    v[n++] = (LibVer){ "libswscale",    LIBSWSCALE_VERSION_INT,    swscale_version() };
    v[n++] = (LibVer){ "libswresample", LIBSWRESAMPLE_VERSION_INT, swresample_version() };
    return n;
}

/* 大版本必须相同；运行时的库不能比头文件旧（同一个大版本里，新的库兼容旧的头文件）。 */
int mt_check_versions(void)
{
    LibVer v[8];
    const int n = lib_versions(v);
    int bad = 0;

    for (int i = 0; i < n; i++) {
        if (AV_VERSION_MAJOR(v[i].compiled) != AV_VERSION_MAJOR(v[i].runtime) ||
            v[i].runtime < v[i].compiled) {
            mt_err("%s 的版本对不上：编译用的头文件是 %u.%u.%u，运行时加载的库是 %u.%u.%u。"
                   "检查 PKG_CONFIG_PATH 和 LD_LIBRARY_PATH 指向的是不是同一份 FFmpeg（L22 第 5 步）",
                   v[i].name, AV_VERSION_MAJOR(v[i].compiled), AV_VERSION_MINOR(v[i].compiled),
                   AV_VERSION_MICRO(v[i].compiled), AV_VERSION_MAJOR(v[i].runtime),
                   AV_VERSION_MINOR(v[i].runtime), AV_VERSION_MICRO(v[i].runtime));
            bad = 1;
        }
    }
    return bad ? -1 : 0;
}

void mt_print_versions(FILE *f)
{
    LibVer v[8];
    const int n = lib_versions(v);

    for (int i = 0; i < n; i++)
        fprintf(f, "%-14s %u.%u.%u（头文件 %u.%u.%u）\n", v[i].name,
                AV_VERSION_MAJOR(v[i].runtime), AV_VERSION_MINOR(v[i].runtime), AV_VERSION_MICRO(v[i].runtime),
                AV_VERSION_MAJOR(v[i].compiled), AV_VERSION_MINOR(v[i].compiled), AV_VERSION_MICRO(v[i].compiled));
    fprintf(f, "许可证：%s\n", avformat_license());
}
