#pragma once
#ifndef _ENCODER_H_
#define _ENCODER_H_
#include "VideoEncodingHeader.h"
#include "InputOutput.h"


bool OpenEncoder(Codec_Ctx &ctx, IO_Param io_param);

void CloseEncoder(Codec_Ctx &ctx);

#endif