#pragma once
#ifndef _DECODER_H_
#define _DECODER_H_
#include "VideoDecodingHeader.h"

bool OpenDeocder(CodecCtx &ctx);

void CloseDecoder(CodecCtx &ctx);

#endif