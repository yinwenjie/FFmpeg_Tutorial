#include "common.h"


/*************************************************
	Function:		hello
	Description:	解析命令行传入的参数
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)files : 解析命令行的结果
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
static bool hello(int argc, char **argv, IOFiles &files)
{
	printf("FFMpeg Filtering Demo.\nCommand format: %s inputfile outputfile logofile\n", argv[0]);
	if (argc != 4)
	{
		printf("Error: command line error, please re-check.\n");
		return false;
	}

	files.inputName = argv[1];
	files.outputName = argv[2];
	files.logoName = argv[3];

	fopen_s(&(files.inputFile), files.inputName, "rb+");
	if (!(files.inputFile))
	{
		printf("Error: open input file failed.\n");
		return false;
	}
	fopen_s(&(files.outputFile), files.outputName, "wb+");
	if (!(files.outputFile))
	{
		printf("Error: open output file failed.\n");
		return false;
	}

	return true;
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
const char *filter_descr = "movie=logo.png[wm];[in][wm]overlay=5:5[out]";

int main(int argc, char **argv)
{
	IOFiles files = { NULL };
	if (!hello(argc, argv, files))
	{
		return -1;
	}

	av_register_all();
	if (init_filters(filter_descr) < 0)
	{
		return -1;
	}

	AVFrame *frameIn = avframe_allocation(720, 480, AV_PIX_FMT_YUV420P);

	avframe_deallocation(frameIn);

	return 0;
}