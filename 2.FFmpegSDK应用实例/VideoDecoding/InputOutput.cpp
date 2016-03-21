#include <stdio.h>

#include "InputOutput.h"

bool Open_files(IOParam &IOParam)
{
	IOParam.pFin = fopen(IOParam.pNameIn, "rb");
	if (!IOParam.pFin)
	{
		printf("Could not open %s\n", IOParam.pNameIn);
		return false;
	}

	IOParam.pFout = fopen(IOParam.pNameOut, "wb");
	if (!IOParam.pFout) {
		printf("Could not open %s\n", IOParam.pNameOut);
		return false;
	}

	return true;
}

void Parse(int argc, char **argv, IOParam &IOParam)
{
	IOParam.pNameIn = argv[1];
	IOParam.pNameOut = argv[2];
}

void Close_files(IOParam &IOParam)
{
	fclose(IOParam.pFin);
	fclose(IOParam.pFout);
}
