#include <stdio.h>

#include "InputOutput.h"

bool OpenFiles(IO_Param &io_param)
{
	io_param.pFin = fopen(io_param.pNameIn, "rb");
	if (!io_param.pFin)
	{
		printf("Could not open %s\n", io_param.pNameIn);
		return false;
	}

	io_param.pFout = fopen(io_param.pNameOut, "wb");
	if (!io_param.pFout) {
		printf("Could not open %s\n", io_param.pNameOut);
		return false;
	}

	return true;
}

void Parse(int argc, char **argv, IO_Param &io_param)
{
	io_param.pNameIn = argv[1];
	io_param.pNameOut = argv[2];
}

void CloseFiles(IO_Param &io_param)
{
	fclose(io_param.pFin);
	fclose(io_param.pFout);
}
