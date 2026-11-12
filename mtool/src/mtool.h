/*
 * mtool：命令行媒体工具箱（综合项目）。所有子命令共用的声明。
 *
 * 退出码只有三个（L22 第 4 步的约定）：
 *   0  成功
 *   1  失败：输入打不开、文件损坏、写不出来……原因一定写在 stderr 上
 *   2  用法错误：子命令不存在、选项不认识、缺参数
 * 不拿 FFmpeg 的错误码当退出码：负的错误码只剩低 8 位，AVERROR_EOF 和 AVERROR_EXIT 都变成 187，没法用。
 */
#ifndef MTOOL_H
#define MTOOL_H

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>

#define MT_OK    0
#define MT_FAIL  1
#define MT_USAGE 2

/* ---- 子命令：argv[0] 是子命令的名字，argv[1] 起是它自己的选项和参数 ---- */
int cmd_info(int argc, char **argv);
int cmd_cut(int argc, char **argv);
int cmd_thumb(int argc, char **argv);
int cmd_audio(int argc, char **argv);
int cmd_transcode(int argc, char **argv);
int cmd_push(int argc, char **argv);

/* ---- 错误和提示：都写到 stderr，统一以“mtool：”开头 ---- */
void mt_err(const char *fmt, ...) av_printf_format(1, 2);
/* 错误加上 FFmpeg 错误码的说明：“mtool：错误：打不开 x.mp4：Invalid data found when processing input（文件损坏，或者不是媒体文件）” */
void mt_averr(int averr, const char *fmt, ...) av_printf_format(2, 3);
void mt_warn(const char *fmt, ...) av_printf_format(1, 2);
/* 子命令用法错误：打印“mtool：用法错误：……”和一行提示，返回 MT_USAGE */
int  mt_usage_err(const char *cmd, const char *fmt, ...) av_printf_format(2, 3);

/* ---- 输入输出 ---- */
/* 打开输入并读出流信息，失败时已经打印好错误，返回负的 AVERROR。 */
int mt_open_input(AVFormatContext **ctx, const char *path);
/* 把 AVERROR 翻译成给用户看的一句话（比 av_strerror 多一句“可能是什么原因”）。 */
const char *mt_errhint(int averr);

/* ---- 解码器和滤镜图（media.c，来自 L5、L11、L12、L16） ---- */
/* 为一路流打开解码器；失败时已经打印好错误。 */
int mt_open_decoder(const AVStream *st, int keyframes_only, AVCodecContext **dec);

typedef struct MtGraph {
    AVFilterGraph   *graph;
    AVFilterContext *src, *sink;
} MtGraph;
/*
 * 建滤镜图：入口按解码器的参数，中间是 desc（滤镜图描述，写法和 ffmpeg 的 -vf、-af 相同），
 * 出口只接受 enc 支持的格式（enc 为 NULL 不限制）；格式对不上时图会自动插入 scale、aresample。
 * frame_rate 只用于视频入口，未知填 {0, 1}。失败时已经打印好错误。
 */
int  mt_graph_init(MtGraph *g, AVCodecContext *dec, AVRational frame_rate, const char *desc, const AVCodec *enc);
void mt_graph_free(MtGraph *g);
/* 视频流的显示矩阵转成“让画面朝上”的滤镜（和 ffmpeg 命令行自动旋转的做法相同）。没有旋转写成空字符串。 */
void mt_rotation_filter(const AVStream *st, char *buf, size_t size);

/* ---- 时间：接受“90”“1.5”“1:30”“01:02:03.5”，成功返回 0，结果是秒 ---- */
int mt_parse_time(const char *s, double *seconds);

/* ---- 进度：一行刷新，stderr 不是终端时只在结束时打印一行 ---- */
typedef struct Progress {
    const char *label;
    double      total;      /* 总时长（秒），未知填 0 */
    int64_t     t0;         /* 开始时的 av_gettime_relative() */
    int64_t     last;       /* 上一次刷新的时间 */
    int         tty;
    int         shown;
} Progress;
void mt_progress_start(Progress *p, const char *label, double total_seconds);
/* done 是已经处理的媒体时间（秒） */
void mt_progress_update(Progress *p, double done, int64_t frames);
void mt_progress_end(Progress *p, double done, int64_t frames);

/* ---- 结构化输出：同一份数据既能打印成文本，也能打印成 JSON ---- */
typedef struct Out {
    FILE *f;
    int   json;
    int   depth;
    char  first[24];            /* 每一层是不是还没写过东西（决定要不要逗号） */
    char  is_array[24];
    int   arr_idx[24];
    const char *arr_name[24];
} Out;
void out_init(Out *o, FILE *f, int json);
void out_obj_begin(Out *o, const char *key);   /* key 为 NULL：顶层，或数组里的一个元素 */
void out_obj_end(Out *o);
void out_arr_begin(Out *o, const char *key);
void out_arr_end(Out *o);
void out_str(Out *o, const char *key, const char *val);
void out_int(Out *o, const char *key, int64_t val);
void out_dbl(Out *o, const char *key, double val, int prec);
void out_bool(Out *o, const char *key, int val);

/* ---- 日志和启动检查（main.c 用） ---- */
int  mt_log_init(int level, int keep_log);
void mt_log_dump_on_error(void);
void mt_log_close(void);
/* 头文件的版本和运行时库的版本一致吗？不一致打印原因并返回 -1（L22 第 5 步）。 */
int  mt_check_versions(void);
void mt_print_versions(FILE *f);

#endif
