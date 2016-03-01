#pragma once
#ifndef _ENCODER_H_
#define _ENCODER_H_
#include "VideoEncodingHeader.h"

typedef struct
{
	AVCodec			*codec;
	AVFrame			*frame;
	AVCodecContext	*c;
	AVPacket		pkt;
} Codec_Ctx;

bool OpenEncoder(Codec_Ctx &ctx);

void CloseEncoder(Codec_Ctx &ctx);

#endif