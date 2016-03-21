#pragma once
#ifndef _ENCODER_H_
#define _ENCODER_H_
#include "VideoEncodingHeader.h"
#include "InputOutput.h"


bool Open_encoder(CodecCtx &ctx, IOParam io_param);

void Close_encoder(CodecCtx &ctx);

#endif