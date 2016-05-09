#include "common.h"

/*************************************************
	Function:		hello
	Description:	解析命令行传入的参数
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)files : 输入输出文件对象
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
static bool  hello(int argc, char **argv, FileInOut &files)
{
	printf("FFMpeg Trans Coding Demo:\nCommand line format: %s inputFileName outputFileName.\n", argv[0]);
	if (argc != 3)
	{
		printf("Command line parameters error. Please re-check.\n");
		return false; 
	}

	files.inputFileName = argv[1];
	files.outputFileName = argv[2];

	return true;
}


/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	FileInOut files = { NULL };
	if (!hello(argc, argv, files))
	{
		printf("Error: Command line parameters error.\n");
		return -1;
	}

	TranscodingContext transCtx = {NULL};
	Init_transcoder(transCtx);

	if (!Open_input_file(files, transCtx))
	{
		printf("Error: Open input file failed.\n");
		return -1;
	}

	if (!Open_output_file(files, transCtx))
	{
		printf("Error: Open output file failed.\n");
		return -1;
	}

	/* read all packets */
	while (1) 
	{

	}

	return 0;
}