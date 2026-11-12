/*
 * Copyright (c) 2011 Reinhard Tartler
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
 * @file libavformat metadata extraction API usage example
 * @example show_metadata.c
 *
 * Show metadata from an input file.
 */

/*
 * 课程说明：这是 FFmpeg doc/examples/show_metadata.c（提交 a337e8d）的原样拷贝，
 * 代码一行未改，只加了中文注释。它是官方示例里最短的一个，
 * 展示了使用 libavformat 读文件的最小骨架：打开、探测、读取信息、关闭。
 */

#include <stdio.h>

#include <libavformat/avformat.h>
#include <libavutil/dict.h>

int main (int argc, char **argv)
{
    /* 必须初始化为 NULL：avformat_open_input() 看到 NULL 才会替我们分配。 */
    AVFormatContext *fmt_ctx = NULL;
    const AVDictionaryEntry *tag = NULL;
    int ret;

    if (argc != 2) {
        printf("usage: %s <input_file>\n"
               "example program to demonstrate the use of the libavformat metadata API.\n"
               "\n", argv[0]);
        return 1;
    }

    /*
     * 第 1 步：打开输入，识别容器格式，读文件头。
     * 后两个参数分别是“强制指定格式”和“选项字典”，传 NULL 表示自动识别、不带选项。
     * 成功返回 0，失败返回负的错误码，并且 fmt_ctx 保持为 NULL。
     *
     * 注意官方示例在这里失败时直接 return，没有打印任何信息（patch 0009）。
     * 返回的是负的错误码，进程的退出码是它的低 8 位：打不开文件是 254，读不出流信息是 183，都不是约定的 1。
     * 课程里的 media_info.c 演示了更合适的写法。
     */
    if ((ret = avformat_open_input(&fmt_ctx, argv[1], NULL, NULL)))
        return ret;

    /*
     * 第 2 步：再读一小段数据，把文件头里没有写的流参数补齐。
     * 只读元数据的话，MP4 这类格式其实不需要这一步；
     * 但 MPEG-TS、裸流等格式离开它就拿不到完整的流信息，所以习惯上总是调用。
     */
    if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot find stream information\n");
        return ret;
    }

    /*
     * 第 3 步：遍历容器级别的元数据。
     * AVDictionary 是一组“字符串键 → 字符串值”；
     * av_dict_iterate() 每次返回下一项，第二个参数传上一次的返回值，传 NULL 表示从头开始。
     * 这里只打印了 fmt_ctx->metadata，每一路流自己的元数据在 fmt_ctx->streams[i]->metadata。
     */
    while ((tag = av_dict_iterate(fmt_ctx->metadata, tag)))
        printf("%s=%s\n", tag->key, tag->value);

    /* 第 4 步：关闭输入，释放所有资源，并把 fmt_ctx 置回 NULL。 */
    avformat_close_input(&fmt_ctx);
    return 0;
}
