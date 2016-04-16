#include "common.h"
#include "IOFile.h"
#include "DemuxerDecoder.h"
#include "VideoFilter.h"
#include "MuxerEncoder.h"

const int c_maxDecodedFrames = 2000;
const int c_outputTempYUV = 1;

/*************************************************
Function:		hello
Description:	输出提示信息和命令行格式
Calls:			无
Called By:		main
Input:			无
Output:			无
Return:			无
*************************************************/
static int hello(IOFileName &files, int argc, char **argv)
{
	if (argc != 3) 
	{
		fprintf(stderr, "usage: %s input_file video_output_file\n", argv[0]);
		return -1;
	}

	files.src_filename = argv[1];
	files.dst_filename = argv[2];

	printf("Watermarking %s to %s.\n", files.src_filename, files.dst_filename);
	return 0;
}

void test_AVFrame_callback(AVFrame *frame)
{
	printf("test_AVFrame_callback is called!\n");
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	int frameCnt = 0;
	IOFileName files = {NULL};

	//输出提示信息
	if(hello(files, argc, argv) < 0)
	{
		printf("Error: Command line format error.\n");
		return -1;
	}

	//注册FFMpeg组件
	av_register_all();

	//初始化解复用-解码器
	if(InitDemuxerDecoder(files.src_filename, c_outputTempYUV) < 0)
	{
		goto end;
	}
	if (InitFramePacket() < 0)
	{
		goto end;
	}

	//初始化视频滤镜
	if (Init_video_filters(GetAVCodecContextAddress()))
	{
		goto end;
	}

	if (InitMuxerEncoder(files.dst_filename, GetAVCodecContextAddress()))
	{
		goto end;
	}

	//解复用-解码循环
	while(ReadFramesFromFile())
	{
		if (DecodeOutFrames(c_outputTempYUV, AVFrame_filter_callback, FetchAVFrameToEncoder))
		{
			EncodeOneFrame();
		}

		if (frameCnt++ > c_maxDecodedFrames)
		{
			break;
		}
	}

end:
	CloseDemuxerDecoder();
	Close_video_filters();
	CloseMuxerEncoder();
	return -1;
}