/*
 * Copyright (c) 2014 Lukasz Marek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/**
 * @file libavformat AVIOContext list directory API usage example
 * @example avio_list_dir.c
 *
 * Show how to list directories through the libavformat AVIOContext API.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/avio_list_dir.c 的拷贝，只加了以“课程注释”开头的中文注释。
 * 拷贝的不是提交 a337e8d 的原版，而是打了 1 个 patch（0085）的版本，改动过的地方下面标了编号：
 *     0085  用来放“用户号(组号)”的数组只有 20 字节，十位以上的 id 被截断，加大了
 * 另有两个 patch 改的不是这个文件而是 libavformat/file.c，要装了打过补丁的 libavformat 才看得到区别（README 第 2 步）：
 *     0086  avio_open_dir() 不认 "file:" 前缀（avio_open2() 和 ffmpeg 都认），这个示例传 "file:/tmp" 会报
 *           "No such file or directory"
 *     0093  avio_close_dir() 会 close(0)，把标准输入关掉：file 协议的“关闭”函数去关一个从没打开过的 0 号描述符
 * 这个示例演示：用 libavformat 的协议层列出“目录”里有什么，不用 opendir()/readdir()。
 * 好处是同一套代码可以列别的协议的“目录”：本机文件、ftp、smb、sftp（后三个要编译时打开对应的选项）。
 * 数据是这样流动的：
 *     avio_open_dir(&ctx, url, NULL)      按 url 的前缀选协议（没有前缀就是 file），要求这个协议有“列目录”的三个函数
 *     avio_read_dir(ctx, &entry)          每调一次得到一项 AVIODirEntry，没有了得到 entry == NULL、返回 0
 *     avio_free_directory_entry(&entry)   每一项用完都要释放
 *     avio_close_dir(&ctx)                关闭
 * 不是所有协议都能列目录：http 返回 AVERROR(ENOSYS)（“Function not implemented”）。
 * 本讲自己写的 avio_ls.c 在这个示例的基础上加了排序、-l 长格式、-R 递归、可读的时间。
 */

/*
 * 课程注释：下面的 avcodec.h 这个示例其实用不到：本程序只用了 libavformat 的目录接口和 libavutil 的日志。
 * 保留它是因为官方示例原样如此；自己的程序（avio_ls.c）就没有包含它。
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>

/*
 * 课程注释：把 AVIODirEntry 的 type 变成可以打印的字符串。file 协议只会产生前七种（目录、文件、管道、字符设备、
 * 块设备、符号链接、socket）；SERVER、SHARE、WORKGROUP 是 smb 协议用的。type 之外的几种也都要有默认值：
 * 协议拿不到类型时就是 AVIO_ENTRY_UNKNOWN。
 */
static const char *type_string(int type)
{
    switch (type) {
    case AVIO_ENTRY_DIRECTORY:
        return "<DIR>";
    case AVIO_ENTRY_FILE:
        return "<FILE>";
    case AVIO_ENTRY_BLOCK_DEVICE:
        return "<BLOCK DEVICE>";
    case AVIO_ENTRY_CHARACTER_DEVICE:
        return "<CHARACTER DEVICE>";
    case AVIO_ENTRY_NAMED_PIPE:
        return "<PIPE>";
    case AVIO_ENTRY_SYMBOLIC_LINK:
        return "<LINK>";
    case AVIO_ENTRY_SOCKET:
        return "<SOCKET>";
    case AVIO_ENTRY_SERVER:
        return "<SERVER>";
    case AVIO_ENTRY_SHARE:
        return "<SHARE>";
    case AVIO_ENTRY_WORKGROUP:
        return "<WORKGROUP>";
    case AVIO_ENTRY_UNKNOWN:
    default:
        break;
    }
    return "<UNKNOWN>";
}

/*
 * 课程注释：列出一个目录。AVIODirContext 是不透明的类型，只能通过 avio_open_dir() 得到、avio_close_dir() 释放。
 * entry 每次由 avio_read_dir() 分配，用完要 avio_free_directory_entry()，不能自己 free()：里面的 name 也是它分配的。
 * 头文件里写明：只有 name 和 type 这两个字段一定有值，其余的字段（大小、各种时间、用户号、组号、权限）
 * 取决于协议和平台，拿不到时是 -1。所以下面对 filemode 单独判断了 -1，大小和时间则原样打印，可能打出 -1。
 */
static int list_op(const char *input_dir)
{
    AVIODirEntry *entry = NULL;
    AVIODirContext *ctx = NULL;
    int cnt, ret;
    /*
     * 课程注释：patch 0085。原版是 uid_and_gid[20]，snprintf 也只写 20 字节。用户号、组号都是 int64_t，
     * 各自最多 20 个字符（19 位数字加符号），再加上两个括号和结尾的 NUL，最坏要 43 字节，取 44。
     * 实测：文件的属主是 4294967294(4294967293)（用户命名空间、网络文件系统上常见）时，原版只打出 4294967294(42949672。
     * filemode[4]：file 协议给的是 st_mode & 0777，最多三位八进制数，加 NUL 刚好 4 字节。
     */
    /* Two int64_t, each up to 20 characters with the sign, plus "(", ")" and NUL. */
    char filemode[4], uid_and_gid[44];

    /*
     * 课程注释：第三个参数是协议选项（AVDictionary），这里不用。常见的失败：目录不存在（ENOENT）、
     * 没有权限（EACCES）、协议不能列目录（ENOSYS）。出错时 ctx 是 NULL，下面 fail: 里再 avio_close_dir(&ctx) 也不会崩，
     * 只是返回 AVERROR(EINVAL)，返回值本来就没人看。
     */
    if ((ret = avio_open_dir(&ctx, input_dir, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot open directory: %s.\n", av_err2str(ret));
        goto fail;
    }

    cnt = 0;
    for (;;) {
        /*
         * 课程注释：返回 < 0 是出错；返回 0 并且 entry 为 NULL 是“列完了”，下面的 if (!entry) break 就是这个意思。
         * file 协议已经把 "." 和 ".." 跳过了，别的协议不保证；顺序是 readdir() 给的顺序，不是按名字排的。
         */
        if ((ret = avio_read_dir(ctx, &entry)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Cannot list directory: %s.\n", av_err2str(ret));
            goto fail;
        }
        if (!entry)
            break;
        /*
         * 课程注释：filemode 是 Unix 权限，用八进制打印（644、755 这种）。注意 file 协议只给 st_mode 的低九位，
         * setuid、setgid、sticky 位（4755 里的 4）看不到。
         */
        if (entry->filemode == -1) {
            snprintf(filemode, 4, "???");
        } else {
            snprintf(filemode, 4, "%3"PRIo64, entry->filemode);
        }
        /* 课程注释：patch 0085，buffer 的大小用 sizeof，不再写死 20。 */
        snprintf(uid_and_gid, sizeof(uid_and_gid), "%"PRId64"(%"PRId64")",
                 entry->user_id, entry->group_id);
        /*
         * 课程注释：表头只在第一项之前打一次，所以空目录什么都不打印。三个时间是“微秒数”（从 1970 年算起），
         * 不是可读的日期：1794505826000000 是 2026-11-12 17:50:26 UTC。avio_ls.c 里换算成了可读的时间。
         * 类型一列 %-9s 放不下 "<BLOCK DEVICE>"、"<CHARACTER DEVICE>"，这两种列会错位，不影响内容。
         */
        if (cnt == 0)
            av_log(NULL, AV_LOG_INFO, "%-9s %12s %30s %10s %s %16s %16s %16s\n",
                   "TYPE", "SIZE", "NAME", "UID(GID)", "UGO", "MODIFIED",
                   "ACCESSED", "STATUS_CHANGED");
        av_log(NULL, AV_LOG_INFO, "%-9s %12"PRId64" %30s %10s %s %16"PRId64" %16"PRId64" %16"PRId64"\n",
               type_string(entry->type),
               entry->size,
               entry->name,
               uid_and_gid,
               filemode,
               entry->modification_timestamp,
               entry->access_timestamp,
               entry->status_change_timestamp);
        /* 课程注释：释放这一项；释放以后 entry 变成 NULL，下一轮 avio_read_dir() 重新分配。 */
        avio_free_directory_entry(&entry);
        cnt++;
    };

    /*
     * 课程注释：正常列完时 ret 是最后一次 avio_read_dir() 的返回值 0；出错时是负的错误码，main() 据此返回 1。
     * 任何路径都要走到这里关掉目录（goto fail），不然目录的文件描述符不会关。
     * 没打 patch 0093 的 libavformat 里，这一行还会顺手 close(0)：用 strace -e trace=close 能看到。
     * 对这个只列一个目录就退出的程序没有影响，对后面要用标准输入的程序、对多线程的服务器有（本讲第 9 步）。
     */
  fail:
    avio_close_dir(&ctx);
    return ret;
}

static void usage(const char *program_name)
{
    fprintf(stderr, "usage: %s input_dir\n"
            "API example program to show how to list files in directory "
            "accessed through AVIOContext.\n", program_name);
}

int main(int argc, char *argv[])
{
    int ret;

    /*
     * 课程注释：日志级别设成 DEBUG，列表走 av_log(…, AV_LOG_INFO, …)，所以和 libavformat 自己的调试信息混在 stderr 里，
     * 不能当成“数据”用管道处理。自己的 avio_ls.c 改成了 printf 到 stdout。
     */
    av_log_set_level(AV_LOG_DEBUG);

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    /*
     * 课程注释：头文件说这个调用“可选，不再推荐”：它只是为了绕开旧版 GnuTLS、OpenSSL 的线程安全问题，
     * 链接的是新版或者根本不用 TLS 时不需要。官方示例仍然保留了它，和最后的 avformat_network_deinit() 配对。
     * 文件和目录用不到网络；将来换成 sftp://、ftp:// 也不用改这里。
     */
    avformat_network_init();

    ret = list_op(argv[1]);

    avformat_network_deinit();

    return ret < 0 ? 1 : 0;
}
