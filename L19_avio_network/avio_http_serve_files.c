/*
 * Copyright (c) 2015 Stephan Holljes
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
 * @file libavformat multi-client network API usage example
 * @example avio_http_serve_files.c
 *
 * Serve a file without decoding or demuxing it over the HTTP protocol. Multiple
 * clients can connect and will receive the same file.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/avio_http_serve_files.c 的拷贝，只加了以“课程注释”开头的中文注释。
 * 拷贝的不是提交 a337e8d 的原版，而是打了 4 个 patch（0089 到 0092）的版本，改动过的地方下面标了编号：
 *     0089  resource 的类型从 uint8_t * 改成 char *，去掉 -Wpointer-sign 警告（strlen、strcmp 要 char *）
 *     0090  跟踪日志里用 %p 打印了字符串的地址，改成 %s 打印字符串
 *     0091  原版先回复 200、后打开文件：文件不存在时客户端收到“200 OK”加一个空的正文。现在先打开文件，再决定回复 404、500 还是 200
 *     0092  原版的注释自己承认“不回收子进程会产生僵尸进程”，现在 signal(SIGCHLD, SIG_IGN) 让系统回收
 * 另有两个 patch 改的是 libavformat，不是这个文件（没有它们，这个示例在原版 libavformat 上有两个问题）：
 *     0087  子进程里 avio_close(server) 永远不返回：关闭“监听用的上下文”时它往监听的 socket 写分块传输的结束标记，写不出去
 *     0088  父进程里 avio_close(client) 不关 socket：没做过握手的客户端被当成“没连上”，socket 一直泄漏，客户端那边的连接也不断开
 *     （打了 0087、0088 以后，这个示例才算一个能长期运行的服务器。README 第 7 步用原版和打补丁的两个 libavformat 对比过）
 * 这个示例演示：用 libavformat 自己的 HTTP 服务端能力，把一个文件发给多个客户端，一个字节也不解码、不解复用。
 * 数据是这样流动的：
 *     main：avio_open2(&server, "http://地址:端口", AVIO_FLAG_WRITE, NULL, {listen=2})
 *               只监听，还没有客户端；server 这个 AVIOContext 自己不收发数据，只负责 accept
 *     循环：avio_accept(server, &client)     阻塞到有 TCP 连接进来，client 是新的 AVIOContext，还没有读过请求
 *           fork()
 *     子进程：process_client(client, 文件名)
 *               avio_handshake(client) 读请求头，从选项 "resource" 里取出请求的路径
 *               设选项 "reply_code"（200 或 404……），再 avio_handshake(client) 发回复头
 *               文件 → avio_read → avio_write(client)
 *               avio_close(client)    发分块传输的结束标记，关连接
 *     父进程：avio_close(client)，回去继续 accept
 * 它是 libavformat 内置的 HTTP 服务器，能力很有限：只支持 GET（HEAD 会得到 400）、只会回复 200、400、403、404、429、500，
 * 没有 Range（206）：客户端不能从中间开始取。第 10 步会看到这对 moov 在文件尾的 MP4 意味着什么（回忆 L18）。
 * 本讲自己写的 avio_serve.c 在这个基础上换成了线程、加了目录列表和路径检查。
 */

#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <signal.h>
#include <unistd.h>

/*
 * 课程注释：处理一个客户端：握手（读请求、发回复头），再把文件发过去，最后关掉。在 fork 出来的子进程里调用。
 * client   avio_accept() 得到的 AVIOContext，还没有读过请求
 * in_uri   要发送的文件的名字；请求的路径必须正好是 "/" 加上这个名字，才算请求的是它
 */
static void process_client(AVIOContext *client, const char *in_uri)
{
    AVIOContext *input = NULL;
    uint8_t buf[1024];
    int ret, n, reply_code;
    char *resource = NULL;
    /*
     * 课程注释：avio_handshake() 每调一次往前走一步，返回值：> 0 还没完，< 0 出错，0 握手结束。
     * 对 HTTP 服务端，按顺序是三步：1. 底层协议（tcp）的握手；2. 读请求行和请求头，读完以后选项 "resource" 才有值；
     * 3. 把回复头写出去，回复头里用什么状态码由选项 "reply_code" 决定，默认 200。
     * 所以这里的做法是：循环调 avio_handshake() 直到第 2 步做完（resource 有值），停在第 3 步之前，
     * 自己看过请求的路径，再设 reply_code，再调 avio_handshake() 完成第 3 步。
     * 用 av_opt_get() 读选项：返回的字符串是新分配的，用完要 av_freep()。
     */
    while ((ret = avio_handshake(client)) > 0) {
        /* 课程注释：patch 0089。av_opt_get() 要的是 uint8_t **，但 strlen()、strcmp() 要 char *；原版 resource 是 uint8_t *。 */
        av_opt_get(client, "resource", AV_OPT_SEARCH_CHILDREN,
                   (uint8_t **)&resource);
        // check for strlen(resource) is necessary, because av_opt_get()
        // may return empty string.
        if (resource && strlen(resource))
            break;
        av_freep(&resource);
    }
    if (ret < 0)
        goto end;
    /*
     * 课程注释：patch 0090。原版是 "resource=%p"，打印的是指针的值。
     * 请求的路径原样放在 resource 里，没有解码百分号（"%20" 还是 "%20"），带查询串（"?a=1"）也原样带着：
     * 所以 /文件名?x=1 和 /文件名 对这个示例是两个不同的资源，前者得到 404。
     */
    av_log(client, AV_LOG_TRACE, "resource=%s\n", resource ? resource : "(none)");
    /*
     * 课程注释：patch 0091。原版在这里直接把 reply_code 设成 200，打开文件放在握手完成以后，这时“200 OK”已经发出去了。
     * 现在先打开文件：能打开才回复 200，文件不存在回复 404，别的原因（权限、I/O 错误）回复 500。
     * 能用的状态码只有 AVERROR_HTTP_BAD_REQUEST、FORBIDDEN、NOT_FOUND、TOO_MANY_REQUESTS、SERVER_ERROR（对应 400、403、404、429、500）和 200；
     * 别的值，avio_handshake() 会返回 AVERROR(EINVAL)。负的状态码回复里会带一个简短的文字正文并写明 Content-Length，
     * 200 的回复没有 Content-Length，用分块传输（Transfer-Encoding: chunked）。
     */
    reply_code = AVERROR_HTTP_NOT_FOUND;
    if (resource && resource[0] == '/' && !strcmp((resource + 1), in_uri)) {
        // The reply code is sent before any data, so open the input first
        // and report a file that cannot be opened instead of sending an
        // empty "200 OK" reply.
        fprintf(stderr, "Opening input file.\n");
        if ((ret = avio_open2(&input, in_uri, AVIO_FLAG_READ, NULL, NULL)) < 0) {
            av_log(NULL, AV_LOG_ERROR, "Failed to open input: %s: %s.\n",
                   in_uri, av_err2str(ret));
            reply_code = ret == AVERROR(ENOENT) ? AVERROR_HTTP_NOT_FOUND
                                                : AVERROR_HTTP_SERVER_ERROR;
        } else {
            reply_code = 200;
        }
    }
    /* 课程注释：设状态码。要在第 3 步（发回复头）之前设，设了以后下面的 avio_handshake() 才会用它。 */
    if ((ret = av_opt_set_int(client, "reply_code", reply_code, AV_OPT_SEARCH_CHILDREN)) < 0) {
        av_log(client, AV_LOG_ERROR, "Failed to set reply_code: %s.\n", av_err2str(ret));
        goto end;
    }
    av_log(client, AV_LOG_TRACE, "Set reply code to %d\n", reply_code);

    while ((ret = avio_handshake(client)) > 0);

    if (ret < 0)
        goto end;

    /* 课程注释：握手完成，回复头已经发出去了。不是 200 就到此为止：404 等的回复头和正文在握手那一步都已经写完。 */
    fprintf(stderr, "Handshake performed.\n");
    if (reply_code != 200)
        goto end;
    /*
     * 课程注释：把文件复制给客户端。每次读 1024 字节，写出去，马上 avio_flush()：每个 flush 都是一个分块（chunk）和一次 send()，
     * 对大文件很慢（每 1 KiB 一次系统调用），去掉 avio_flush() 让 AVIOContext 自己攒满缓冲区再发会快得多；
     * 留着它的好处是每一块马上就到客户端手里，对“直播”这类场景有意义。
     * avio_write()、avio_flush() 没有返回值：客户端中途断开，写会失败，但这里看不到，循环照样把文件读完才结束（练习 4）。
     */
    for(;;) {
        n = avio_read(input, buf, sizeof(buf));
        if (n < 0) {
            if (n == AVERROR_EOF)
                break;
            av_log(input, AV_LOG_ERROR, "Error reading from input: %s.\n",
                   av_err2str(n));
            break;
        }
        avio_write(client, buf, n);
        avio_flush(client);
    }
    /*
     * 课程注释：收尾。avio_close(client) 对 200 的回复会再写一个“长度为 0 的块”（"0\r\n\r\n"），客户端靠它知道正文结束了，
     * 所以关闭之前一定要走完握手（否则 libavformat 不知道回复头已经发过，见 patch 0088）。
     * avio_close(input)：input 在 404、500 的情况下是 NULL，avio_close(NULL) 是允许的，返回 0。
     */
end:
    fprintf(stderr, "Flushing client\n");
    avio_flush(client);
    fprintf(stderr, "Closing client\n");
    avio_close(client);
    fprintf(stderr, "Closing input\n");
    avio_close(input);
    av_freep(&resource);
}

/*
 * 课程注释：main()：打开监听，循环接受客户端，每个客户端 fork 一个子进程去处理。
 * 命令行：avio_http_serve_files 文件名 http://地址[:端口]
 *     地址决定监听在哪块网卡上：127.0.0.1 只有本机能连，0.0.0.0 所有网卡；端口不写是 80，低于 1024 要 root。
 *     URL 里 "/" 之后的内容在这里没有用，客户端请求的路径是 "/" 加文件名。
 */
int main(int argc, char **argv)
{
    AVDictionary *options = NULL;
    AVIOContext *client = NULL, *server = NULL;
    const char *in_uri, *out_uri;
    int ret, pid;
    /* 课程注释：日志级别 TRACE 是最啰嗦的一级，请求头、回复头、每一步握手都会打印，学习时有用，真正跑服务时要调低。 */
    av_log_set_level(AV_LOG_TRACE);
    if (argc < 3) {
        printf("usage: %s input http://hostname[:port]\n"
               "API example program to serve http to multiple clients.\n"
               "\n", argv[0]);
        return 1;
    }

    in_uri = argv[1];
    out_uri = argv[2];

    avformat_network_init();

    /* 课程注释：patch 0092，见下面的注释。信号处理放在 fork() 之前，子进程继承这个设置。 */
    // The children are never waited for. Ignoring SIGCHLD makes the system
    // reap them when they exit, so that they do not stay behind as zombies.
    signal(SIGCHLD, SIG_IGN);

    /*
     * 课程注释：listen 选项：0 是客户端（默认），1 是“只接受一个客户端”（avio_open2() 自己阻塞到有客户端连上并完成握手，
     * 之后这个 AVIOContext 就是和那个客户端的连接），2 是“接受多个客户端”：avio_open2() 马上返回，之后用 avio_accept() 一个一个接。
     * 源码里 2 的宏名叫 HTTP_MUTLI（Multi 拼错了），不影响使用。
     */
    if ((ret = av_dict_set(&options, "listen", "2", 0)) < 0) {
        fprintf(stderr, "Failed to set listen mode for server: %s\n", av_err2str(ret));
        return ret;
    }
    /*
     * 课程注释：监听。flags 是 AVIO_FLAG_WRITE，因为服务端对外“写”（发文件）；端口被占用、没有权限会在这里失败，
     * 返回 AVERROR(EADDRINUSE)、AVERROR(EACCES) 一类。options 里的 listen 在 avio_open2() 里被用掉。
     * 注意这里 return ret（负数），进程的退出码是它的低 8 位，不是 1。
     */
    if ((ret = avio_open2(&server, out_uri, AVIO_FLAG_WRITE, NULL, &options)) < 0) {
        fprintf(stderr, "Failed to open server: %s\n", av_err2str(ret));
        return ret;
    }
    fprintf(stderr, "Entering main loop.\n");
    for(;;) {
        /* 课程注释：阻塞到有客户端连进来。client 是新的 AVIOContext，每个客户端一个，和 server 互不相干。 */
        if ((ret = avio_accept(server, &client)) < 0)
            goto end;
        fprintf(stderr, "Accepted client, forking process.\n");
        /*
         * 课程注释：每个客户端一个进程：一个客户端慢，不影响别的客户端。子进程是父进程的副本，
         * 所以子进程里也有一份 server（监听的 socket）和 client；父进程里也还有一份 client（连接的 socket）。
         * 因此两边都要关掉“用不着的那一份”：子进程关 server（下面），父进程关 client（再下面）。
         */
        pid = fork();
        if (pid < 0) {
            perror("Fork failed");
            ret = AVERROR(errno);
            goto end;
        }
        if (pid == 0) {
            fprintf(stderr, "In child.\n");
            process_client(client, in_uri);
            /* 课程注释：patch 0087 以前，原版 libavformat 在这里永远不返回，每个客户端留下一个卡住的子进程。 */
            avio_close(server);
            exit(0);
        }
        /* 课程注释：patch 0088 以前，原版 libavformat 在这里不真正关掉 socket：父进程每接一个客户端就多一个泄漏的文件描述符。 */
        if (pid > 0)
            avio_close(client);
    }
    /*
     * 课程注释：只有 avio_accept() 或 fork() 失败才会走到这里。main() 里没有任何正常退出的路径，
     * 服务器一直跑到被 Ctrl+C 或者 kill 为止。
     */
end:
    avio_close(server);
    if (ret < 0 && ret != AVERROR_EOF) {
        fprintf(stderr, "Some errors occurred: %s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}
