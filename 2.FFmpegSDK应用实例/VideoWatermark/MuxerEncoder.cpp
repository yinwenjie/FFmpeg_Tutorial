#include "common.h"
#include "MuxerEncoder.h"

// a wrapper around a single output AVStream
typedef struct OutputStream 
{
	AVStream *st;

	/* pts of the next frame that will be generated */
	int64_t next_pts;
	int samples_count;

	AVFrame *frame;
	AVFrame *tmp_frame;

	float t, tincr, tincr2;

	struct SwsContext *sws_ctx;
	struct SwrContext *swr_ctx;
} OutputStream;

static OutputStream		video_st = { 0 };
static AVOutputFormat	*fmt;
static AVFormatContext	*oc;
static AVCodec			*video_codec;
static AVDictionary		*opt = NULL;

const char *filename;
int have_video = 0, encode_video = 0;

static int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, const AVCodecContext *pCodecCtx, enum AVCodecID codec_id);
static int open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg);
static int write_video_frame(AVFormatContext *oc, OutputStream *ost);
static void close_stream(AVFormatContext *oc, OutputStream *ost);

//**************************************************************************
//---------------------------------接口函数---------------------------------
//**************************************************************************
int InitMuxerEncoder(const char *dst_filename, const AVCodecContext *pCodecCtx)
{
	int ret = 0;
	filename = dst_filename;

	/* allocate the output media context */
	avformat_alloc_output_context2(&oc, NULL, NULL, filename);
	if (!oc)
	{
		printf("Could not deduce output format from file extension: using MPEG.\n");
		avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
	}
	if (!oc)
	{
		return -1;
	}

	fmt = oc->oformat;

	/* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (fmt->video_codec != AV_CODEC_ID_NONE) 
	{
        add_stream(&video_st, oc, &video_codec, pCodecCtx, fmt->video_codec);
        have_video = 1;
        encode_video = 1;
    }

	if (have_video)
		open_video(oc, video_codec, &video_st, opt);

	av_dump_format(oc, 0, filename, 1);

	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0) 
		{
			fprintf(stderr, "Could not open '%s'\n", filename);
			return -1;
		}
	}

	/* Write the stream header, if any. */
	ret = avformat_write_header(oc, &opt);
	if (ret < 0) 
	{
		fprintf(stderr, "Error occurred when opening output file.\n");
		return -1;
	}

	return 0;
}


void CloseMuxerEncoder()
{
	av_write_trailer(oc);
	if (have_video)
		close_stream(oc, &video_st);
	if (!(fmt->flags & AVFMT_NOFILE))
		/* Close the output file. */
		avio_closep(&oc->pb);

	/* free the stream */
	avformat_free_context(oc);
}

void EncodeFrames()
{
	while (encode_video)
	{
		encode_video = !write_video_frame(oc, &video_st);
	}
}
//**************************************************************************
//---------------------------------私有函数---------------------------------
//**************************************************************************
/* Add an output stream. */
static int add_stream(OutputStream *ost, AVFormatContext *oc, 	AVCodec **codec, const AVCodecContext *pCodecCtx, enum AVCodecID codec_id)
{
	AVCodecContext *c;

	/* find the encoder */
	*codec = avcodec_find_encoder(codec_id);
	if (!(*codec)) 
	{
		fprintf(stderr, "MuxerEncoder: Could not find encoder for '%s'\n", avcodec_get_name(codec_id));
		return -1;
	}

	ost->st = avformat_new_stream(oc, *codec);
	if (!ost->st) 
	{
		fprintf(stderr, "MuxerEncoder: Could not allocate stream\n");
		return -1;
	}
	ost->st->id = oc->nb_streams-1;
	c = ost->st->codec;

	AVRational r = { 1, STREAM_FRAME_RATE };
	switch ((*codec)->type) 
	{
		case AVMEDIA_TYPE_VIDEO:
			c->codec_id = pCodecCtx->codec_id;
			c->bit_rate = pCodecCtx->bit_rate;
			c->width    = pCodecCtx->width;
			c->height   = pCodecCtx->height;
			ost->st->time_base = r;
			c->time_base       = ost->st->time_base;
			c->gop_size      = pCodecCtx->gop_size; /* emit one intra frame every twelve frames at most */
			c->pix_fmt       = STREAM_PIX_FMT;
			break;

		default:
			break;
	}

	/* Some formats want stream headers to be separate. */
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
	{
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	return 0;
}

/**************************************************************/
/* video output */
static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	int ret;

	picture = av_frame_alloc();
	if (!picture)
		return NULL;

	picture->format = pix_fmt;
	picture->width  = width;
	picture->height = height;

	/* allocate the buffers for the frame data */
	ret = av_frame_get_buffer(picture, 32);
	if (ret < 0) 
	{
		fprintf(stderr, "Could not allocate frame data.\n");
		exit(1);
	}

	return picture;
}

static int open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
	int ret;
	AVCodecContext *c = ost->st->codec;
	AVDictionary *opt = NULL;

	av_dict_copy(&opt, opt_arg, 0);

	/* open the codec */
	ret = avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);
	if (ret < 0) 
	{
		fprintf(stderr, "Could not open video codec: %d\n", ret);
		return -1;
	}

	/* allocate and init a re-usable frame */
	ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
	if (!ost->frame) 
	{
		fprintf(stderr, "Could not allocate video frame\n");
		return -1;
	}

	ost->tmp_frame = NULL;
	if (c->pix_fmt != AV_PIX_FMT_YUV420P) 
	{
		ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
		if (!ost->tmp_frame) 
		{
			fprintf(stderr, "Could not allocate temporary picture\n");
			return -1;
		}
	}

	return 0;
}


/* Prepare a dummy image. */
static void fill_yuv_image(AVFrame *pict, int frame_index,
                           int width, int height)
{
    int x, y, i, ret;

    /* when we pass a frame to the encoder, it may keep a reference to it
     * internally;
     * make sure we do not overwrite it here
     */
    ret = av_frame_make_writable(pict);
    if (ret < 0)
        exit(1);

    i = frame_index;

    /* Y */
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++)
            pict->data[0][y * pict->linesize[0] + x] = x + y + i * 3;

    /* Cb and Cr */
    for (y = 0; y < height / 2; y++) {
        for (x = 0; x < width / 2; x++) {
            pict->data[1][y * pict->linesize[1] + x] = 128 + y + i * 2;
            pict->data[2][y * pict->linesize[2] + x] = 64 + x + i * 5;
        }
    }
}

static AVFrame *get_video_frame(OutputStream *ost)
{
    AVCodecContext *c = ost->st->codec;

    /* check if we want to generate more frames */
	AVRational r = { 1, 1 };
    if (av_compare_ts(ost->next_pts, ost->st->codec->time_base, STREAM_DURATION, r) >= 0)
	{
        return NULL;
	}

    if (c->pix_fmt != AV_PIX_FMT_YUV420P) 
	{
        /* as we only generate a YUV420P picture, we must convert it
         * to the codec pixel format if needed */
        if (!ost->sws_ctx) 
		{
            ost->sws_ctx = sws_getContext(c->width, c->height,
                                          AV_PIX_FMT_YUV420P,
                                          c->width, c->height,
                                          c->pix_fmt,
                                          SCALE_FLAGS, NULL, NULL, NULL);
            if (!ost->sws_ctx) 
			{
                fprintf(stderr,
                        "Could not initialize the conversion context\n");
                exit(1);
            }
        }
        fill_yuv_image(ost->tmp_frame, ost->next_pts, c->width, c->height);
        sws_scale(ost->sws_ctx,
                  (const uint8_t * const *)ost->tmp_frame->data, ost->tmp_frame->linesize,
                  0, c->height, ost->frame->data, ost->frame->linesize);
    } 
	else 
	{
        fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);
    }

    ost->frame->pts = ost->next_pts++;

    return ost->frame;
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/* Write the compressed frame to the media file. */
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

static int write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
	int ret;
	AVCodecContext *c;
	AVFrame *frame;
	int got_packet = 0;
	AVPacket pkt = { 0 };

	c = ost->st->codec;

	frame = get_video_frame(ost);

	av_init_packet(&pkt);

	/* encode the image */
	ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
	if (ret < 0) 
	{
		fprintf(stderr, "Error encoding video frame: %d\n", ret);
		return -1;
	}

	if (got_packet) 
	{
		ret = write_frame(oc, &c->time_base, ost->st, &pkt);
	} 
	else 
	{
		ret = 0;
	}

	if (ret < 0) 
	{
		fprintf(stderr, "Error while writing video frame: %s\n", ret);
		exit(1);
	}

	return (frame || got_packet) ? 0 : 1;
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
	avcodec_close(ost->st->codec);
	av_frame_free(&ost->frame);
	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
	swr_free(&ost->swr_ctx);
}

int FetchAVFrameToEncoder(AVFrame *src)
{
	int ret = av_frame_copy(video_st.frame, src);

	return ret;
}

int EncodeOneFrame()
{
	int ret;
	AVCodecContext *c;
	AVFrame *frame;
	int got_packet = 0;
	AVPacket pkt = { 0 };

	c = video_st.st->codec;

	frame = video_st.frame;

	av_init_packet(&pkt);

	/* encode the image */
	ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
	if (ret < 0) 
	{
		fprintf(stderr, "Error encoding video frame: %d\n", ret);
		return -1;
	}

	if (got_packet) 
	{
		ret = write_frame(oc, &c->time_base, video_st.st, &pkt);
	} 
	else 
	{
		ret = 0;
	}

	if (ret < 0) 
	{
		fprintf(stderr, "Error while writing video frame: %s\n", ret);
		return -1;
	}

	return (frame || got_packet) ? 0 : 1;
}

