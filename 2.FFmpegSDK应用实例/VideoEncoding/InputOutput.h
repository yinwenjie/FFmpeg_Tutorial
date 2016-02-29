#pragma once
#ifndef _INPUT_OUTPUT_H_
#define _INPUT_OUTPUT_H_

#include <stdio.h>
#include <stdint.h>

typedef struct
{
	FILE *pFin;
	FILE *pFout;

	char *pNameIn;
	char *pNameOut;

	uint16_t nImageWidth;
	uint16_t nImageHeight;

	uint16_t nFrameRate;
	uint16_t nBitRate;
	uint16_t nGOPSize;
	uint16_t nMaxBFrames;
} IO_Param;

bool ParseInputParam(int argc, char **argv, IO_Param &io_param);

#endif