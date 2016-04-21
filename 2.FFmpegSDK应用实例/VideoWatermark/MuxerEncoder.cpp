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

#define DESIGNATED_FRAME_RATE 1

#define STREAM_DURATION   10.0
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */

#if DESIGNATED_FRAME_RATE

#define STREAM_FRAME_RATE 10000 /* 25 images/s */

#endif

static OutputStream		video_st = { 0 };
static AVOutputFormat	*fmt;
static AVFormatContext	*oc;
static AVCodec			*video_codec;
static AVDictionary		*opt = NULL;

const char *filename;
int have_video = 0, encode_video = 0;
int frame_pts = 0, frame_dts = 0; 

static int output_packet_count = 0;
static int output_frame_count = 0;

static int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, const AVCodecContext *pCodecCtx, enum AVCodecID codec_id);
static int open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg);
static void close_stream(AVFormatContext *oc, OutputStream *ost);

//**************************************************************************
//---------------------------------接口函数---------------------------------
//**************************************************************************
int InitMuxerEncoder(const char *dst_filename, const AVCodecContext *pCodecCtx, const AVStream *pInputVideoStream)
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
	fmt->video_codec = pCodecCtx->codec_id;

	/* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
	AVRational r = {1, 1000};
    if (fmt->video_codec != AV_CODEC_ID_NONE) 
	{
        add_stream(&video_st, oc, &video_codec, pCodecCtx, fmt->video_codec);

#if (DESIGNATED_FRAME_RATE == 0)
// 		AVRational stream_time_base = {avg_frame_rate.den, avg_frame_rate.num};
// 		video_st.st->time_base = stream_time_base;
// 		video_st.st->codec->time_base = stream_time_base;	
		video_st.st->time_base = r;//pInputVideoStream->time_base;
		video_st.st->avg_frame_rate = pInputVideoStream->avg_frame_rate;
#endif
		video_st.st->time_base = r;
		video_st.st->avg_frame_rate = pInputVideoStream->avg_frame_rate;
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
	printf("**************************\nTotal num of packets in output file; %d\n**************************\n", output_packet_count);
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

#if DESIGNATED_FRAME_RATE
	AVRational r = { 1, STREAM_FRAME_RATE };
#endif
	switch ((*codec)->type) 
	{
		case AVMEDIA_TYPE_VIDEO:
			c->codec_id = pCodecCtx->codec_id;
			c->bit_rate = pCodecCtx->bit_rate;
			c->width    = pCodecCtx->width;
			c->height   = pCodecCtx->height;
			c->time_base = pCodecCtx->time_base;
#if DESIGNATED_FRAME_RATE
			ost->st->time_base = r;
			c->time_base       = ost->st->time_base;
#endif
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

	if (av_frame_is_writable(picture))
	{
		memset(picture->data[0], width * height, 128);
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
/*
	// allocate and init a re-usable frame 
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
*/
	return 0;
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
//	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/* Write the compressed frame to the media file. */
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

static void close_stream(AVFormatContext *oc, OutputStream *ost)
{
	avcodec_close(ost->st->codec);
//	av_frame_free(&ost->frame);
//	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
	swr_free(&ost->swr_ctx);
}

int FetchAVFrameToEncoder(AVFrame *src)
{
// 	int ret = av_frame_copy(video_st.frame, src);
// 
// 	av_frame_copy_props(video_st.frame, src);
// 
// 	video_st.frame->pts = video_st.frame->pkt_pts;

	video_st.frame = src;

	return 0;
}

int EncodeOneFrame()
{
	int ret;
	AVCodecContext *c;
	AVFrame *frame;
	int got_packet = 0;
	AVPacket pkt = { 0 };
//	AVRational r = {1, 1000};

	c = video_st.st->codec;
//	c->time_base = r;

	frame = video_st.frame;

	OutputTextLogOut("***********************************\nEncoded frame in:", output_frame_count++);
	OutputTextLogOut("\n***********************************\n");

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
		OutputTextLogOut("Write packet:", output_packet_count++);
		OutputTextLogOut("\t\tPacket pts:", pkt.pts);
		OutputTextLogOut("\n");
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

	av_frame_unref(frame);
	return (frame || got_packet) ? 0 : 1;
}

