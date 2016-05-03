#include "common.h"

static int  hello(int argc, char **argv, FileInOut &files)
{
	printf("FFMpeg Trans Coding Demo:\nCommand line format: %s inputFileName outputFileName.\n", argv[0]);
	if (argc != 3)
	{
		printf("Command line parameters error. Please re-check.\n");
		return -1; 
	}

	files.inputFileName = argv[1];
	files.outputFileName = argv[2];

	return 0;
}


/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	FileInOut files = { NULL };
	if (hello(argc, argv, files))
	{
		return -1;
	}

	return 0;
}