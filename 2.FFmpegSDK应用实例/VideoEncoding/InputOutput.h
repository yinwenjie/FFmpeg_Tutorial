#pragma once
#ifndef _INPUT_OUTPUT_H_
#define _INPUT_OUTPUT_H_

#include <stdio.h>
#include <stdint.h>
#include "VideoEncodingHeader.h"

typedef struct
{
	FILE *pFin;
	FILE *pFout;

	char *pNameIn;
	char *pNameOut;

	uint16_t nImageWidth;
	uint16_t nImageHeight;

	uint16_t nFrameRate;
	uint64_t nBitRate;
	uint16_t nGOPSize;
	uint16_t nMaxBFrames;
	uint16_t nTotalFrames;
} IO_Param;

bool ParseInputParam(int argc, char **argv, IO_Param &io_param);

bool OpenFile(IO_Param &io_param);

void CloseFile(IO_Param &io_param);

int ReadYUVData(Codec_Ctx &ctx, IO_Param &io_param, int color_plane);

#endif