/*
 * avio_serve.c —— 课程 L19：自己的 HTTP 文件服务器
 *
 * 官方示例 avio_http_serve_files.c 的加强版，还是用 libavformat 自己的 HTTP 服务端能力：
 * avio_open2()（listen=2）、avio_accept()、avio_handshake()、选项 resource 和 reply_code。
 * 和官方示例相比：
 *   - 发一个目录下的所有文件，请求的路径对应目录里的路径，请求目录得到一份目录列表（默认用 POSIX 的 opendir()，
 *     编译时加 -DUSE_AVIO_DIR 换成 libavformat 的 avio_open_dir()，原因见 collect_items() 上面的注释）；
 *   - 每个客户端一个线程，不 fork：没有子进程要回收，监听的 socket 也不会被复制到别的进程里；
 *   - 检查请求的路径：百分号编码要解码，查询串去掉，路径里不许有 ".." 也不许通过符号链接跑出根目录；
 *   - 按扩展名回复 Content-Type（用选项 content_type），文件打不开时回 404 或 403 或 500，同时处理的客户端
 *     太多回 429；
 *   - 客户端中途断开能发现，每个请求打印一行日志；
 *   - Ctrl+C 或 SIGTERM 以后，等正在发送的客户端发完再退出。
 *
 * 用法：
 *   avio_serve <根目录> <http://地址[:端口]> [max=N]
 *
 *   max=N    同时处理的客户端最多 N 个，更多的回 429，默认 16
 *
 *   地址写 127.0.0.1 只有本机能连；0.0.0.0 是所有网卡。
 *   每个请求在 stdout 打印一行：路径 状态码 发送的正文字节数 [aborted]
 *
 * libavformat 内置的 HTTP 服务器的限制（本程序没法改变）：只支持 GET，HEAD 等方法得到 400；没有 Range（206），
 * 客户端不能从中间开始取；状态码只能是 200、400、403、404、429、500；200 的回复用分块传输，没有 Content-Length。
 *
 * 退出码：0 正常退出；1 用法错误；2 监听失败；3 accept 出错。
 */

/* -std=c11 下要这一行，才能用 POSIX 的线程、sigaction，和 XSI 里的 realpath */
#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avstring.h>
#include <libavutil/bprint.h>
#include <libavutil/dict.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>

typedef struct Server {
    char       *root;        /* 根目录，绝对路径，末尾没有 / */
    size_t      root_len;
    int         max;         /* 同时处理的客户端上限 */
    atomic_int  active;      /* 现在正在处理的客户端个数 */
} Server;

typedef struct Client {
    AVIOContext *io;         /* avio_accept() 给的 */
    Server      *srv;
} Client;

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* ---- 路径 ---------------------------------------------------------------------------------- */

static int hex_value(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * 把请求的资源变成路径：先去掉查询串（? 以后）和片段（# 以后），再解百分号编码（%41 → A）。
 * 官方示例不做这两件事：带查询串或者带 %20 的请求，对它来说是另一个资源。
 * 解出来的路径里不许有 NUL（%00），否则后面 C 字符串函数只看到前半截，检查就被绕过了。
 * 成功返回 0，格式不对或太长返回 -1。
 */
static int decode_path(const char *res, char *out, size_t size)
{
    size_t n = 0;

    for (const char *p = res; *p && *p != '?' && *p != '#'; p++) {
        int c = (unsigned char)*p;

        if (c == '%') {
            int hi = hex_value((unsigned char)p[1]);
            int lo = hi < 0 ? -1 : hex_value((unsigned char)p[2]);

            if (hi < 0 || lo < 0)
                goto bad;
            c = hi * 16 + lo;
            p += 2;
            if (c == 0)
                goto bad;
        }
        if (n + 1 >= size)
            goto bad;
        out[n++] = c;
    }
    out[n] = '\0';
    return out[0] == '/' ? 0 : -1;

bad:
    out[n] = '\0';     /* 出错时 out 里是解到一半的路径，也要有结尾，日志里还要打印它 */
    return -1;
}

/* 路径里有没有 ".." 这一段：/a/../b、/..、/a/.. 都算；/a..b、/..a 不算 */
static int has_dotdot(const char *path)
{
    for (const char *p = path; *p; ) {
        const char *end = strchr(p + 1, '/');
        size_t len = end ? (size_t)(end - p - 1) : strlen(p + 1);

        if (len == 2 && p[1] == '.' && p[2] == '.')
            return 1;
        if (!end)
            break;
        p = end;
    }
    return 0;
}

static const char *content_type(const char *path)
{
    static const struct { const char *ext, *type; } table[] = {
        { "mp4",  "video/mp4" },            { "m4v",  "video/mp4" },
        { "mkv",  "video/x-matroska" },     { "webm", "video/webm" },
        { "ts",   "video/mp2t" },           { "flv",  "video/x-flv" },
        { "avi",  "video/x-msvideo" },      { "mov",  "video/quicktime" },
        { "mp3",  "audio/mpeg" },           { "mp2",  "audio/mpeg" },
        { "aac",  "audio/aac" },            { "m4a",  "audio/mp4" },
        { "wav",  "audio/wav" },            { "flac", "audio/flac" },
        { "ogg",  "audio/ogg" },            { "m3u8", "application/vnd.apple.mpegurl" },
        { "png",  "image/png" },            { "jpg",  "image/jpeg" },
        { "txt",  "text/plain" },           { "json", "application/json" },
    };
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');

    if (dot && (!slash || dot > slash)) {
        for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
            if (!av_strcasecmp(dot + 1, table[i].ext))
                return table[i].type;
    }
    return "application/octet-stream";
}

/* ---- 目录列表 ------------------------------------------------------------------------------- */

typedef struct Item {
    char   *name;
    int     type;       /* AVIO_ENTRY_DIRECTORY、AVIO_ENTRY_SYMBOLIC_LINK 或者别的 */
    int64_t size;
} Item;

static int item_cmp(const void *a, const void *b)
{
    return strcmp(((const Item *)a)->name, ((const Item *)b)->name);
}

static int add_item(Item **items, int *n, const char *name, int type, int64_t size)
{
    Item *p = av_realloc_array(*items, *n + 1, sizeof(**items));
    char *copy = av_strdup(name);

    if (!p || !copy) {
        av_free(copy);
        if (p)
            *items = p;
        return AVERROR(ENOMEM);
    }
    *items = p;
    (*items)[(*n)++] = (Item){ copy, type, size };
    return 0;
}

/*
 * 读出目录 path 里的所有项（不含 . 和 ..）。成功返回 0，*items 和 *n 由调用者释放。
 *
 * 默认用 POSIX 的 opendir()，不用 libavformat 的 avio_open_dir()。原因：file 协议的 avio_close_dir()
 * 在没有打 patch 0093 的 libavformat 里会 close(0)，也就是关掉 0 号文件描述符。单线程的程序里它是标准输入，
 * 没人在乎；这个服务器里，关掉以后 0 号成了“最小的空号”，下一个连进来的客户端会拿到它，
 * 再下一次列目录，就把那个客户端的连接关了：客户端收到空回复，或者收到别人的回复（README 第 9 步）。
 * 打了补丁以后，编译时加 -DUSE_AVIO_DIR 就可以用 avio_open_dir()。
 */
#ifdef USE_AVIO_DIR
static int collect_items(const char *path, Item **items, int *n)
{
    AVIODirContext *ctx = NULL;
    int ret = avio_open_dir(&ctx, path, NULL);

    if (ret < 0)
        return ret;
    for (;;) {
        AVIODirEntry *e = NULL;

        ret = avio_read_dir(ctx, &e);
        if (ret < 0 || !e)
            break;
        ret = add_item(items, n, e->name, e->type, e->size);
        avio_free_directory_entry(&e);
        if (ret < 0)
            break;
    }
    avio_close_dir(&ctx);
    return ret < 0 ? ret : 0;
}
#else
static int collect_items(const char *path, Item **items, int *n)
{
    DIR *d = opendir(path);
    struct dirent *de;
    int ret = 0;

    if (!d)
        return AVERROR(errno);
    while ((de = readdir(d))) {
        char *full;
        struct stat st;
        int type = AVIO_ENTRY_FILE;
        int64_t size = -1;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        full = av_append_path_component(path, de->d_name);
        if (full && !lstat(full, &st)) {
            type = S_ISDIR(st.st_mode) ? AVIO_ENTRY_DIRECTORY :
                   S_ISLNK(st.st_mode) ? AVIO_ENTRY_SYMBOLIC_LINK : AVIO_ENTRY_FILE;
            size = st.st_size;
        }
        av_free(full);
        if ((ret = add_item(items, n, de->d_name, type, size)) < 0)
            break;
    }
    closedir(d);
    return ret;
}
#endif

/*
 * 目录 path 的列表，排好序写进 bp：每项一行，类型（d 目录、l 符号链接、- 别的）、大小、名字，目录的名字后面加 /。
 * 成功返回 0。
 */
static int build_listing(const char *path, AVBPrint *bp)
{
    Item *items = NULL;
    int n = 0, ret = collect_items(path, &items, &n);

    if (ret >= 0) {
        if (n > 1)      /* 空目录时 items 是 NULL，qsort() 不许传 NULL */
            qsort(items, n, sizeof(*items), item_cmp);
        for (int i = 0; i < n; i++) {
            int dir = items[i].type == AVIO_ENTRY_DIRECTORY;
            char c = dir ? 'd' : items[i].type == AVIO_ENTRY_SYMBOLIC_LINK ? 'l' : '-';

            av_bprintf(bp, "%c %12" PRId64 " %s%s\n", c, items[i].size, items[i].name, dir ? "/" : "");
        }
        ret = av_bprint_is_complete(bp) ? 0 : AVERROR(ENOMEM);
    }
    for (int i = 0; i < n; i++)
        av_free(items[i].name);
    av_free(items);
    return ret;
}

/* ---- 处理一个客户端 -------------------------------------------------------------------------- */

/* open 的错误码换成 HTTP 状态码：没有这个文件 404，没有权限 403，别的都是服务器的问题 500 */
static int error_to_reply(int err)
{
    if (err == AVERROR(ENOENT) || err == AVERROR(ENOTDIR))
        return AVERROR_HTTP_NOT_FOUND;
    if (err == AVERROR(EACCES) || err == AVERROR(EPERM))
        return AVERROR_HTTP_FORBIDDEN;
    return AVERROR_HTTP_SERVER_ERROR;
}

static int status_number(int reply)
{
    switch (reply) {
    case AVERROR_HTTP_BAD_REQUEST:      return 400;
    case AVERROR_HTTP_FORBIDDEN:        return 403;
    case AVERROR_HTTP_NOT_FOUND:        return 404;
    case AVERROR_HTTP_TOO_MANY_REQUESTS:return 429;
    case AVERROR_HTTP_SERVER_ERROR:     return 500;
    default:                            return reply;
    }
}

static void *client_thread(void *arg)
{
    Client *cl = arg;
    Server *srv = cl->srv;
    AVIOContext *io = cl->io;
    AVIOContext *in = NULL;
    struct stat st;
    AVBPrint listing;
    char *resource = NULL, *real = NULL;
    char path[4096], full[PATH_MAX + 1];
    const char *ctype = "text/plain";
    int reply = AVERROR_HTTP_BAD_REQUEST, ret, aborted = 0, bad_path;
    char note[128] = "";
    int64_t sent = 0;
    int busy = atomic_fetch_add(&srv->active, 1) + 1 > srv->max;

    av_free(cl);
    av_bprint_init(&listing, 0, AV_BPRINT_SIZE_UNLIMITED);
    path[0] = '\0';

    /*
     * 第一段握手：读请求头，直到选项 resource 有值。在这之前出错（客户端连上来什么都没发就断开、
     * 请求头太长或者不是 HTTP），libavformat 自己回 400 之类的，我们没有资源可看，直接关掉。
     */
    while ((ret = avio_handshake(io)) > 0) {
        av_opt_get(io, "resource", AV_OPT_SEARCH_CHILDREN, (uint8_t **)&resource);
        if (resource && resource[0])
            break;
        av_freep(&resource);
    }
    if (ret < 0) {
        snprintf(note, sizeof(note), " (握手失败：%s)", av_err2str(ret));
        goto end;
    }

    /* 决定回复什么。路径先解出来（日志里要用）；顺序：同时处理的太多 → 路径不对 → 目录 → 文件 */
    bad_path = !resource || decode_path(resource, path, sizeof(path)) < 0;
    if (busy) {
        reply = AVERROR_HTTP_TOO_MANY_REQUESTS;
    } else if (bad_path) {
        reply = AVERROR_HTTP_BAD_REQUEST;
    } else if (has_dotdot(path)) {
        reply = AVERROR_HTTP_FORBIDDEN;
    } else if (snprintf(full, sizeof(full), "%s%s", srv->root, path) >= (int)sizeof(full)) {
        reply = AVERROR_HTTP_BAD_REQUEST;
    } else if (!(real = realpath(full, NULL))) {
        reply = errno == EACCES ? AVERROR_HTTP_FORBIDDEN : AVERROR_HTTP_NOT_FOUND;
    } else if (strncmp(real, srv->root, srv->root_len) ||
               (real[srv->root_len] != '/' && real[srv->root_len] != '\0')) {
        /* realpath() 把符号链接都解开了，解开以后不在根目录里面，就是通过符号链接跑出去了 */
        reply = AVERROR_HTTP_FORBIDDEN;
    } else if (!stat(real, &st) && S_ISDIR(st.st_mode)) {
        /* 是目录：回目录列表。目录列表在回复之前就读好，读不出来可以回 500 */
        if (build_listing(real, &listing) < 0) {
            reply = AVERROR_HTTP_SERVER_ERROR;
        } else {
            reply = 200;
            ctype = "text/plain; charset=utf-8";
        }
    } else if ((ret = avio_open2(&in, real, AVIO_FLAG_READ, NULL, NULL)) < 0) {
        reply = error_to_reply(ret);
    } else {
        reply = 200;
        ctype = content_type(real);
    }

    /* 第二段握手：设好 Content-Type 和状态码，让 libavformat 把回复头（非 200 还有正文）发出去 */
    if (reply == 200)
        av_opt_set(io, "content_type", ctype, AV_OPT_SEARCH_CHILDREN);
    ret = av_opt_set_int(io, "reply_code", reply, AV_OPT_SEARCH_CHILDREN);
    while (ret >= 0 && (ret = avio_handshake(io)) > 0)
        ;
    if (ret < 0) {
        snprintf(note, sizeof(note), " (握手失败：%s)", av_err2str(ret));
        goto end;
    }

    if (reply == 200) {
        if (in) {
            uint8_t buf[32768];

            for (;;) {
                int n = avio_read(in, buf, sizeof(buf));

                if (n < 0)      /* AVERROR_EOF 是读完；别的是读出错，这时客户端只收到半个文件，只能断开 */
                    break;
                avio_write(io, buf, n);
                if (io->error) {    /* avio_write() 没有返回值，客户端断开了会在 error 字段里留下错误码 */
                    aborted = 1;
                    break;
                }
                sent += n;
            }
        } else {
            avio_write(io, (const unsigned char *)listing.str, listing.len);
            if (io->error)
                aborted = 1;
            else
                sent = listing.len;
        }
    }
    avio_flush(io);
    if (io->error)
        aborted = 1;

end:
    printf("%s %d %" PRId64 "%s%s\n", path[0] ? path : "-", status_number(reply), sent,
           aborted ? " aborted" : "", note);
    fflush(stdout);
    /* 关闭客户端：对 200 的回复，这里会写出分块传输的结束标记，客户端靠它知道正文结束了 */
    avio_close(io);
    avio_close(in);
    av_bprint_finalize(&listing, NULL);
    av_freep(&resource);
    free(real);
    atomic_fetch_sub(&srv->active, 1);
    return NULL;
}

int main(int argc, char **argv)
{
    Server srv = { .max = 16 };
    AVIOContext *server = NULL;
    AVDictionary *options = NULL;
    struct sigaction sa = { .sa_handler = on_signal };
    char *root;
    int ret, exit_code = 0;

    if (argc < 3) {
        fprintf(stderr, "用法：%s <根目录> <http://地址[:端口]> [max=N]\n", argv[0]);
        return 1;
    }
    for (int i = 3; i < argc; i++) {
        if (!strncmp(argv[i], "max=", 4)) {
            srv.max = atoi(argv[i] + 4);
        } else {
            fprintf(stderr, "不认识的参数：%s\n", argv[i]);
            return 1;
        }
    }
    if (srv.max < 1) {
        fprintf(stderr, "max 要大于 0\n");
        return 1;
    }

    /* 根目录变成绝对路径，去掉末尾的 /：后面检查“有没有跑出根目录”是比较字符串的前缀 */
    root = realpath(argv[1], NULL);
    if (!root) {
        perror(argv[1]);
        return 1;
    }
    srv.root = root;
    srv.root_len = strlen(root);
    if (srv.root_len == 1 && root[0] == '/')
        srv.root_len = 0;       /* 根目录就是 /：前缀是空串 */
    else if (srv.root_len > 1 && root[srv.root_len - 1] == '/')
        root[--srv.root_len] = '\0';
    atomic_init(&srv.active, 0);

    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);   /* 客户端断开以后再写，libavformat 用 MSG_NOSIGNAL，这里再保险一层 */

    avformat_network_init();

    /*
     * listen=2：接受多个客户端，avio_open2() 马上返回，之后用 avio_accept()。
     * listen_timeout：avio_accept() 最多等这么多毫秒，没有客户端连进来就返回 AVERROR(ETIMEDOUT)，
     * 我们借这个机会看一下 Ctrl+C 有没有按过。不设的话 avio_accept() 会一直阻塞。
     */
    av_dict_set(&options, "listen", "2", 0);
    av_dict_set(&options, "listen_timeout", "300", 0);
    ret = avio_open2(&server, argv[2], AVIO_FLAG_WRITE, NULL, &options);
    av_dict_free(&options);
    if (ret < 0) {
        fprintf(stderr, "不能监听 %s：%s\n", argv[2], av_err2str(ret));
        free(root);
        return 2;
    }
    fprintf(stderr, "在 %s 上发送 %s（Ctrl+C 退出）\n", argv[2], srv.root);

    while (!g_stop) {
        AVIOContext *io = NULL;
        Client *cl;
        pthread_t th;
        pthread_attr_t attr;

        ret = avio_accept(server, &io);
        if (ret == AVERROR(ETIMEDOUT) || ret == AVERROR(EINTR))
            continue;
        if (ret < 0) {
            fprintf(stderr, "accept 出错：%s\n", av_err2str(ret));
            exit_code = 3;
            break;
        }
        cl = av_malloc(sizeof(*cl));
        if (!cl) {
            avio_close(io);
            continue;
        }
        cl->io = io;
        cl->srv = &srv;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &attr, client_thread, cl)) {
            fprintf(stderr, "创建线程失败\n");
            avio_close(io);
            av_free(cl);
        }
        pthread_attr_destroy(&attr);
    }

    /* 退出：不再接新的客户端，等正在发送的发完（最多 10 秒） */
    for (int i = 0; i < 200 && atomic_load(&srv.active) > 0; i++)
        nanosleep(&(struct timespec){ .tv_nsec = 50000000 }, NULL);
    if (atomic_load(&srv.active) > 0)
        fprintf(stderr, "还有 %d 个客户端没发完，不等了\n", atomic_load(&srv.active));

    /*
     * 关闭监听。没有打 patch 0087 的 libavformat 在这里会永远卡住（关闭监听的上下文时它往监听的 socket 写
     * 分块传输的结束标记）。alarm(2) 是兜底：2 秒还没返回，SIGALRM 的默认动作结束进程。
     * 打了补丁的 libavformat 马上返回，alarm(0) 取消闹钟。
     */
    alarm(2);
    avio_close(server);
    alarm(0);
    avformat_network_deinit();
    free(root);
    return exit_code;
}
