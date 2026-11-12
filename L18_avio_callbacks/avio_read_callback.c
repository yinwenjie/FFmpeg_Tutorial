/*
 * Copyright (c) 2014 Stefano Sabatini
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
 * @file libavformat AVIOContext read callback API usage example
 * @example avio_read_callback.c
 *
 * Make libavformat demuxer access media content through a custom
 * AVIOContext read callback, with a seek callback so that formats that
 * need to jump around in the data (an MP4 file with the moov atom at
 * the end, for example) can be demuxed as well.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/avio_read_callback.c 的拷贝，只加了以“课程注释”开头的中文注释。
 * 拷贝的不是提交 a337e8d 的原版，而是打了 2 个 patch（0082、0083）的版本，改动过的地方下面都标了编号：
 *     0082  read_packet() 里拿 int 和 size_t 比大小，-Wall -Wextra 下有 -Wsign-compare 警告，加了强制转换
 *     0083  原版没有 seek 回调（avio_alloc_context 的最后一个参数是 NULL），moov 在文件尾的 MP4 读不全：
 *           像素格式、码率都是未知。加了 seek_buffer() 和 buffer_data 里的 start、total
 * 这个示例演示：libavformat 不从文件、网络读，而从“我们自己提供的数据”里读。整个文件先读进内存，
 * 再由读回调一块一块交给 libavformat。数据是这样流动的，AVIOContext 夹在中间，所有的读都经过它的缓冲区：
 *     解复用器（mov、mpegts、matroska ……）
 *         │  avio_read()、avio_r8()、avio_seek() ……
 *         ▼
 *     AVIOContext：缓冲区 avio_ctx_buffer，这里 4096 字节
 *         │  缓冲区空了      → 调 read_packet(opaque, buf, buf_size)
 *         │  要换一个读的位置 → 调 seek_buffer(opaque, offset, whence)
 *         ▼
 *     我们的数据：内存里的整个文件，struct buffer_data 记着读到哪里了
 * 本讲还有两个自己写的程序：avio_mem_in.c（自定义输入：打印每一次回调，可以不提供 seek 回调，可以在读回调里“解密”）
 * 和 avio_mem_out.c（自定义输出：写回调、写进内存、分片 MP4）。
 */

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/file.h>
#include <libavutil/mem.h>

/*
 * 课程注释：交给回调的“上下文”，通过 avio_alloc_context() 的 opaque 参数传进去，每次回调都原样收到。
 * ptr 是下一次从哪里读，size 是后面还剩多少字节，这两个原版就有。
 * patch 0083 加了 start（数据的开头）和 total（整个大小）：seek 要从头算位置，还要回答“一共多大”。
 */
struct buffer_data {
    uint8_t *start;  ///< start of the buffer
    uint8_t *ptr;    ///< current position in the buffer
    size_t size;     ///< size left in the buffer
    size_t total;    ///< size of the whole buffer
};

/*
 * 课程注释：读回调。AVIOContext 的缓冲区空了，就调它要数据。
 *     opaque    avio_alloc_context() 传进去的指针，这里是 &bd，回调靠它找到“我们的数据”
 *     buf       数据放在这里
 *     buf_size  最多放这么多。给得比 buf_size 少是允许的，下面的 FFMIN 就是这样
 * 返回值：读到的字节数；数据没有了返回 AVERROR_EOF；出错返回别的负的错误码。
 * 不能在读到末尾时返回 0：实测 MP4、TS 碰巧没事，MKV、FLV、裸 MPEG-1 视频读到文件尾以后程序不再返回（avio_mem_in.c 里试过）。
 */
static int read_packet(void *opaque, uint8_t *buf, int buf_size)
{
    struct buffer_data *bd = (struct buffer_data *)opaque;
    /*
     * 课程注释：patch 0082。原版是 FFMIN(buf_size, bd->size)，int 和 size_t 比较，编译器警告 -Wsign-compare。
     * 这里 buf_size 不会是负数，先转成 size_t 再比；比较的结果不大于 buf_size，所以再赋给 int 是安全的。
     */
    buf_size = FFMIN((size_t)buf_size, bd->size);

    if (!buf_size)
        return AVERROR_EOF;
    /*
     * 课程注释：每次回调打印一行：这一次从哪里读、后面还剩多少字节，用来观察 libavformat 是怎么读的。
     * 注意它走 stdout，libavformat 的日志走 stderr：输出到管道或文件时 stdout 是整块缓冲的，两边的输出会错开。
     * 想看清楚就分开重定向：./avio_read_callback x.mp4 2>err.txt。
     */
    printf("ptr:%p size:%zu\n", bd->ptr, bd->size);

    /*
     * 课程注释：把数据复制进 libavformat 给的缓冲区 buf，再把读位置往后挪。
     * 复制是必须的：数据一定要放进 buf，不能让它指向别处。读到哪里了记在 bd 里，下一次回调接着往后读。
     */
    /* copy internal buffer data to buf */
    memcpy(buf, bd->ptr, buf_size);
    bd->ptr  += buf_size;
    bd->size -= buf_size;

    return buf_size;
}

/*
 * 课程注释：patch 0083。seek 回调：libavformat 要换一个位置读数据。
 *     whence  SEEK_SET（从头算）、SEEK_CUR（从当前位置算）、SEEK_END（从末尾算）三选一，
 *             可能再或上 AVSEEK_FORCE（“一定要 seek”，这里可以忽略，所以下面先把它去掉）
 *     还有 libavformat 自己加的 AVSEEK_SIZE：不换位置，只问“整个数据一共多大”，返回大小；不知道就返回负数
 * 返回值：换位置以后的新位置（从头算的字节数）；不支持或者位置不对返回负的错误码。
 * 没有这个回调（avio_alloc_context 的最后一个参数传 NULL），这个 AVIOContext 就是“不可 seek 的”。
 * MP4 的 moov 在文件尾时差别很大：没有 seek 回调，138 次读回调读完整个文件，视频流的像素格式是 none；
 * 有 seek 回调，6 次读回调，像素格式 yuv420p（avio_mem_in.c 里试过）。
 */
static int64_t seek_buffer(void *opaque, int64_t offset, int whence)
{
    struct buffer_data *bd = (struct buffer_data *)opaque;
    int64_t pos;

    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE:
        /* libavformat asks for the size of the data without moving */
        return bd->total;
    case SEEK_SET:
        pos = offset;
        break;
    case SEEK_CUR:
        pos = (bd->ptr - bd->start) + offset;
        break;
    case SEEK_END:
        pos = bd->total + offset;
        break;
    default:
        return AVERROR(EINVAL);
    }

    if (pos < 0 || pos > (int64_t)bd->total)
        return AVERROR(EINVAL);

    bd->ptr  = bd->start + pos;
    bd->size = bd->total - pos;

    return pos;
}

/*
 * 课程注释：main() 的结构：
 *     1. av_file_map() 把文件映射进内存，这块内存就是“我们自己的数据”
 *     2. 填 buffer_data，让它指向这块内存
 *     3. av_malloc() 一块缓冲区，avio_alloc_context() 把缓冲区、读回调、seek 回调包成一个 AVIOContext
 *     4. fmt_ctx->pb = avio_ctx，avformat_open_input() 不给文件名
 *     5. 照常 avformat_find_stream_info()、av_dump_format()
 *     6. 收尾，顺序有讲究（见 end:）
 */
int main(int argc, char *argv[])
{
    AVFormatContext *fmt_ctx = NULL;
    AVIOContext *avio_ctx = NULL;
    uint8_t *buffer = NULL, *avio_ctx_buffer = NULL;
    size_t buffer_size, avio_ctx_buffer_size = 4096;
    char *input_filename = NULL;
    int ret = 0;
    struct buffer_data bd = { 0 };

    if (argc != 2) {
        fprintf(stderr, "usage: %s input_file\n"
                "API example program to show how to read from a custom buffer "
                "accessed through AVIOContext.\n", argv[0]);
        return 1;
    }
    input_filename = argv[1];

    /*
     * 课程注释：av_file_map() 把文件映射进内存（Linux 上是只读的 mmap），buffer 是起始地址，buffer_size 是大小。
     * 这里数据其实来自一个文件，只是为了演示。真实的用法是数据来自别处：网络上收到的一块、读出来再解密的、
     * 嵌在别的文件里的一段、程序自己生成的。
     */
    /* slurp file content into buffer */
    ret = av_file_map(input_filename, &buffer, &buffer_size, 0, NULL);
    if (ret < 0)
        goto end;

    /* fill opaque structure used by the AVIOContext read callback */
    bd.start = bd.ptr = buffer;
    bd.size  = bd.total = buffer_size;

    if (!(fmt_ctx = avformat_alloc_context())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    /*
     * 课程注释：AVIOContext 自己用的缓冲区，必须用 av_malloc() 分配（不能是栈上的数组，也不能是 malloc() 的）。
     * libavformat 可能在运行中把它换成别的缓冲区，再 av_free() 掉旧的，所以结尾要释放 avio_ctx->buffer，不是这个变量。
     * 4096 是这个示例随手选的。缓冲区大，回调的次数就少，但是探测时多读进来的字节也多
     * （avio_mem_in.c 的 bufsize= 可以试：MP4 文件 16 字节时 262 次读，4096 字节时 6 次，262144 字节时 3 次）。
     */
    avio_ctx_buffer = av_malloc(avio_ctx_buffer_size);
    if (!avio_ctx_buffer) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    /*
     * 课程注释：avio_alloc_context(buffer, buffer_size, write_flag, opaque, read_packet, write_packet, seek)
     *     buffer、buffer_size  上面分配的缓冲区
     *     write_flag           0 表示这个 AVIOContext 用来读，1 表示用来写（avio_mem_out.c 用 1）
     *     opaque               原样作为三个回调的第一个参数，回调靠它找到“我们的数据”，这里是 &bd
     *     read_packet          读回调；用来写的时候传 NULL
     *     write_packet         写回调；用来读的时候传 NULL，这里就是 NULL
     *     seek                 seek 回调；可以传 NULL，表示“不可 seek”。patch 0083 之前的原版传的就是 NULL
     * 读和写是两种 AVIOContext，不能在同一个里又读又写。
     */
    avio_ctx = avio_alloc_context(avio_ctx_buffer, avio_ctx_buffer_size,
                                  0, &bd, &read_packet, NULL, &seek_buffer);
    if (!avio_ctx) {
        av_freep(&avio_ctx_buffer);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    /*
     * 课程注释：把自己的 AVIOContext 挂到 AVFormatContext 的 pb 上。avformat_open_input() 看到 pb 已经有了，
     * 就不会自己去打开文件，同时自动设上 AVFMT_FLAG_CUSTOM_IO，意思是“这个 pb 是用户的”：
     * avformat_close_input() 不会释放它，所以下面 end: 里要我们自己释放。
     */
    fmt_ctx->pb = avio_ctx;

    /*
     * 课程注释：url 传 NULL，因为没有什么要它去打开的。后两个 NULL 是输入格式和选项，都用默认：
     * 格式靠“探测”（读开头的几千字节，各个解复用器打分，分最高的赢，L03）。
     * 没有文件名，也就没有扩展名可以参考，只靠内容；探测用的数据同样是通过读回调拿到的。
     */
    ret = avformat_open_input(&fmt_ctx, NULL, NULL, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not open input\n");
        goto end;
    }

    /*
     * 课程注释：读一些包、试着解码，把各路流的参数补全（像素格式、码率、时长等，L03）。这一步读的字节最多，也最依赖 seek。
     * MP4 的 moov 在文件尾时，没有 seek 回调，只能一路读到文件尾才拿到 moov（实测读了整个文件），
     * 而读过去的 mdat 回不来了：日志里出现 “partial file”、“unspecified pixel format”。注意 find_stream_info 在这种情况下仍然返回 0。
     */
    ret = avformat_find_stream_info(fmt_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "Could not find stream information\n");
        goto end;
    }

    /*
     * 课程注释：把结果打印到 stderr。第 4 个参数 0 表示这是输入。input_filename 只用来显示（“from '…'”），
     * fmt_ctx 并没有真的打开过这个文件名。
     */
    av_dump_format(fmt_ctx, 0, input_filename, 0);

/*
 * 课程注释：收尾，顺序有讲究：
 *     1. avformat_close_input()：fmt_ctx->pb 是我们自己设的，它只关解复用器，不释放 pb
 *     2. 释放 avio_ctx->buffer，不是 avio_ctx_buffer：libavformat 可能已经把缓冲区换掉了，旧的那块它自己释放过了，再释放就重复了
 *     3. avio_context_free()：释放 AVIOContext 本身，不会管缓冲区。前面的 if (avio_ctx) 是因为 avio_alloc_context() 可能失败
 *     4. 最后才 av_file_unmap()：数据是回调的 opaque 指向的，AVIOContext 还在用的时候不能先释放
 */
end:
    avformat_close_input(&fmt_ctx);

    /* note: the internal buffer could have changed, and be != avio_ctx_buffer */
    if (avio_ctx)
        av_freep(&avio_ctx->buffer);
    avio_context_free(&avio_ctx);

    av_file_unmap(buffer, buffer_size);

    /*
     * 课程注释：出错返回 1。注意：MP4 读不全（流的参数不完整）这种情况，上面所有的调用都返回 0，这里也不会报错，
     * 程序照样返回 0。只有 av_dump_format 的输出里看得出来。自己的程序要检查 codecpar（avio_mem_in.c 就这样做，不完整时退出码是 2）。
     */
    if (ret < 0) {
        fprintf(stderr, "Error occurred: %s\n", av_err2str(ret));
        return 1;
    }

    return 0;
}
