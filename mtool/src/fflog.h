/*
 * L22 工程化：自己的日志回调。可以直接拷进你的项目，只有这一个头文件，不依赖课程里的其他代码。
 *
 * FFmpeg 的所有库通过 av_log() 输出日志，默认写到 stderr。程序可以用 av_log_set_callback() 换成自己的函数。
 * 这个头文件做了三件事：
 *   1. 屏幕上只显示 level 以上的重要日志（和 av_log_set_level() 的作用一样，但由自己的函数判断）；
 *   2. 同时把 keep_level 以上的日志（通常比屏幕上显示的更详细，例如 debug）记进一个环形缓冲区，只保留最近的 N 行；
 *      平时不显示，出错的时候用 fflog_dump_recent() 打印出来，当作“出错之前库在做什么”的上下文；
 *   3. 统计警告和错误的行数，程序结束时可以一并报告。
 * 一次 av_log() 调用不一定是完整的一行（没有换行符的调用会被后面的调用接上），统计和缓冲区都按“行”算。
 * 多个线程同时调用 av_log() 是常态（帧线程、HTTP 服务器的每个连接），所以回调里有锁。
 *
 * 必须知道的几件事（课程里都验证过，见 README）：
 *   - 自己的回调会收到所有级别的日志，不管 av_log_get_level() 是多少；默认回调自己做的级别判断，换了回调就要自己做。
 *     所以 keep_level 比 av_log_get_level() 更详细也能记到，不过有的库在拼日志之前会先看一眼 av_log_get_level()，
 *     想要完整的 debug 日志，同时要 av_log_set_level(AV_LOG_DEBUG)，本头文件的 fflog_init() 替你做了。
 *   - av_log_format_line2() 帮你把“[类名 @ 地址] 内容”这样的前缀拼好；它用 print_prefix 记住上一次的内容有没有以换行结尾，
 *     这样一次调用没写完的行，下一次调用接着写时不会重复打印前缀。这个状态要放在回调外面，而且是所有线程共用的，所以要加锁。
 *   - 回调里不能再调用 av_log()，会递归。
 *   - av_log_set_callback() 本身不是线程安全的，要在启动其他线程之前调用。
 *   - 默认回调的 AV_LOG_SKIP_REPEATED（合并重复的行）、彩色输出、写 stderr 以外的东西，自己的回调都没有，需要的话自己加。
 */
#ifndef FFLOG_H
#define FFLOG_H

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavutil/log.h>

typedef struct FFLogOptions {
    int   level;        /* 屏幕上显示的最低级别，例如 AV_LOG_WARNING；比它更详细的不显示 */
    int   keep_level;   /* 记进环形缓冲区的最低级别，例如 AV_LOG_DEBUG；小于 level 时按 level */
    int   keep_lines;   /* 环形缓冲区的行数，<= 0 用 32 */
    int   timestamps;   /* 1：每条前面加程序启动以来的毫秒数 */
    FILE *out;          /* 屏幕输出，NULL 用 stderr */
} FFLogOptions;

#define FFLOG_LINE_MAX 1024

/* 编译时加 -DFFLOG_NO_LOCK 去掉回调里的锁，用来亲眼看看多线程下不加锁的后果（README 里有练习）。真正的程序不要这样做。 */
#ifdef FFLOG_NO_LOCK
#define FFLOG_LOCK(s)   ((void)0)
#define FFLOG_UNLOCK(s) ((void)0)
#else
#define FFLOG_LOCK(s)   pthread_mutex_lock(&(s)->lock)
#define FFLOG_UNLOCK(s) pthread_mutex_unlock(&(s)->lock)
#endif

typedef struct FFLogState {
    pthread_mutex_t lock;
    int      installed;
    int      level, keep_level, timestamps;
    FILE    *out;
    char   (*ring)[FFLOG_LINE_MAX];     /* keep_lines 行，每行最多 FFLOG_LINE_MAX - 1 个字节 */
    int     *ring_level;
    int      ring_size, ring_next, ring_used;
    int      print_prefix;              /* av_log_format_line2() 用来记“上一条有没有以换行结尾” */
    long     counts[8];                 /* 按 level 分档：0 quiet/panic/fatal，1 error，2 warning，3 info，4 verbose，5 debug，6 trace */
    long     total;
    struct timespec t0;
} FFLogState;

static FFLogState fflog_state = { .lock = PTHREAD_MUTEX_INITIALIZER };

static const char *fflog_level_name(int level)
{
    if (level <= AV_LOG_FATAL)   return "fatal";
    if (level <= AV_LOG_ERROR)   return "error";
    if (level <= AV_LOG_WARNING) return "warning";
    if (level <= AV_LOG_INFO)    return "info";
    if (level <= AV_LOG_VERBOSE) return "verbose";
    if (level <= AV_LOG_DEBUG)   return "debug";
    return "trace";
}

static int fflog_bucket(int level)
{
    if (level <= AV_LOG_FATAL)   return 0;
    if (level <= AV_LOG_ERROR)   return 1;
    if (level <= AV_LOG_WARNING) return 2;
    if (level <= AV_LOG_INFO)    return 3;
    if (level <= AV_LOG_VERBOSE) return 4;
    if (level <= AV_LOG_DEBUG)   return 5;
    return 6;
}

/* 回调本身。在任何线程里都可能被调用。 */
static void fflog_callback(void *avcl, int level, const char *fmt, va_list vl)
{
    FFLogState *s = &fflog_state;
    char line[FFLOG_LINE_MAX];
    int lv = level >= 0 ? (level & 0xff) : level;    /* 低 8 位是级别，高位是 AV_LOG_SKIP_REPEATED 之类的标志 */
    int at_line_start;

    if (lv > s->level && lv > s->keep_level)
        return;     /* 两边都不要，连格式化都省了 */

    FFLOG_LOCK(s);
    at_line_start = s->print_prefix;        /* 这一次调用是不是一行的开头 */
    av_log_format_line2(avcl, level, fmt, vl, line, sizeof(line), &s->print_prefix);
    if (at_line_start) {
        s->total++;                         /* 按“行”统计：接着上一次写的半行不另算 */
        s->counts[fflog_bucket(lv)]++;
    }

    if (lv <= s->keep_level && s->ring) {
        if (!at_line_start && s->ring_used > 0) {
            /* 接着上一次没写完的行：拼到上一条后面（放不下的部分丢掉），缓冲区里的每一条就是完整的一行 */
            char *last = s->ring[(s->ring_next - 1 + s->ring_size) % s->ring_size];

            strncat(last, line, FFLOG_LINE_MAX - strlen(last) - 1);
        } else {
            snprintf(s->ring[s->ring_next], FFLOG_LINE_MAX, "%s", line);
            s->ring_level[s->ring_next] = lv;
            s->ring_next = (s->ring_next + 1) % s->ring_size;
            if (s->ring_used < s->ring_size)
                s->ring_used++;
        }
    }
    if (lv <= s->level) {
        if (s->timestamps && at_line_start) {
            struct timespec t;
            long ms;

            clock_gettime(CLOCK_MONOTONIC, &t);
            ms = (t.tv_sec - s->t0.tv_sec) * 1000 + (t.tv_nsec - s->t0.tv_nsec) / 1000000;
            fprintf(s->out, "%6ld ms ", ms);
        }
        fputs(line, s->out);
    }
    FFLOG_UNLOCK(s);
}

/* 安装回调。成功返回 0。要在启动其他线程之前调用。 */
static int fflog_init(const FFLogOptions *opt)
{
    FFLogState *s = &fflog_state;
    int lines = opt->keep_lines > 0 ? opt->keep_lines : 32;

    s->level      = opt->level;
    s->keep_level = opt->keep_level > opt->level ? opt->keep_level : opt->level;
    s->timestamps = opt->timestamps;
    s->out        = opt->out ? opt->out : stderr;
    s->ring       = calloc(lines, sizeof(*s->ring));
    s->ring_level = calloc(lines, sizeof(*s->ring_level));
    if (!s->ring || !s->ring_level) {
        free(s->ring);
        free(s->ring_level);
        s->ring = NULL;
        s->ring_level = NULL;
        return -1;
    }
    s->ring_size    = lines;
    s->ring_next    = s->ring_used = 0;
    s->print_prefix = 1;
    memset(s->counts, 0, sizeof(s->counts));
    s->total = 0;
    clock_gettime(CLOCK_MONOTONIC, &s->t0);

    /* 库里有些日志在拼之前先看一眼全局级别，所以全局级别要放到最详细的那个。 */
    av_log_set_level(s->keep_level);
    av_log_set_callback(fflog_callback);
    s->installed = 1;
    return 0;
}

/* 恢复默认回调，释放缓冲区。之后不能再有线程在调用 av_log()。 */
static void fflog_close(void)
{
    FFLogState *s = &fflog_state;

    if (!s->installed)
        return;
    av_log_set_callback(av_log_default_callback);
    pthread_mutex_lock(&s->lock);
    free(s->ring);
    free(s->ring_level);
    s->ring = NULL;
    s->ring_level = NULL;
    s->ring_size = s->ring_used = s->ring_next = 0;
    s->installed = 0;
    pthread_mutex_unlock(&s->lock);
}

/* 把最近记下的日志按时间顺序打印出来，每条前面标上级别。 */
static void fflog_dump_recent(FILE *f, const char *title)
{
    FFLogState *s = &fflog_state;

    pthread_mutex_lock(&s->lock);
    fprintf(f, "---- %s（最近 %d 行，缓冲区 %d 行）----\n", title, s->ring_used, s->ring_size);
    for (int i = 0; i < s->ring_used; i++) {
        const int idx = (s->ring_next - s->ring_used + i + s->ring_size) % s->ring_size;

        fprintf(f, "  [%-7s] %s", fflog_level_name(s->ring_level[idx]), s->ring[idx]);
        if (!strchr(s->ring[idx], '\n'))
            fputc('\n', f);     /* 一次调用没写完一行的情况 */
    }
    fprintf(f, "----\n");
    pthread_mutex_unlock(&s->lock);
}

/* 到目前为止的行数（所有级别、错误、警告）。传 NULL 的不取。 */
static void fflog_counts(long *total, long *errors, long *warnings)
{
    FFLogState *s = &fflog_state;

    pthread_mutex_lock(&s->lock);
    if (total)    *total    = s->total;
    if (errors)   *errors   = s->counts[0] + s->counts[1];
    if (warnings) *warnings = s->counts[2];
    pthread_mutex_unlock(&s->lock);
}

#endif /* FFLOG_H */
