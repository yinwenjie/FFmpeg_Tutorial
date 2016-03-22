#pragma once
#ifndef _VIDEO_ENCODING_HEADER_
#define _VIDEO_ENCODING_HEADER_

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
	AVCodec			*codec;		//指向编解码器实例
	AVFrame			*frame;		//保存解码之后/编码之前的像素数据
	AVCodecContext	*c;			//编解码器上下文，保存编解码器的一些参数设置
	AVPacket		pkt;		//码流包结构，包含编码码流数据
} CodecCtx;

#endif