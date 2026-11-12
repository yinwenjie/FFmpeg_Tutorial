/*
 * mtool：命令行媒体工具箱。
 *
 *   mtool [全局选项] <子命令> [子命令的选项] [参数]
 *
 * 子命令：info 看信息，cut 不转码截取（后面的提交会加上 thumb、audio、transcode、push）。
 * 每个子命令的详细用法：mtool <子命令> --help
 */
#include <stdlib.h>
#include <string.h>

#include <libavutil/log.h>

#include "mtool.h"

typedef struct Command {
    const char *name;
    int       (*fn)(int, char **);
    const char *summary;
} Command;

static const Command commands[] = {
    { "info",      cmd_info,      "显示容器、流、GOP 和帧类型统计，可以输出 JSON" },
    { "cut",       cmd_cut,       "不转码截取一段，可以同时换容器" },
    { "thumb",     cmd_thumb,     "按时间间隔或关键帧生成缩略图，或者拼成一张图" },
};

static void usage(FILE *f)
{
    fprintf(f, "用法：mtool [全局选项] <子命令> [选项] [参数]\n\n子命令：\n");
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
        fprintf(f, "  %-10s %s\n", commands[i].name, commands[i].summary);
    fprintf(f,
            "\n全局选项（写在子命令前面）：\n"
            "  -q               只显示库的错误\n"
            "  -v               显示库的信息级日志；-vv 显示调试日志\n"
            "  --log-on-error   失败时把出错之前库输出的最近几十行日志倒出来（L22 第 3 步）\n"
            "  --version        显示 mtool 用的 FFmpeg 各个库的版本和许可证\n"
            "  -h, --help       显示这段说明\n"
            "\n退出码：0 成功，1 失败（原因在 stderr），2 用法错误。\n");
}

int main(int argc, char **argv)
{
    int level = AV_LOG_WARNING, log_on_error = 0, i, ret;
    const Command *cmd = NULL;

    for (i = 1; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(stdout);
            return MT_OK;
        } else if (!strcmp(argv[i], "--version")) {
            if (mt_check_versions() < 0)
                return MT_FAIL;
            mt_print_versions(stdout);
            return MT_OK;
        } else if (!strcmp(argv[i], "-q")) {
            level = AV_LOG_ERROR;
        } else if (!strcmp(argv[i], "-v")) {
            level = AV_LOG_INFO;
        } else if (!strcmp(argv[i], "-vv")) {
            level = AV_LOG_DEBUG;
        } else if (!strcmp(argv[i], "--log-on-error")) {
            log_on_error = 1;
        } else {
            fprintf(stderr, "mtool：用法错误：不认识的全局选项 %s\n", argv[i]);
            usage(stderr);
            return MT_USAGE;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return MT_USAGE;
    }
    for (size_t k = 0; k < sizeof(commands) / sizeof(commands[0]); k++)
        if (!strcmp(argv[i], commands[k].name))
            cmd = &commands[k];
    if (!cmd) {
        fprintf(stderr, "mtool：用法错误：不认识的子命令 %s\n", argv[i]);
        usage(stderr);
        return MT_USAGE;
    }

    if (mt_check_versions() < 0)
        return MT_FAIL;
    if (mt_log_init(level, log_on_error) < 0) {
        mt_err("初始化日志失败（内存不够）");
        return MT_FAIL;
    }

    ret = cmd->fn(argc - i, argv + i);
    if (ret == MT_FAIL && log_on_error)
        mt_log_dump_on_error();
    mt_log_close();
    return ret;
}
