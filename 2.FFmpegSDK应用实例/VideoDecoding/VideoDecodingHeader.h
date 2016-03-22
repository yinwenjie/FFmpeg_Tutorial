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

/*************************************************
	Struct:			CodecCtx
	Description:	FFMpeg编解码器上下文
*************************************************/
typedef struct
{
	AVCodec			*pCodec;				//编解码器实例指针
	AVCodecContext	*pCodecContext;			//编解码器上下文，指定了编解码的参数
	AVCodecParserContext *pCodecParserCtx;	//编解码解析器，从码流中截取完整的一个NAL Unit数据
	
	AVFrame			*frame;					//封装图像对象指针
	AVPacket		pkt;					//封装码流对象实例
} CodecCtx;

#endif