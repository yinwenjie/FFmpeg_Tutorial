#include <string.h>
#include <stdlib.h>
#include "InputOutput.h"

bool ParseInputParam(int argc, char **argv, IO_Param &io_param)
{
	bool bNameInFound = false, bNameOutFound = false, bWidthFound = false, bHeightFound = false, bBitrateFound = false;

	io_param.nFrameRate		= 25;
	io_param.nGOPSize		= 10;
	io_param.nMaxBFrames	= 1;

	for (int idx = 1; idx < argc; idx++)
	{
		if (!stricmp(argv[idx], "-i"))
		{
			io_param.pNameIn = argv[idx+1];
			bNameInFound = true;
		} 
		else if (!stricmp(argv[idx], "-o"))
		{
			io_param.pNameOut = argv[idx+1];
			bNameOutFound = true;
		}
		else if (!stricmp(argv[idx], "-w"))
		{
			io_param.nImageWidth = atoi(argv[idx+1]);
			bWidthFound = true;
		}
		else if (!stricmp(argv[idx], "-h"))
		{
			io_param.nImageHeight = atoi(argv[idx+1]);
			bHeightFound = true;
		}
		else if (!stricmp(argv[idx], "-br"))
		{
			io_param.nBitRate = atol(argv[idx+1]);
			bBitrateFound = true;
		}
		else if (!stricmp(argv[idx], "-fr"))
		{
			io_param.nFrameRate = atoi(argv[idx+1]);
		}
		else if (!stricmp(argv[idx], "-gs"))
		{
			io_param.nGOPSize = atoi(argv[idx+1]);
		}
		else if (!stricmp(argv[idx], "-mbs"))
		{
			io_param.nMaxBFrames = atoi(argv[idx+1]);
		}
	}

	if (!bNameInFound)
	{
		printf("Error: Cannot find input file name.\n");
		return false;
	}
	if (!bNameOutFound)
	{
		printf("Error: Cannot find output file name.\n");
		return false;
	}
	if (!bWidthFound)
	{
		printf("Error: Cannot find image width.\n");
		return false;
	}
	if (!bHeightFound)
	{
		printf("Error: Cannot find image height.\n");
		return false;
	}
	if (!bBitrateFound)
	{
		printf("Error: Cannot find bitrate.\n");
		return false;
	}

	printf("Paramaters parsing OK!\n");
	return true;
}

bool OpenFile(IO_Param &io_param)
{
	io_param.pFin = fopen(io_param.pNameIn, "rb");
	if (!(io_param.pFin))
	{
		fprintf(stderr, "Could not open %s\n", io_param.pNameIn);
		return false;
	}

	io_param.pFout = fopen(io_param.pNameOut, "wb");
	if (!(io_param.pFin))
	{
		fprintf(stderr, "Could not open %s\n", io_param.pNameOut);
		return false;
	}
}

void CloseFile(IO_Param &io_param)
{
	fclose(io_param.pFin);
	fclose(io_param.pFout);
}

int ReadYUVData(IO_Param &io_param)
{

}
