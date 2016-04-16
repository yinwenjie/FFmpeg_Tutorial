#ifndef _VIDEO_ENCODING_MUXING_HEADER_
#define _VIDEO_ENCODING_MUXING_HEADER_
#include "common.h"

typedef struct _OutputStream 
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

typedef struct _EncodingContext
{
	OutputStream video_st;
	AVCodec *video_codec;
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	AVPacket outPacket;
} EncodingContext;

#endif