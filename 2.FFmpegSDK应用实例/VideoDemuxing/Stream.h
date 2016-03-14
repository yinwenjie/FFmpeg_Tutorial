#pragma once
#ifndef _STREAM_H_
#define _STREAM_H_

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

void Add_stream(OutputStream *ost, AVFormatContext *oc,	AVCodec **codec, enum AVCodecID codec_id);

#endif