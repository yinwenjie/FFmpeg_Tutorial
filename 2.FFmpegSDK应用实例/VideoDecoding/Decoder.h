#pragma once
#ifndef _DECODER_H_
#define _DECODER_H_
#include "VideoDecodingHeader.h"

bool OpenDeocder(Codec_Ctx &ctx);

void CloseDecoder(Codec_Ctx &ctx);

#endif