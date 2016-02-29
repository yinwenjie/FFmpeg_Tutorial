#pragma once
#ifndef _ENCODER_H_
#define _ENCODER_H_

#include "VideoEncodingHeader.h"

bool OpenFile(FILE* &pFile);

bool OpenEncoder(CodecCtx &ctx);

void CloseEncoder(CodecCtx &ctx);

#endif