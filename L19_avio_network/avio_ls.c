/*
 * avio_ls.c —— 课程 L19：自己的“列目录”程序
 *
 * 官方示例 avio_list_dir.c 的加强版，用的还是 libavformat 的目录接口
 * avio_open_dir()、avio_read_dir()、avio_close_dir()，不调 opendir()、readdir()。
 * 和官方示例相比：
 *   - 结果按名字、大小或时间排序（官方示例按 readdir() 给的顺序，每次可能不同）；
 *   - 输出像 ls -l：类型、权限、用户号、组号、大小、修改时间、名字，时间是可读的；
 *   - 可以递归（recurse=1）；
 *   - 结果写到 stdout，错误写到 stderr，可以接管道；
 *   - 取不到的信息（协议不给）显示成 ? 或 -，而不是 -1。
 *
 * 用法：
 *   avio_ls <目录的URL> [名字=值 ...]
 *
 *   long=0|1        long=1 时每项一行完整信息，默认 0 只打印名字（目录后面加 /）
 *   recurse=0|1     递归进子目录（符号链接不跟进），默认 0
 *   sort=name|size|time
 *                   name 按名字（字节序），size 按大小从大到小，time 按修改时间从新到旧；
 *                   大小或时间相同的按名字，默认 name
 *   其他 名字=值    当作协议选项交给 avio_open_dir()，比如 ftp 的 timeout=1000000
 *
 *   环境变量 TZ 决定时间显示成哪个时区；TZ=UTC 可以得到各机器上都一样的结果。
 *
 * 退出码：0 正常；1 用法错误；2 目录打不开；3 列到一半出错。
 */

/* -std=c11 下要这一行，才能用 POSIX 的 localtime_r */
#define _POSIX_C_SOURCE 200809L

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>

/* 从 AVIODirEntry 里抄下来的一项。avio_read_dir() 给的结构每次都要释放，排序要先留一份。 */
typedef struct Entry {
    char   *name;
    int     type;
    int64_t size, mtime, uid, gid, mode;
} Entry;

enum { SORT_NAME, SORT_SIZE, SORT_TIME };

typedef struct Config {
    int long_format;
    int recurse;
    int sort;
    AVDictionary *options;   /* 交给协议的选项，每次 avio_open_dir() 前复制一份 */
} Config;

/* 类型的一个字符，和 ls -l 的第一列一样；协议没告诉我们类型是 ? */
static char type_char(int type)
{
    switch (type) {
    case AVIO_ENTRY_DIRECTORY:        return 'd';
    case AVIO_ENTRY_FILE:             return '-';
    case AVIO_ENTRY_SYMBOLIC_LINK:    return 'l';
    case AVIO_ENTRY_NAMED_PIPE:       return 'p';
    case AVIO_ENTRY_SOCKET:           return 's';
    case AVIO_ENTRY_CHARACTER_DEVICE: return 'c';
    case AVIO_ENTRY_BLOCK_DEVICE:     return 'b';
    case AVIO_ENTRY_SERVER:           return 'S';   /* 下面三种只有 smb 协议会给 */
    case AVIO_ENTRY_SHARE:            return 'H';
    case AVIO_ENTRY_WORKGROUP:        return 'W';
    default:                          return '?';
    }
}

/* 权限九个字符，rwxr-xr-x 这种；mode 是 -1（不知道）时全是 ? */
static void mode_string(int64_t mode, char out[10])
{
    static const char rwx[] = "rwxrwxrwx";

    for (int i = 0; i < 9; i++) {
        if (mode < 0)
            out[i] = '?';
        else
            out[i] = (mode & (1 << (8 - i))) ? rwx[i] : '-';
    }
    out[9] = '\0';
}

/* 微秒时间戳换成本地时间的字符串；协议不给（-1）就是 "-" */
static void time_string(int64_t us, char *out, size_t size)
{
    time_t t;
    struct tm tm;

    if (us < 0) {
        snprintf(out, size, "-");
        return;
    }
    t = (time_t)(us / 1000000);
    if (!localtime_r(&t, &tm) || !strftime(out, size, "%Y-%m-%d %H:%M:%S", &tm))
        snprintf(out, size, "?");
}

static int compare(const Entry *a, const Entry *b, int sort)
{
    if (sort == SORT_SIZE && a->size != b->size)
        return a->size < b->size ? 1 : -1;      /* 大的在前 */
    if (sort == SORT_TIME && a->mtime != b->mtime)
        return a->mtime < b->mtime ? 1 : -1;    /* 新的在前 */
    return strcmp(a->name, b->name);
}

/* qsort() 的比较函数不能带参数，排序方式放在全局变量里 */
static int g_sort = SORT_NAME;

static int compare_qsort(const void *a, const void *b)
{
    return compare(a, b, g_sort);
}

static void free_entries(Entry *entries, int n)
{
    for (int i = 0; i < n; i++)
        av_free(entries[i].name);
    av_free(entries);
}

/*
 * 读完一个目录的所有项，存进 *out，返回项数；出错返回负的错误码。
 * 先全部读完、再排序、再打印：avio_read_dir() 的顺序是协议给的顺序，要排序就得都在手里。
 */
static int read_all(const char *url, const Config *cfg, Entry **out)
{
    AVIODirContext *ctx = NULL;
    AVIODirEntry *e = NULL;
    AVDictionary *opts = NULL;
    Entry *entries = NULL;
    int n = 0, ret;

    /* avio_open_dir() 会用掉（删掉）它认识的选项，所以每次给它一份副本 */
    av_dict_copy(&opts, cfg->options, 0);
    ret = avio_open_dir(&ctx, url, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        return ret;

    for (;;) {
        Entry *p;

        ret = avio_read_dir(ctx, &e);
        if (ret < 0)
            goto fail;
        if (!e)                 /* 返回 0、e 是 NULL：列完了 */
            break;

        p = av_realloc_array(entries, n + 1, sizeof(*entries));
        if (!p) {
            ret = AVERROR(ENOMEM);
            avio_free_directory_entry(&e);
            goto fail;
        }
        entries = p;
        entries[n] = (Entry){
            .name  = av_strdup(e->name),
            .type  = e->type,
            .size  = e->size,
            .mtime = e->modification_timestamp,
            .uid   = e->user_id,
            .gid   = e->group_id,
            .mode  = e->filemode,
        };
        avio_free_directory_entry(&e);
        if (!entries[n].name) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        n++;
    }
    avio_close_dir(&ctx);
    *out = entries;
    return n;

fail:
    free_entries(entries, n);
    avio_close_dir(&ctx);
    return ret;
}

static void print_entry(const Entry *e, const Config *cfg)
{
    if (cfg->long_format) {
        char mode[10], when[32];

        mode_string(e->mode, mode);
        time_string(e->mtime, when, sizeof(when));
        /* 用户号、组号、大小取不到时是 -1，原样打印出来：-1 比一个随便编的数字诚实 */
        printf("%c%s %10" PRId64 " %10" PRId64 " %12" PRId64 " %-19s %s\n",
               type_char(e->type), mode, e->uid, e->gid, e->size, when, e->name);
    } else {
        printf("%s%s\n", e->name, e->type == AVIO_ENTRY_DIRECTORY ? "/" : "");
    }
}

/* 列一个目录；递归时先打印自己的所有项，再逐个进子目录。返回 0 或负的错误码。 */
static int list(const char *url, const Config *cfg, int *first)
{
    Entry *entries = NULL;
    int n, ret = 0;

    n = read_all(url, cfg, &entries);
    if (n < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s: %s\n", url, av_err2str(n));
        return n;
    }
    g_sort = cfg->sort;
    if (n > 1)      /* 空目录时 entries 是 NULL，qsort() 不许传 NULL */
        qsort(entries, n, sizeof(*entries), compare_qsort);

    if (cfg->recurse) {
        /* 像 ls -R：每个目录前一行 "路径:"，目录之间空一行 */
        if (!*first)
            printf("\n");
        printf("%s:\n", url);
    }
    *first = 0;
    for (int i = 0; i < n; i++)
        print_entry(&entries[i], cfg);

    if (cfg->recurse) {
        for (int i = 0; i < n; i++) {
            char *sub;
            int r;

            if (entries[i].type != AVIO_ENTRY_DIRECTORY)   /* 符号链接不是 DIRECTORY，不会跟进 */
                continue;
            sub = av_append_path_component(url, entries[i].name);
            if (!sub) {
                ret = AVERROR(ENOMEM);
                break;
            }
            r = list(sub, cfg, first);
            av_free(sub);
            if (r < 0 && ret == 0)
                ret = r;        /* 一个子目录出错不影响别的，最后返回第一个错误 */
        }
    }
    free_entries(entries, n);
    return ret;
}

int main(int argc, char **argv)
{
    Config cfg = { .sort = SORT_NAME };
    int first = 1, ret;

    if (argc < 2 || argv[1][0] == '-') {
        fprintf(stderr, "用法：%s <目录的URL> [long=0|1] [recurse=0|1] [sort=name|size|time] [名字=值 ...]\n",
                argv[0]);
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        char *name, *val;

        if (!eq) {
            fprintf(stderr, "参数要写成 名字=值：%s\n", argv[i]);
            return 1;
        }
        name = av_strndup(argv[i], eq - argv[i]);
        val  = eq + 1;
        if (!strcmp(name, "long")) {
            cfg.long_format = atoi(val);
        } else if (!strcmp(name, "recurse")) {
            cfg.recurse = atoi(val);
        } else if (!strcmp(name, "sort")) {
            if (!strcmp(val, "name"))      cfg.sort = SORT_NAME;
            else if (!strcmp(val, "size")) cfg.sort = SORT_SIZE;
            else if (!strcmp(val, "time")) cfg.sort = SORT_TIME;
            else {
                fprintf(stderr, "sort 只能是 name、size、time：%s\n", val);
                av_free(name);
                return 1;
            }
        } else {
            av_dict_set(&cfg.options, name, val, 0);
        }
        av_free(name);
    }

    avformat_network_init();
    ret = list(argv[1], &cfg, &first);
    avformat_network_deinit();
    av_dict_free(&cfg.options);

    if (ret < 0)
        return first ? 2 : 3;   /* 一项都没打印出来就失败，是“打不开”；否则是“列到一半出错” */
    return 0;
}
