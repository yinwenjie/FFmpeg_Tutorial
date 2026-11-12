/*
 * L22 工程化：演示 fflog.h 的日志回调。
 *
 * 用法：
 *   log_demo levels                       各个级别各打一条，看屏幕和“最近的日志”分别收到什么
 *   log_demo open <文件> [选项]           打开并解码一个文件；成功时屏幕上只有重要的日志，失败时把最近的日志都倒出来
 *   log_demo threads <线程数> <每线程行数>  多个线程同时写日志，核对行数和每一行的完整性
 *
 * 选项（名字=值）：
 *   level=warning   屏幕上显示的最低级别：quiet、error、warning、info、verbose、debug 之一
 *   keep=debug      记进缓冲区的最低级别
 *   lines=40        缓冲区的行数
 *   ts=1            屏幕上的每一条前面加毫秒数
 *
 * 退出码：0 成功，1 失败（用法错误、文件打不开、解码出错、线程测试发现问题）。
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>

#include "fflog.h"

static int parse_level(const char *s)
{
    static const struct { const char *name; int level; } t[] = {
        { "quiet", AV_LOG_QUIET }, { "fatal", AV_LOG_FATAL }, { "error", AV_LOG_ERROR },
        { "warning", AV_LOG_WARNING }, { "info", AV_LOG_INFO }, { "verbose", AV_LOG_VERBOSE },
        { "debug", AV_LOG_DEBUG }, { "trace", AV_LOG_TRACE },
    };

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if (!strcmp(s, t[i].name))
            return t[i].level;
    return -1000;
}

/* 解析 名字=值 的选项，填进 opt。返回 0 成功，-1 有不认识的。 */
static int parse_options(int argc, char **argv, int first, FFLogOptions *opt)
{
    for (int i = first; i < argc; i++) {
        const char *a = argv[i];

        if (!strncmp(a, "level=", 6))
            opt->level = parse_level(a + 6);
        else if (!strncmp(a, "keep=", 5))
            opt->keep_level = parse_level(a + 5);
        else if (!strncmp(a, "lines=", 6))
            opt->keep_lines = atoi(a + 6);
        else if (!strncmp(a, "ts=", 3))
            opt->timestamps = atoi(a + 3);
        else {
            fprintf(stderr, "不认识的选项 %s\n", a);
            return -1;
        }
        if (opt->level == -1000 || opt->keep_level == -1000) {
            fprintf(stderr, "不认识的级别：%s\n", a);
            return -1;
        }
    }
    return 0;
}

static int cmd_levels(const FFLogOptions *opt)
{
    static const struct { int level; const char *name; } t[] = {
        { AV_LOG_FATAL, "fatal" }, { AV_LOG_ERROR, "error" }, { AV_LOG_WARNING, "warning" },
        { AV_LOG_INFO, "info" }, { AV_LOG_VERBOSE, "verbose" }, { AV_LOG_DEBUG, "debug" },
        { AV_LOG_TRACE, "trace" },
    };
    long total, errors, warnings;

    printf("屏幕显示 %s 及更重要的，缓冲区记 %s 及更重要的\n", fflog_level_name(opt->level),
           fflog_level_name(opt->keep_level > opt->level ? opt->keep_level : opt->level));
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        av_log(NULL, t[i].level, "这是一条 %s（值 %d）\n", t[i].name, t[i].level);
    /* 一次调用不带换行，分两段写，再补上换行：属于同一行 */
    av_log(NULL, AV_LOG_WARNING, "这一行分两次写，");
    av_log(NULL, AV_LOG_WARNING, "第二次不应该再有前缀\n");
    fflog_counts(&total, &errors, &warnings);
    printf("共 %ld 行，其中错误 %ld 行，警告 %ld 行\n", total, errors, warnings);
    fflog_dump_recent(stdout, "缓冲区里的内容");
    return 0;
}

static int cmd_open(const char *path)
{
    AVFormatContext *fmt = NULL;
    AVCodecContext *dec = NULL;
    const AVCodec *codec = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int index, ret, nb_frames = 0;
    long total, errors, warnings;

    if (!pkt || !frame) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avformat_open_input(&fmt, path, NULL, NULL);
    if (ret < 0)
        goto end;
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0)
        goto end;
    index = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (index < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s 里没有视频流\n", path);
        ret = index;
        goto end;
    }
    dec = avcodec_alloc_context3(codec);
    if (!dec) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    ret = avcodec_parameters_to_context(dec, fmt->streams[index]->codecpar);
    if (ret >= 0)
        ret = avcodec_open2(dec, codec, NULL);
    while (ret >= 0 && nb_frames < 10 && (ret = av_read_frame(fmt, pkt)) >= 0) {
        if (pkt->stream_index == index) {
            ret = avcodec_send_packet(dec, pkt);
            while (ret >= 0 && (ret = avcodec_receive_frame(dec, frame)) >= 0) {
                nb_frames++;
                av_frame_unref(frame);
            }
            if (ret == AVERROR(EAGAIN))
                ret = 0;
        }
        av_packet_unref(pkt);
    }
    if (ret == AVERROR_EOF)
        ret = 0;

end:
    fflog_counts(&total, &errors, &warnings);
    if (ret < 0) {
        /* 出错：先说结论，再倒出最近的日志，作为出错之前库在做什么的线索。 */
        fprintf(stderr, "失败：%s\n", av_err2str(ret));
        fflog_dump_recent(stderr, "出错之前的日志");
    } else {
        printf("解出 %d 帧，共 %ld 行日志（错误 %ld，警告 %ld）\n", nb_frames, total, errors, warnings);
    }
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    return ret < 0 ? 1 : 0;
}

/* ---- 多线程 ---------------------------------------------------------------- */

typedef struct Job {
    int id, count;
} Job;

static void *worker(void *arg)
{
    const Job *j = arg;

    for (int i = 0; i < j->count; i++)
        av_log(NULL, AV_LOG_WARNING, "线程 %03d 第 %05d 行：%s\n", j->id, i, "这一行的内容必须完整，不能被别的线程插进来");
    return NULL;
}

static int cmd_threads(int nb_threads, int per_thread)
{
    pthread_t *tid = calloc(nb_threads, sizeof(*tid));
    Job *jobs = calloc(nb_threads, sizeof(*jobs));
    long total, errors, warnings, expect = (long)nb_threads * per_thread;
    int ret = 0;

    if (!tid || !jobs) {
        free(tid);
        free(jobs);
        return 1;
    }
    for (int i = 0; i < nb_threads; i++) {
        jobs[i] = (Job){ i, per_thread };
        if (pthread_create(&tid[i], NULL, worker, &jobs[i])) {
            fprintf(stderr, "创建线程 %d 失败\n", i);
            nb_threads = i;     /* 已经创建的照常等 */
            ret = 1;
            break;
        }
    }
    for (int i = 0; i < nb_threads; i++)
        pthread_join(tid[i], NULL);
    fflog_counts(&total, &errors, &warnings);
    printf("%d 个线程各写 %d 行，应该有 %ld 行警告，实际 %ld 行\n", nb_threads, per_thread, expect, warnings);
    if (warnings != expect)
        ret = 1;
    free(tid);
    free(jobs);
    return ret;
}

int main(int argc, char **argv)
{
    FFLogOptions opt = { .level = AV_LOG_WARNING, .keep_level = AV_LOG_DEBUG, .keep_lines = 32 };
    int ret;

    if (argc < 2) {
usage:
        fprintf(stderr, "用法：%s levels | open <文件> | threads <线程数> <每线程行数>  [level=… keep=… lines=… ts=…]\n", argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "levels")) {
        if (parse_options(argc, argv, 2, &opt) || fflog_init(&opt))
            return 1;
        ret = cmd_levels(&opt);
    } else if (!strcmp(argv[1], "open") && argc >= 3) {
        if (parse_options(argc, argv, 3, &opt) || fflog_init(&opt))
            return 1;
        ret = cmd_open(argv[2]);
    } else if (!strcmp(argv[1], "threads") && argc >= 4) {
        const int n = atoi(argv[2]), m = atoi(argv[3]);

        if (n < 1 || m < 1)
            goto usage;
        opt.keep_lines = 8;     /* 线程测试只看行数，缓冲区小一点 */
        if (parse_options(argc, argv, 4, &opt) || fflog_init(&opt))
            return 1;
        ret = cmd_threads(n, m);
    } else {
        goto usage;
    }
    fflog_close();
    return ret;
}
