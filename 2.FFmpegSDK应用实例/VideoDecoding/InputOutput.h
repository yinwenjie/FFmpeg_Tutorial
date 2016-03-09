#pragma once
#ifndef _INPUT_OUTPUT_H_
#define _INPUT_OUTPUT_H_

typedef struct
{
	FILE *pFin;
	FILE *pFout;

	char *pNameIn;
	char *pNameOut;
} IO_Param;

void Parse(int argc, char **argv, IO_Param &io_param);

bool OpenFiles(IO_Param &io_param);

void CloseFiles(IO_Param &io_param);

#endif