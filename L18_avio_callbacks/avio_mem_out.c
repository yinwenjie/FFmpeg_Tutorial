/*
 * avio_mem_out.c —— 课程 L18：自己的“自定义输出”程序
 *
 * 把输入文件的包原样复制（不解码、不编码，和 L10 的 remux 一样）到一个输出容器里，
 * 但输出可以不写文件，而是写进内存。可以用来观察：
 *   - 写回调被调了几次、每次几个字节；
 *   - 没有 seek 回调时，哪些容器写不出来（MP4 要回头补 moov/mdat 的大小）；
 *   - 用 movflags=frag_keyframe+empty_moov 这样的“分片 MP4”，不回头也能写；
 *   - libavformat 自带的内存输出 avio_open_dyn_buf，和自己写回调有什么区别。
 *
 * 用法：
 *   avio_mem_out <输入文件> <输出格式名> <输出文件> [名字=值 ...]
 *
 *   输出格式名是 libavformat 的名字：mp4、matroska、mpegts、flv、adts ……
 *   写出来的结果，最后统一存进 <输出文件>，方便用 ffprobe 检查、用 cmp 比较。
 *
 *   mode=file|cb|dyn  输出方式，默认 cb
 *                       file  libavformat 自己打开文件（对照用）
 *                       cb    自己的写回调，数据先攒在内存里
 *                       dyn   libavformat 自带的内存输出 avio_open_dyn_buf
 *   seek=0|1          cb 模式下是否提供 seek 回调，默认 1
 *   bufsize=N         AVIOContext 的缓冲区大小，默认 4096
 *   trace=N           把前 N 次回调（写、seek）打印出来，默认 0
 *   dynend=0|1        仅 mode=dyn：关闭之前先把写位置移到数据的末尾，默认 0。
 *                     用来绕开 avio_close_dyn_buf() 的一个问题（上游 patch 0084），见 README 第 9 步
 *   xor=KEY           仅 mode=cb：写回调里用 KEY 逐字节异或，当作“加密”，KEY 循环使用；
 *                     用 avio_mem_in 的 xor= 可以读回来
 *   bitexact=0|1      1 时让复用器不写随机数和时间（Matroska 的 UID 每次都不同），
 *                     这样两次输出才能逐字节比较，默认 0
 *   其他 名字=值      交给复用器，比如 movflags=frag_keyframe+empty_moov+default_base_moof
 *
 * 退出码：0 正常；1 出错。
 */

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>

/* 攒在内存里的输出：一块能长大的缓冲区，加一个“当前写位置”。 */
typedef struct Sink {
    uint8_t *data;
    size_t   size;          /* 已写入的最大位置，也就是文件的大小 */
    size_t   cap;           /* 缓冲区容量 */
    int64_t  pos;           /* 当前写位置 */

    const char *key;        /* 异或的密钥，NULL 表示不“加密” */
    size_t   keylen;

    int      trace;
    int64_t  calls, writes, seeks, seek_back;  /* seek_back：往回 seek 的次数 */
} Sink;

static void trace_call(Sink *s, const char *fmt, ...)
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
 * 写回调：AVIOContext 的缓冲区满了，或者 avio_flush 了，把数据交给我们。
 * 返回写出的字节数；出错返回负的错误码。
 * 这里的 buf 是 const：回调只能读，不能改。
 */
static int write_cb(void *opaque, const uint8_t *buf, int buf_size)
{
    Sink *s = opaque;
    size_t end = s->pos + buf_size;

    if (end > s->cap) {
        size_t cap = s->cap ? s->cap : 64 * 1024;
        while (cap < end)
            cap *= 2;
        uint8_t *p = av_realloc(s->data, cap);
        if (!p)
            return AVERROR(ENOMEM);
        s->data = p;
        s->cap  = cap;
    }
    /* 往回 seek 过的话，新数据会盖在旧数据上，这正是 MP4 回头补大小的做法 */
    memcpy(s->data + s->pos, buf, buf_size);
    /* “加密”：密钥按文件里的位置循环用，盖在旧数据上时位置也是对的 */
    if (s->key) {
        for (int i = 0; i < buf_size; i++)
            s->data[s->pos + i] ^= (uint8_t)s->key[(s->pos + i) % s->keylen];
    }

    trace_call(s, "写   位置 %lld，%d 字节%s", (long long)s->pos, buf_size,
               end <= s->size ? "（盖在已有的数据上）" : "");
    s->writes++;
    s->pos = end;
    if (s->pos > (int64_t)s->size)
        s->size = s->pos;
    return buf_size;
}

/*
 * seek 回调：复用器要改写位置。输出方向不需要 AVSEEK_SIZE，但回答一下没有坏处。
 */
static int64_t seek_cb(void *opaque, int64_t offset, int whence)
{
    Sink *s = opaque;
    int64_t target;

    if (whence == AVSEEK_SIZE)
        return s->size;

    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: target = offset;                     break;
    case SEEK_CUR: target = s->pos + offset;            break;
    case SEEK_END: target = (int64_t)s->size + offset;  break;
    default:       return AVERROR(EINVAL);
    }
    if (target < 0 || target > (int64_t)s->size)
        return AVERROR(EINVAL);

    trace_call(s, "seek 位置 %lld → %lld", (long long)s->pos, (long long)target);
    s->seeks++;
    if (target < s->pos)
        s->seek_back++;
    s->pos = target;
    return target;
}

typedef struct Options {
    const char *mode;
    int         seek;
    int         bufsize;
    int         trace;
    int         bitexact;
    int         dynend;
    const char *xor_key;
    AVDictionary *mux;
} Options;

static int parse_args(int argc, char **argv, Options *o)
{
    o->mode    = "cb";
    o->seek    = 1;
    o->bufsize = 4096;

    for (int i = 4; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) {
            fprintf(stderr, "参数要写成 名字=值：%s\n", argv[i]);
            return AVERROR(EINVAL);
        }
        *eq = '\0';
        const char *name = argv[i], *val = eq + 1;
        if (!strcmp(name, "mode"))
            o->mode = val;
        else if (!strcmp(name, "seek"))
            o->seek = atoi(val);
        else if (!strcmp(name, "bufsize"))
            o->bufsize = atoi(val);
        else if (!strcmp(name, "trace"))
            o->trace = atoi(val);
        else if (!strcmp(name, "bitexact"))
            o->bitexact = atoi(val);
        else if (!strcmp(name, "dynend"))
            o->dynend = atoi(val);
        else if (!strcmp(name, "xor"))
            o->xor_key = val;
        else
            av_dict_set(&o->mux, name, val, 0);
    }
    if (strcmp(o->mode, "file") && strcmp(o->mode, "cb") && strcmp(o->mode, "dyn")) {
        fprintf(stderr, "mode= 只能是 file、cb 或 dyn：%s\n", o->mode);
        return AVERROR(EINVAL);
    }
    if (o->bufsize < 16) {
        fprintf(stderr, "bufsize= 的值 %d 太小，至少 16\n", o->bufsize);
        return AVERROR(EINVAL);
    }
    if (o->xor_key && (!*o->xor_key || strcmp(o->mode, "cb"))) {
        fprintf(stderr, "xor= 只能用在 mode=cb，而且密钥不能为空\n");
        return AVERROR(EINVAL);
    }
    return 0;
}

/* 把内存里的结果存成文件。 */
static int save_file(const char *name, const uint8_t *data, size_t size)
{
    FILE *f = fopen(name, "wb");

    if (!f) {
        fprintf(stderr, "不能写 %s\n", name);
        return AVERROR(EIO);
    }
    if (fwrite(data, 1, size, f) != size) {
        fclose(f);
        fprintf(stderr, "写 %s 失败\n", name);
        return AVERROR(EIO);
    }
    return fclose(f) ? AVERROR(EIO) : 0;
}

int main(int argc, char **argv)
{
    Sink sink = { 0 };
    Options opt = { 0 };
    AVFormatContext *ifmt = NULL, *ofmt = NULL;
    AVIOContext *avio = NULL;       /* cb 模式下我们自己建的 */
    AVPacket *pkt = NULL;
    int *stream_map = NULL;
    uint8_t *dyn_data = NULL;       /* dyn 模式下 avio_close_dyn_buf 交回来的 */
    int dyn_size = 0;
    int ret, use_file, use_dyn, use_cb;
    int64_t npackets = 0;

    if (argc < 4) {
        fprintf(stderr, "用法：%s <输入文件> <输出格式名> <输出文件> "
                "[mode=file|cb|dyn] [seek=0|1] [bufsize=N] [trace=N] [bitexact=0|1] "
                "[名字=值 ...]\n",
                argv[0]);
        return 1;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    if ((ret = parse_args(argc, argv, &opt)) < 0)
        goto end;
    sink.trace = opt.trace;
    if (opt.xor_key) {
        sink.key    = opt.xor_key;
        sink.keylen = strlen(opt.xor_key);
    }
    use_file = !strcmp(opt.mode, "file");
    use_cb   = !strcmp(opt.mode, "cb");
    use_dyn  = !strcmp(opt.mode, "dyn");

    /* ---- 输入：普通文件 ---- */
    if ((ret = avformat_open_input(&ifmt, argv[1], NULL, NULL)) < 0) {
        fprintf(stderr, "打不开输入 %s：%s\n", argv[1], av_err2str(ret));
        goto end;
    }
    if ((ret = avformat_find_stream_info(ifmt, NULL)) < 0) {
        fprintf(stderr, "找不到流信息：%s\n", av_err2str(ret));
        goto end;
    }

    /* ---- 输出：按名字找复用器；文件名只是给 file 模式用的 ---- */
    ret = avformat_alloc_output_context2(&ofmt, NULL, argv[2], use_file ? argv[3] : NULL);
    if (ret < 0) {
        fprintf(stderr, "不认识的输出格式 %s：%s\n", argv[2], av_err2str(ret));
        goto end;
    }

    if (opt.bitexact)
        ofmt->flags |= AVFMT_FLAG_BITEXACT;

    stream_map = av_calloc(ifmt->nb_streams, sizeof(*stream_map));
    if (!stream_map) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    for (unsigned i = 0; i < ifmt->nb_streams; i++) {
        const AVCodecParameters *in = ifmt->streams[i]->codecpar;
        AVStream *out;

        stream_map[i] = -1;
        if (in->codec_type != AVMEDIA_TYPE_VIDEO &&
            in->codec_type != AVMEDIA_TYPE_AUDIO)
            continue;
        if (!(out = avformat_new_stream(ofmt, NULL))) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        if ((ret = avcodec_parameters_copy(out->codecpar, in)) < 0)
            goto end;
        out->codecpar->codec_tag = 0;
        stream_map[i] = out->index;
    }

    /* ---- 给输出接上 AVIOContext ---- */
    if (use_file) {
        if (!(ofmt->oformat->flags & AVFMT_NOFILE) &&
            (ret = avio_open(&ofmt->pb, argv[3], AVIO_FLAG_WRITE)) < 0) {
            fprintf(stderr, "打不开输出 %s：%s\n", argv[3], av_err2str(ret));
            goto end;
        }
    } else if (use_dyn) {
        if ((ret = avio_open_dyn_buf(&ofmt->pb)) < 0)
            goto end;
    } else {
        uint8_t *iobuf = av_malloc(opt.bufsize);
        if (!iobuf) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        /* write_flag 要给 1；写回调没有 opaque 以外的上下文，所以把 &sink 传进去 */
        avio = avio_alloc_context(iobuf, opt.bufsize, 1, &sink, NULL, write_cb,
                                  opt.seek ? seek_cb : NULL);
        if (!avio) {
            av_free(iobuf);
            ret = AVERROR(ENOMEM);
            goto end;
        }
        ofmt->pb = avio;
    }
    printf("输出方式：%s", use_file ? "libavformat 自己写文件" :
                          use_dyn  ? "avio_open_dyn_buf（内存）" : "自己的写回调（内存）");
    if (use_cb)
        printf("，缓冲区 %d 字节，seek 回调 %s%s", opt.bufsize, opt.seek ? "有" : "没有",
               sink.key ? "，写回调里异或“加密”" : "");
    printf("，格式 %s\n", ofmt->oformat->name);

    /* ---- 写头、写包、写尾，和写文件时一模一样 ---- */
    if (opt.trace)
        printf("回调记录（前 %d 次）：\n", opt.trace);
    ret = avformat_write_header(ofmt, &opt.mux);
    if (ret < 0) {
        fprintf(stderr, "写文件头失败：%s\n", av_err2str(ret));
        goto end;
    }
    if (av_dict_count(opt.mux)) {
        const AVDictionaryEntry *e = av_dict_iterate(opt.mux, NULL);
        fprintf(stderr, "复用器不认识的参数：%s\n", e->key);
    }

    if (!(pkt = av_packet_alloc())) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    while ((ret = av_read_frame(ifmt, pkt)) >= 0) {
        int in_idx = pkt->stream_index;
        if (stream_map[in_idx] < 0) {
            av_packet_unref(pkt);
            continue;
        }
        AVStream *ist = ifmt->streams[in_idx];
        AVStream *ost = ofmt->streams[stream_map[in_idx]];
        pkt->stream_index = ost->index;
        av_packet_rescale_ts(pkt, ist->time_base, ost->time_base);
        pkt->pos = -1;
        ret = av_interleaved_write_frame(ofmt, pkt);
        av_packet_unref(pkt);
        if (ret < 0) {
            fprintf(stderr, "写包失败：%s\n", av_err2str(ret));
            goto end;
        }
        npackets++;
    }
    if (ret != AVERROR_EOF) {
        fprintf(stderr, "读包失败：%s\n", av_err2str(ret));
        goto end;
    }
    if ((ret = av_write_trailer(ofmt)) < 0) {
        fprintf(stderr, "写文件尾失败：%s\n", av_err2str(ret));
        goto end;
    }

    /* ---- 收尾：把内存里的结果存成文件 ---- */
    if (use_file) {
        printf("复制了 %lld 个包，已写入文件 %s\n", (long long)npackets, argv[3]);
    } else if (use_dyn) {
        if (opt.dynend) {
            /* 有的复用器写完文件尾以后，写位置不在数据的末尾（flv 回头改完时长就停在那里）。
             * avio_close_dyn_buf() 会在当前位置补 64 字节的零，盖掉真正的数据，返回的长度也少 64。
             * 先把位置移到末尾，再关闭，就没有这个问题。 */
            uint8_t *tmp;
            int size = avio_get_dyn_buf(ofmt->pb, &tmp);
            avio_seek(ofmt->pb, size, SEEK_SET);
        }
        /* avio_close_dyn_buf 把数据的所有权交给我们，用完要 av_free */
        dyn_size = avio_close_dyn_buf(ofmt->pb, &dyn_data);
        ofmt->pb = NULL;
        printf("复制了 %lld 个包，内存里共 %d 字节\n", (long long)npackets, dyn_size);
        ret = save_file(argv[3], dyn_data, dyn_size);
    } else {
        avio_flush(avio);
        printf("复制了 %lld 个包，内存里共 %zu 字节\n", (long long)npackets, sink.size);
        printf("回调统计：写 %lld 次，seek %lld 次（往回 %lld 次）\n",
               (long long)sink.writes, (long long)sink.seeks, (long long)sink.seek_back);
        ret = save_file(argv[3], sink.data, sink.size);
    }

end:
    if (ofmt) {
        if (use_file && ofmt->pb && !(ofmt->oformat->flags & AVFMT_NOFILE))
            avio_closep(&ofmt->pb);
        else if (use_dyn && ofmt->pb) {
            uint8_t *tmp = NULL;
            avio_close_dyn_buf(ofmt->pb, &tmp);
            av_free(tmp);
        }
        ofmt->pb = NULL;        /* cb 模式的 pb 是我们自己的，下面自己释放 */
        avformat_free_context(ofmt);
    }
    if (avio) {
        av_freep(&avio->buffer);
        avio_context_free(&avio);
    }
    av_free(dyn_data);
    av_free(sink.data);
    av_free(stream_map);
    av_packet_free(&pkt);
    avformat_close_input(&ifmt);
    av_dict_free(&opt.mux);

    if (ret < 0) {
        fprintf(stderr, "出错：%s\n", av_err2str(ret));
        return 1;
    }
    return 0;
}
