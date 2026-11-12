/*
 * mtool thumb：按时间间隔或关键帧生成缩略图，或者拼成一张图。
 *
 *   mtool thumb [-i 间隔秒 | -k] [-w 宽 | -s 宽x高] [-m 最多张数] [--sheet 列x行] [--format jpg|png] <输入> <输出>
 *
 * 来自 L7（解码）、L9（缩放和像素格式）、L12（解码接滤镜图）、L13（编码）。
 * 解码出来的帧先选（到了下一个时间点的第一帧，或者每一个关键帧），再过一张滤镜图：
 *     [旋转] → scale 缩小 → setsar=1 → [tile 拼图]
 * 旋转、缩放、转像素格式（JPEG 要 yuvj420p，PNG 要 rgb24）都交给滤镜图，出口只接受编码器支持的格式，
 * 所以格式对不上时图自己插入 scale。图片编码好以后直接写文件：一个 JPEG 或 PNG 包就是完整的一张图，不需要复用器。
 */
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avstring.h>
#include <libavutil/mathematics.h>

#include "mtool.h"

typedef struct Thumb {
    AVFormatContext *ic;
    AVStream        *st;
    AVCodecContext  *dec, *enc;
    MtGraph          graph;
    AVFrame         *frame, *filt;
    AVPacket        *pkt;
    int       sheet, cols, rows, quality, overwrite, png;
    int64_t   written;              /* 已经写出的图片数 */
    int64_t   max;                  /* 最多多少张（拼图时是格子数），0 不限 */
    int64_t   fed;                  /* 已经送进滤镜图的帧数 */
    int64_t   skipped_changed;      /* 尺寸或格式中途变了，没法送进图的帧 */
    int64_t   decode_errors;
    char      out[1024];            /* 拼图时是输出文件，否则是输出目录 */
    const char *ext;
    double    file_start;
    FILE     *fp;
    char      cur_path[1100];
    double    cur_time;
    /* 选帧用 */
    int       keyframes, auto_interval;
    double    interval, next_t;
    int64_t   frame_no;
} Thumb;

static void usage(FILE *f)
{
    fprintf(f,
            "用法：mtool thumb [选项] <输入> <输出>\n\n"
            "从视频里取缩略图。不加 --sheet：输出是一个目录（不存在会创建），里面是 thumb_0001.jpg、thumb_0002.jpg……；\n"
            "加 --sheet：输出是一个图片文件，所有缩略图拼成一张。\n\n"
            "选项：\n"
            "  -i, --interval 秒   每隔这么多秒取一张（到了这个时间点之后的第一帧），第一张在 0 秒\n"
            "  -k, --keyframes     每个关键帧取一张（只解码关键帧，快）\n"
            "                      不给 -i 也不给 -k：普通模式每秒一张；拼图模式把整个视频平均分给所有格子\n"
            "  -w, --width 宽      缩略图的宽，高按比例（默认 320）\n"
            "  -s, --size 宽x高    缩到这个框里，保持比例（和 -w 只能给一个）\n"
            "  -m, --max 张数      最多取这么多张（拼图时默认是格子数）\n"
            "      --sheet 列x行   拼成一张图，例如 4x3\n"
            "      --format 格式   jpg 或 png（普通模式用；拼图时由输出文件的扩展名决定），默认 jpg\n"
            "  -q, --quality 2-31  JPEG 质量，越小越好，和 ffmpeg 的 -q:v 一样（默认 3）\n"
            "      --no-rotate     不按视频里的旋转信息转正（默认转正）\n"
            "  -y                  已经存在的输出覆盖掉\n"
            "  -h, --help          显示这段说明\n");
}

static int parse_wxh(const char *s, int *w, int *h)
{
    char junk;

    return sscanf(s, "%dx%d%c", w, h, &junk) == 2 && *w > 0 && *h > 0 ? 0 : -1;
}

/* 编码器在收到第一张图时才打开：宽高和像素格式以滤镜图出口给出的为准。 */
static int open_encoder(Thumb *t, const AVFrame *f)
{
    const AVCodec *codec = avcodec_find_encoder(t->png ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG);
    int ret;

    if (!codec) {
        mt_averr(AVERROR_ENCODER_NOT_FOUND, "这份 FFmpeg 没有 %s 编码器", t->png ? "png" : "mjpeg");
        return AVERROR_ENCODER_NOT_FOUND;
    }
    t->enc = avcodec_alloc_context3(codec);
    if (!t->enc)
        return AVERROR(ENOMEM);
    t->enc->width      = f->width;
    t->enc->height     = f->height;
    t->enc->pix_fmt    = f->format;
    t->enc->time_base  = (AVRational){ 1, 25 };
    t->enc->color_range = f->color_range;
    t->enc->colorspace  = f->colorspace;
    t->enc->sample_aspect_ratio = f->sample_aspect_ratio;
    if (!t->png) {
        t->enc->flags         |= AV_CODEC_FLAG_QSCALE;       /* 固定质量，不是固定码率 */
        t->enc->global_quality = FF_QP2LAMBDA * t->quality;
    }
    if ((ret = avcodec_open2(t->enc, codec, NULL)) < 0)
        mt_averr(ret, "打不开 %s 编码器", codec->name);
    return ret;
}

static int open_output_file(Thumb *t, int index)
{
    if (t->sheet)
        snprintf(t->cur_path, sizeof(t->cur_path), "%s", t->out);
    else
        snprintf(t->cur_path, sizeof(t->cur_path), "%s/thumb_%04d.%s", t->out, index, t->ext);
    t->fp = fopen(t->cur_path, "wb");
    if (!t->fp) {
        mt_err("打不开输出 %s：%s", t->cur_path, strerror(errno));
        return AVERROR(errno);
    }
    return 0;
}

/* 编码一张图并写文件。frame 为 NULL 是冲刷编码器（图片编码器没有延迟，但接口要求这样结束）。 */
static int encode_write(Thumb *t, AVFrame *frame)
{
    int ret = avcodec_send_frame(t->enc, frame);

    if (ret < 0) {
        mt_averr(ret, "编码图片失败");
        return ret;
    }
    while ((ret = avcodec_receive_packet(t->enc, t->pkt)) >= 0) {
        int index = (int)t->written + 1;

        if (open_output_file(t, index) < 0) {
            av_packet_unref(t->pkt);
            return AVERROR(EIO);
        }
        if (fwrite(t->pkt->data, 1, t->pkt->size, t->fp) != (size_t)t->pkt->size || fclose(t->fp) != 0) {
            mt_err("写 %s 失败：%s（磁盘满了吗？）", t->cur_path, strerror(errno));
            t->fp = NULL;
            unlink(t->cur_path);
            av_packet_unref(t->pkt);
            return AVERROR(EIO);
        }
        t->fp = NULL;
        t->written++;
        if (t->sheet)
            printf("拼图 %s：%d 列 x %d 行，每格取自视频的一帧，图片 %dx%d\n", t->cur_path, t->cols, t->rows,
                   t->enc->width, t->enc->height);
        else
            printf("%s  %.3f 秒\n", t->cur_path, t->cur_time);
        av_packet_unref(t->pkt);
    }
    return ret == AVERROR(EAGAIN) || ret == AVERROR_EOF ? 0 : ret;
}

/* 从滤镜图出口取图、编码、写文件，取到没有为止。 */
static int drain_graph(Thumb *t)
{
    int ret;

    while ((ret = av_buffersink_get_frame(t->graph.sink, t->filt)) >= 0) {
        if (!t->enc && (ret = open_encoder(t, t->filt)) < 0) {
            av_frame_unref(t->filt);
            return ret;
        }
        t->filt->pts = t->written;
        t->filt->pict_type = AV_PICTURE_TYPE_NONE;
        /* 固定质量模式下 mjpeg 编码器按每一帧自己的 quality 来；不设的话 -q 不起作用（ffmpeg 命令行也是这样设的） */
        t->filt->quality = t->enc->global_quality;
        /* 旋转已经在滤镜里做掉了；显示矩阵留着的话 PNG 编码器会写成 EXIF 方向，看图软件会再转一次 */
        av_frame_remove_side_data(t->filt, AV_FRAME_DATA_DISPLAYMATRIX);
        ret = encode_write(t, t->filt);
        av_frame_unref(t->filt);
        if (ret < 0)
            return ret;
    }
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
        return 0;
    mt_averr(ret, "从滤镜图取图失败");
    return ret;
}

/*
 * 从解码器取出所有现成的帧：选帧（到了下一个时间点的第一帧，或者关键帧），选中的送进滤镜图并把出来的图写掉。
 * 返回 0 正常，负数是出错（已经报告过）。
 */
static int receive_frames(Thumb *t)
{
    int ret;

    while ((ret = avcodec_receive_frame(t->dec, t->frame)) >= 0) {
        const int64_t ts = t->frame->best_effort_timestamp != AV_NOPTS_VALUE ? t->frame->best_effort_timestamp : t->frame->pts;
        double ft;
        int pick;

        if (ts != AV_NOPTS_VALUE)
            ft = ts * av_q2d(t->st->time_base) - t->file_start;
        else
            ft = t->frame_no / (t->st->avg_frame_rate.num ? av_q2d(t->st->avg_frame_rate) : 25.0);
        t->frame_no++;

        pick = t->keyframes ? !!(t->frame->flags & AV_FRAME_FLAG_KEY) : ft >= t->next_t - 1e-6;
        if (pick && (t->frame->width != t->dec->width || t->frame->height != t->dec->height ||
                     t->frame->format != t->dec->pix_fmt)) {
            t->skipped_changed++;   /* 画面参数中途变了：滤镜图是按开头的参数建的，送不进去（L22 讲过怎么重建） */
            pick = 0;
        }
        if (pick && (!t->max || t->fed < t->max)) {
            if (!t->keyframes && t->interval > 0)
                t->next_t = (t->auto_interval ? (floor(ft / t->interval) + 1.5) : (floor(ft / t->interval) + 1)) * t->interval;
            t->cur_time = ft;
            ret = av_buffersrc_add_frame_flags(t->graph.src, t->frame, AV_BUFFERSRC_FLAG_KEEP_REF);
            if (ret < 0) {
                mt_averr(ret, "把帧送进滤镜图失败");
                av_frame_unref(t->frame);
                return ret;
            }
            t->fed++;
            if ((ret = drain_graph(t)) < 0) {
                av_frame_unref(t->frame);
                return ret;
            }
        }
        av_frame_unref(t->frame);
    }
    return 0;
}

int cmd_thumb(int argc, char **argv)
{
    static const struct option longopts[] = {
        { "interval",  required_argument, NULL, 'i' },
        { "keyframes", no_argument,       NULL, 'k' },
        { "width",     required_argument, NULL, 'w' },
        { "size",      required_argument, NULL, 's' },
        { "max",       required_argument, NULL, 'm' },
        { "sheet",     required_argument, NULL, 'S' },
        { "format",    required_argument, NULL, 'f' },
        { "quality",   required_argument, NULL, 'q' },
        { "no-rotate", no_argument,       NULL, 'R' },
        { "help",      no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    Thumb t = { .quality = 3, .ext = "jpg" };
    double interval = 0, next_t = 0;
    int keyframes = 0, width = 0, box_w = 0, box_h = 0, no_rotate = 0, opt, ret = 0, rc = MT_FAIL;
    int vidx, auto_interval = 0;
    const char *in_path, *out_path, *fmt = NULL;
    char desc[512], rot[48];

    optind = 1;
    while ((opt = getopt_long(argc, argv, "i:kw:s:m:q:yh", longopts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            if (mt_parse_time(optarg, &interval) < 0 || interval <= 0)
                return mt_usage_err("thumb", "间隔 %s 不是正的时间", optarg);
            break;
        case 'k': keyframes = 1; break;
        case 'w':
            width = atoi(optarg);
            if (width < 16 || width > 8192)
                return mt_usage_err("thumb", "宽 %s 要在 16 到 8192 之间", optarg);
            break;
        case 's':
            if (parse_wxh(optarg, &box_w, &box_h) < 0 || box_w < 16 || box_h < 16 || box_w > 8192 || box_h > 8192)
                return mt_usage_err("thumb", "尺寸 %s 要写成 宽x高，各在 16 到 8192 之间", optarg);
            break;
        case 'm':
            t.max = atoll(optarg);
            if (t.max < 1)
                return mt_usage_err("thumb", "张数 %s 要大于 0", optarg);
            break;
        case 'S':
            if (parse_wxh(optarg, &t.cols, &t.rows) < 0 || t.cols > 30 || t.rows > 30)
                return mt_usage_err("thumb", "拼图 %s 要写成 列x行，各在 1 到 30 之间", optarg);
            t.sheet = 1;
            break;
        case 'f': fmt = optarg; break;
        case 'q':
            t.quality = atoi(optarg);
            if (t.quality < 2 || t.quality > 31)
                return mt_usage_err("thumb", "质量 %s 要在 2 到 31 之间", optarg);
            break;
        case 'R': no_rotate = 1; break;
        case 'y': t.overwrite = 1; break;
        case 'h': usage(stdout); return MT_OK;
        default:  return mt_usage_err("thumb", "不认识的选项");
        }
    }
    if (argc - optind != 2)
        return mt_usage_err("thumb", "需要一个输入和一个输出");
    if (keyframes && interval > 0)
        return mt_usage_err("thumb", "-i 和 -k 只能给一个");
    if (width && box_w)
        return mt_usage_err("thumb", "-w 和 -s 只能给一个");
    if (fmt && strcmp(fmt, "jpg") && strcmp(fmt, "jpeg") && strcmp(fmt, "png"))
        return mt_usage_err("thumb", "格式 %s 不支持，只有 jpg 和 png", fmt);
    in_path  = argv[optind];
    out_path = argv[optind + 1];
    snprintf(t.out, sizeof(t.out), "%s", out_path);
    if (t.sheet) {
        const char *dot = strrchr(out_path, '.');

        if (dot && (!av_strcasecmp(dot, ".png")))
            t.png = 1;
        else if (!dot || (av_strcasecmp(dot, ".jpg") && av_strcasecmp(dot, ".jpeg")))
            return mt_usage_err("thumb", "拼图的输出文件 %s 要以 .jpg、.jpeg 或 .png 结尾", out_path);
        if (fmt)
            mt_warn("拼图的格式由输出文件的扩展名决定，--format 不起作用");
        if (!t.max || t.max > (int64_t)t.cols * t.rows)
            t.max = (int64_t)t.cols * t.rows;
    } else {
        t.png = fmt && !strcmp(fmt, "png");
        t.ext = t.png ? "png" : "jpg";
    }

    t.pkt = av_packet_alloc();
    t.frame = av_frame_alloc();
    t.filt  = av_frame_alloc();
    if (!t.pkt || !t.frame || !t.filt) {
        mt_err("内存不够");
        goto end;
    }
    if (mt_open_input(&t.ic, in_path) < 0)
        goto end;
    vidx = av_find_best_stream(t.ic, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vidx < 0 || (t.ic->streams[vidx]->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
        mt_err("%s 里没有视频流，取不了缩略图", in_path);
        goto end;
    }
    t.st = t.ic->streams[vidx];
    t.file_start = t.ic->start_time != AV_NOPTS_VALUE ? t.ic->start_time / (double)AV_TIME_BASE : 0;

    /* 输出：普通模式要有目录，拼图模式要有能写的位置；已经存在的东西不加 -y 不覆盖 */
    if (t.sheet) {
        if (!t.overwrite && access(out_path, F_OK) == 0) {
            mt_err("输出文件 %s 已经存在，加 -y 覆盖它", out_path);
            goto end;
        }
    } else {
        struct stat sb;

        if (stat(out_path, &sb) == 0) {
            if (!S_ISDIR(sb.st_mode)) {
                mt_err("输出 %s 已经存在而且不是目录", out_path);
                goto end;
            }
        } else if (mkdir(out_path, 0777) < 0) {
            mt_err("创建输出目录 %s 失败：%s", out_path, strerror(errno));
            goto end;
        }
        {
            char first[1100];

            snprintf(first, sizeof(first), "%s/thumb_0001.%s", out_path, t.ext);
            if (!t.overwrite && access(first, F_OK) == 0) {
                mt_err("目录 %s 里已经有 thumb_0001.%s，加 -y 覆盖", out_path, t.ext);
                goto end;
            }
        }
    }

    /* 取样的间隔：没有指定时，普通模式每秒一张，拼图模式把整个视频平均分给所有格子（每格取所在时间段的中间） */
    if (!keyframes && interval <= 0) {
        const double dur = t.ic->duration != AV_NOPTS_VALUE ? t.ic->duration / (double)AV_TIME_BASE : 0;

        if (t.sheet && dur > 0) {
            interval = dur / (t.cols * t.rows);
            next_t = interval / 2;
            auto_interval = 1;
        } else {
            interval = 1;
        }
    }

    if (mt_open_decoder(t.st, keyframes, &t.dec) < 0)
        goto end;

    /* 滤镜图描述：先转正，再缩小，再拼图 */
    rot[0] = '\0';
    if (!no_rotate)
        mt_rotation_filter(t.st, rot, sizeof(rot));
    {
        int n = snprintf(desc, sizeof(desc), "%s%s", rot, rot[0] ? "," : "");

        /* JPEG 是全范围（0 到 255）的 YUV；mjpeg 编码器不接受标称范围（16 到 235）的 YUV，所以缩放的同时转成全范围 */
        const char *range = t.png ? "" : ":out_range=full";

        if (box_w)
            n += snprintf(desc + n, sizeof(desc) - n, "scale=%d:%d:force_original_aspect_ratio=decrease:force_divisible_by=2%s",
                          box_w, box_h, range);
        else
            n += snprintf(desc + n, sizeof(desc) - n, "scale=%d:-2%s", width ? width : 320, range);
        n += snprintf(desc + n, sizeof(desc) - n, ",setsar=1");
        if (t.sheet)
            snprintf(desc + n, sizeof(desc) - n, ",tile=%dx%d:margin=4:padding=4:color=black", t.cols, t.rows);
    }
    {
        const AVCodec *enc = avcodec_find_encoder(t.png ? AV_CODEC_ID_PNG : AV_CODEC_ID_MJPEG);

        if (mt_graph_init(&t.graph, t.dec, av_guess_frame_rate(t.ic, t.st, NULL), desc, enc) < 0)
            goto end;
    }

    /* ---- 读包、解码、选帧、送进滤镜图 ---- */
    t.keyframes = keyframes;
    t.auto_interval = auto_interval;
    t.interval = interval;
    t.next_t = next_t;
    while (!(t.max && t.fed >= t.max)) {
        ret = av_read_frame(t.ic, t.pkt);
        if (ret < 0)
            break;
        if (t.pkt->stream_index != vidx) {
            av_packet_unref(t.pkt);
            continue;
        }
        ret = avcodec_send_packet(t.dec, t.pkt);
        av_packet_unref(t.pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF)
            t.decode_errors++;      /* 损坏的包：记下来，继续往后读 */
        if (receive_frames(&t) < 0)
            goto end;
    }
    if (ret < 0 && ret != AVERROR_EOF && !(t.max && t.fed >= t.max))
        mt_warn("读包到一半出错了：%s。已经取到的图片保留", av_err2str(ret));

    /* 冲刷：解码器里还留着几帧（帧线程的延迟、B 帧重排），只取关键帧时它们恰好是最后几个关键帧，不能丢 */
    if (!(t.max && t.fed >= t.max)) {
        avcodec_send_packet(t.dec, NULL);
        if (receive_frames(&t) < 0)
            goto end;
    }
    /* 告诉滤镜图结束；拼图时把没填满的格子补黑后输出 */
    if (av_buffersrc_add_frame_flags(t.graph.src, NULL, 0) < 0 || drain_graph(&t) < 0)
        goto end;

    if (t.written == 0) {
        mt_err("没有取到任何图片（视频里没有满足条件的帧%s）", keyframes ? "，没有关键帧" : "");
        goto end;
    }
    if (t.skipped_changed)
        mt_warn("有 %lld 帧因为画面尺寸或格式中途变了被跳过", (long long)t.skipped_changed);
    if (t.decode_errors)
        mt_warn("解码时遇到 %lld 个出错的包，对应的画面可能是花的", (long long)t.decode_errors);
    if (!t.sheet)
        printf("共 %lld 张，图片 %dx%d\n", (long long)t.written, t.enc->width, t.enc->height);
    rc = MT_OK;

end:
    if (t.fp) {
        fclose(t.fp);
        unlink(t.cur_path);
    }
    mt_graph_free(&t.graph);
    avcodec_free_context(&t.dec);
    avcodec_free_context(&t.enc);
    avformat_close_input(&t.ic);
    av_frame_free(&t.frame);
    av_frame_free(&t.filt);
    av_packet_free(&t.pkt);
    return rc;
}
