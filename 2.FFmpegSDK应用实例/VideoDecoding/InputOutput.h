#pragma once
#ifndef _INPUT_OUTPUT_H_
#define _INPUT_OUTPUT_H_

typedef struct
{
	FILE *pFin;
	FILE *pFout;

	char *pNameIn;
	char *pNameOut;
} IOParam;

void Parse(int argc, char **argv, IOParam &IOParam);

bool Open_files(IOParam &IOParam);

void Close_files(IOParam &IOParam);

#endif