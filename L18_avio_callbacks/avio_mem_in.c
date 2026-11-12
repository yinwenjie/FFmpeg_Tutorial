/*
 * avio_mem_in.c —— 课程 L18：自己的“自定义输入”程序
 *
 * 官方示例 avio_read_callback.c 的加强版。数据不是 libavformat 自己打开的文件，
 * 而是由我们提供读回调（和可选的 seek 回调）。可以用来观察：
 *   - libavformat 到底调了几次读回调、几次 seek 回调、每次要多少字节；
 *   - 没有 seek 回调时，哪些格式读不全（MP4 的 moov 在文件尾、TS 没有时长）；
 *   - 数据来自标准输入（管道，读不回头）和来自内存有什么区别；
 *   - 在读回调里“解密”：加密的文件交给 libavformat，它看到的是解密以后的数据。
 *
 * 用法：
 *   avio_mem_in <文件名|-> [名字=值 ...]
 *
 *   文件名是 - 时从标准输入读：管道里的数据读不回头；用 < 重定向文件的话还能 seek。
 *
 *   seek=0|1      是否提供 seek 回调，默认 1（标准输入是管道时自动没有）
 *   bufsize=N     AVIOContext 的缓冲区大小，默认 4096（和官方示例一样）
 *   xor=KEY       读回调里用 KEY 逐字节异或，当作“解密”；KEY 是一个字符串，循环使用
 *   trace=N       把前 N 次回调（读、seek、查询大小）打印出来，默认 0
 *   packets=0|1   把所有的包读完，按流统计个数和字节数，默认 1
 *   其他 名字=值  交给解复用器，比如 probesize=100000 analyzeduration=0
 *
 * 退出码：0 正常；1 出错；2 能打开，但有的流参数不完整（缺像素格式、采样率等）。
 */

/* -std=c11 下要这一行，才能用 POSIX 的 read、lseek 和 off_t */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/file.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/samplefmt.h>

typedef struct Source {
    uint8_t *data;          /* 内存模式：整个文件 */
    size_t   size;          /* 总大小；标准输入是管道时为 0（不知道） */
    int      fd;            /* 标准输入模式：文件描述符，内存模式为 -1 */
    int64_t  pos;           /* 当前读位置 */
    int      size_known;

    const char *key;        /* 异或的密钥，NULL 表示不“解密” */
    size_t   keylen;

    int      trace;         /* 还能打印几次回调 */
    int64_t  reads, read_bytes, seeks, size_queries, calls;
} Source;

/* 打印一次回调的情况，只打印前 trace 次，一个编号对应一次回调。 */
static void trace_call(Source *s, const char *fmt, ...)
{
    va_list ap;

    s->calls++;
    if (s->trace <= 0)
        return;
    s->trace--;
    printf("  回调 #%-3lld ", (long long)s->calls);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

/*
 * 读回调：AVIOContext 的缓冲区空了，向我们要数据。
 * 返回读到的字节数（可以比 buf_size 少）；读到末尾返回 AVERROR_EOF；出错返回负的错误码。
 * 不能返回 0：libavformat 把它当作无效的返回值。
 */
static int read_cb(void *opaque, uint8_t *buf, int buf_size)
{
    Source *s = opaque;
    int n;

    if (s->fd >= 0) {
        ssize_t r = read(s->fd, buf, buf_size);
        if (r < 0)
            return AVERROR(errno);
        n = (int)r;
    } else {
        size_t left = s->size - s->pos;
        n = (int)FFMIN((size_t)buf_size, left);
        /*
         * 空文件的数据指针是 NULL，n 是 0：memcpy 的指针参数按标准不能是 NULL，哪怕长度是 0。
         * L22 的 ASan/UBSan 扫描在这里报过 “null pointer passed as argument 2”，所以先判断长度。
         */
        if (n > 0)
            memcpy(buf, s->data + s->pos, n);
    }

    if (n == 0) {
        trace_call(s, "读   位置 %lld，要 %d 字节 → 没有数据了，返回 AVERROR_EOF",
                   (long long)s->pos, buf_size);
        return AVERROR_EOF;
    }

    /* “解密”：密钥按文件里的位置循环用，所以 seek 以后位置对得上才解得对。 */
    if (s->key) {
        for (int i = 0; i < n; i++)
            buf[i] ^= (uint8_t)s->key[(s->pos + i) % s->keylen];
    }

    trace_call(s, "读   位置 %lld，要 %d 字节 → 给 %d 字节",
               (long long)s->pos, buf_size, n);
    s->reads++;
    s->read_bytes += n;
    s->pos += n;
    return n;
}

/*
 * seek 回调：libavformat 要换一个位置读。
 * whence 是 SEEK_SET、SEEK_CUR、SEEK_END 之一，可能带 AVSEEK_FORCE；
 * 还有一个 libavformat 自己加的 AVSEEK_SIZE：不移动，只问整个数据有多大。
 * 返回新的位置；不支持或者出错返回负数。
 */
static int64_t seek_cb(void *opaque, int64_t offset, int whence)
{
    Source *s = opaque;
    int64_t target;

    if (whence == AVSEEK_SIZE) {
        s->size_queries++;
        trace_call(s, "问大小 → %s", s->size_known ? "知道" : "不知道");
        return s->size_known ? (int64_t)s->size : AVERROR(ENOSYS);
    }

    whence &= ~AVSEEK_FORCE;
    if (s->fd >= 0) {
        off_t r = lseek(s->fd, offset, whence);
        if (r < 0)
            return AVERROR(errno);
        target = r;
    } else {
        switch (whence) {
        case SEEK_SET: target = offset;                 break;
        case SEEK_CUR: target = s->pos + offset;        break;
        case SEEK_END: target = (int64_t)s->size + offset; break;
        default:       return AVERROR(EINVAL);
        }
        if (target < 0 || target > (int64_t)s->size)
            return AVERROR(EINVAL);
    }

    s->seeks++;
    trace_call(s, "seek 位置 %lld → %lld（%s %lld）",
               (long long)s->pos, (long long)target,
               whence == SEEK_SET ? "从头" : whence == SEEK_CUR ? "相对当前" : "相对末尾",
               (long long)offset);
    s->pos = target;
    return target;
}

typedef struct Options {
    int         seek;       /* 提供 seek 回调 */
    int         bufsize;
    const char *xor_key;
    int         trace;
    int         packets;
    AVDictionary *demux;    /* 其余的交给解复用器 */
} Options;

static int parse_args(int argc, char **argv, Options *o)
{
    o->seek    = 1;
    o->bufsize = 4096;
    o->packets = 1;

    for (int i = 2; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            fprintf(stderr, "参数要写成 名字=值：%s\n", argv[i]);
            return AVERROR(EINVAL);
        }
        *eq = '\0';
        const char *name = argv[i], *val = eq + 1;
        if (!strcmp(name, "seek"))
            o->seek = atoi(val);
        else if (!strcmp(name, "bufsize"))
            o->bufsize = atoi(val);
        else if (!strcmp(name, "xor"))
            o->xor_key = val;
        else if (!strcmp(name, "trace"))
            o->trace = atoi(val);
        else if (!strcmp(name, "packets"))
            o->packets = atoi(val);
        else
            av_dict_set(&o->demux, name, val, 0);
    }
    if (o->bufsize < 16) {
        fprintf(stderr, "bufsize= 的值 %d 太小，至少 16\n", o->bufsize);
        return AVERROR(EINVAL);
    }
    if (o->xor_key && !*o->xor_key) {
        fprintf(stderr, "xor= 的密钥不能为空\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

/* 流的参数是否完整：视频要有像素格式和尺寸，音频要有采样格式、采样率和声道。 */
static int params_complete(const AVCodecParameters *p)
{
    switch (p->codec_type) {
    case AVMEDIA_TYPE_VIDEO:
        return p->format != AV_PIX_FMT_NONE && p->width > 0 && p->height > 0;
    case AVMEDIA_TYPE_AUDIO:
        return p->format != AV_SAMPLE_FMT_NONE && p->sample_rate > 0 &&
               p->ch_layout.nb_channels > 0;
    default:
        return 1;
    }
}

static void describe_stream(const AVStream *st, int complete)
{
    const AVCodecParameters *p = st->codecpar;
    char layout[64];

    printf("  流 #%d：%s %s", st->index, av_get_media_type_string(p->codec_type),
           avcodec_get_name(p->codec_id));
    if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
        printf("，%dx%d，像素格式 %s", p->width, p->height,
               p->format == AV_PIX_FMT_NONE ? "未知"
                                            : av_get_pix_fmt_name(p->format));
    } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
        av_channel_layout_describe(&p->ch_layout, layout, sizeof(layout));
        printf("，%d Hz，%s，采样格式 %s", p->sample_rate, layout,
               p->format == AV_SAMPLE_FMT_NONE ? "未知"
                                               : av_get_sample_fmt_name(p->format));
    }
    printf("%s\n", complete ? "" : "    ← 参数不完整");
}

/* 把所有的包读完，按流统计。 */
static int count_packets(AVFormatContext *fmt)
{
    AVPacket *pkt = av_packet_alloc();
    int64_t *count = av_calloc(fmt->nb_streams, sizeof(*count));
    int64_t *bytes = av_calloc(fmt->nb_streams, sizeof(*bytes));
    int ret = 0;

    if (!pkt || !count || !bytes) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    while ((ret = av_read_frame(fmt, pkt)) >= 0) {
        count[pkt->stream_index]++;
        bytes[pkt->stream_index] += pkt->size;
        av_packet_unref(pkt);
    }
    if (ret == AVERROR_EOF)
        ret = 0;
    else
        fprintf(stderr, "读包失败：%s\n", av_err2str(ret));

    for (unsigned i = 0; i < fmt->nb_streams; i++)
        printf("  流 #%u：%lld 个包，%lld 字节\n", i,
               (long long)count[i], (long long)bytes[i]);
end:
    av_free(count);
    av_free(bytes);
    av_packet_free(&pkt);
    return ret;
}

int main(int argc, char **argv)
{
    Source src = { .fd = -1 };
    Options opt = { 0 };
    AVFormatContext *fmt = NULL;
    AVIOContext *avio = NULL;
    uint8_t *iobuf = NULL;
    int ret, complete = 1, use_seek;
    const char *input;
    const char *from;

    if (argc < 2) {
        fprintf(stderr, "用法：%s <文件名|-> [seek=0|1] [bufsize=N] [xor=KEY] "
                "[trace=N] [packets=0|1] [名字=值 ...]\n", argv[0]);
        return 1;
    }
    input = argv[1];
    /* 回调的记录用 printf，libavformat 的警告用 stderr；按行输出，两边的顺序才对得上 */
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((ret = parse_args(argc, argv, &opt)) < 0)
        goto end;

    src.trace = opt.trace;
    if (opt.xor_key) {
        src.key    = opt.xor_key;
        src.keylen = strlen(opt.xor_key);
    }

    /* ---- 准备数据来源 ---- */
    use_seek = opt.seek;
    if (!strcmp(input, "-")) {
        src.fd = 0;
        off_t cur = lseek(src.fd, 0, SEEK_CUR);
        if (cur < 0) {
            /* 管道：读不回头，也不知道多大，就算要求了 seek 也没有 */
            from = "标准输入（管道）";
            use_seek = 0;
        } else {
            off_t end = lseek(src.fd, 0, SEEK_END);
            lseek(src.fd, cur, SEEK_SET);
            src.pos        = cur;
            src.size       = end;
            src.size_known = 1;
            from = "标准输入（文件重定向）";
        }
    } else {
        ret = av_file_map(input, &src.data, &src.size, 0, NULL);
        if (ret < 0) {
            fprintf(stderr, "读不了输入文件 %s：%s\n", input, av_err2str(ret));
            goto end;
        }
        src.size_known = 1;
        from = "内存";
    }

    /* ---- 建 AVIOContext，交给解复用器 ---- */
    if (!(fmt = avformat_alloc_context()) ||
        !(iobuf = av_malloc(opt.bufsize))) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    avio = avio_alloc_context(iobuf, opt.bufsize, 0, &src, read_cb, NULL,
                              use_seek ? seek_cb : NULL);
    if (!avio) {
        av_freep(&iobuf);
        ret = AVERROR(ENOMEM);
        goto end;
    }
    fmt->pb = avio;

    printf("来源：%s，%s，缓冲区 %d 字节，seek 回调 %s%s\n", from,
           src.size_known ? "大小已知" : "大小未知", opt.bufsize,
           use_seek ? "有" : "没有", src.key ? "，读回调里异或“解密”" : "");
    if (src.size_known)
        printf("数据大小：%zu 字节\n", src.size);

    if (opt.trace)
        printf("回调记录（前 %d 次）：\n", opt.trace);
    ret = avformat_open_input(&fmt, NULL, NULL, &opt.demux);
    if (ret < 0) {
        fprintf(stderr, "打不开输入：%s\n", av_err2str(ret));
        goto end;
    }
    ret = avformat_find_stream_info(fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "找不到流信息：%s\n", av_err2str(ret));
        goto end;
    }

    /* ---- 看结果 ---- */
    printf("格式：%s，%u 路流", fmt->iformat->name, fmt->nb_streams);
    if (fmt->duration != AV_NOPTS_VALUE)
        printf("，时长 %.3f 秒", fmt->duration / (double)AV_TIME_BASE);
    else
        printf("，时长未知");
    if (fmt->bit_rate > 0)
        printf("，码率 %lld kbit/s", (long long)(fmt->bit_rate / 1000));
    printf("\n");
    for (unsigned i = 0; i < fmt->nb_streams; i++) {
        int ok = params_complete(fmt->streams[i]->codecpar);
        complete &= ok;
        describe_stream(fmt->streams[i], ok);
    }
    printf("流参数%s\n", complete ? "完整" : "不完整");
    printf("回调统计：读 %lld 次共 %lld 字节，seek %lld 次，问大小 %lld 次\n",
           (long long)src.reads, (long long)src.read_bytes,
           (long long)src.seeks, (long long)src.size_queries);

    if (opt.packets) {
        int64_t reads0 = src.reads;
        printf("读完所有的包：\n");
        ret = count_packets(fmt);
        printf("读包又用了 %lld 次读回调，现在共读 %lld 字节，seek %lld 次\n",
               (long long)(src.reads - reads0), (long long)src.read_bytes,
               (long long)src.seeks);
        if (ret < 0)
            goto end;
    }

end:
    avformat_close_input(&fmt);
    /* libavformat 可能把缓冲区换成了别的，要释放 avio->buffer 而不是我们自己的 iobuf */
    if (avio)
        av_freep(&avio->buffer);
    avio_context_free(&avio);
    av_dict_free(&opt.demux);
    if (src.data)
        av_file_unmap(src.data, src.size);

    if (ret < 0) {
        fprintf(stderr, "出错：%s\n", av_err2str(ret));
        return 1;
    }
    return complete ? 0 : 2;
}
