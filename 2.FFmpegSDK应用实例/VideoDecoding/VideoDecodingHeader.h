#pragma once

#ifndef _VIDEO_DECODING_HEADER_
#define _VIDEO_DECODING_HEADER_

#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

extern "C"
{
	#include "libavutil/opt.h"
	#include "libavcodec/avcodec.h"
	#include "libavutil/channel_layout.h"
	#include "libavutil/common.h"
	#include "libavutil/imgutils.h"
	#include "libavutil/mathematics.h"
	#include "libavutil/samplefmt.h"
}

typedef struct
{
	AVCodec			*pCodec;	
	AVCodecContext	*pCodecContext;
	AVCodecParserContext *pCodecParserCtx;
	
	AVFrame			*frame;
	AVPacket		pkt;
} CodecCtx;

#endif