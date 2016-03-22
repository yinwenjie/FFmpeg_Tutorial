#include <stdio.h>
#include "VideoEncodingHeader.h"
#include "Encoder.h"
#include "InputOutput.h"

/*************************************************
Function:		hello
Description:	输出提示信息和命令行格式
Calls:			无
Called By:		main
Input:			无
Output:			无
Return:			无
*************************************************/
void hello()
{
	printf("*********************************\n");
	printf("VideoEncoding: A FFmpeg SDK demo.\nDeveloped by Yin Wenjie\n");
	printf("*********************************\n");
	printf("=================================\nCompulsory Paramaters:\n");
	printf("\t-i:\tInput YUV file name\n");
	printf("\t-o:\tOutput stream file name\n");
	printf("\t-w:\tInput frame width\n");
	printf("\t-h:\tInput frame height\n");
	printf("\t-br:\tInput bit rate\n");
	printf("\t-tf:\tTotal frames to encode\n");
	printf("=================================\nOptional Paramaters:\n");
	printf("\t-fr:\tFrame rate\n");
	printf("\t-gs:\tGOP size\n");
	printf("\t-mbf:\tMax B Frames\n");
	printf("*********************************\n");
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	hello();									//输出提示信息

	IOParam io_param;
	if (Parse_input_param(argc, argv, io_param))//读取并解析命令行参数
	{
		printf("Error: Incomplete input parameters. Please check the command line.");
	}
	
	
	CodecCtx ctx = { NULL, NULL, NULL};
	int frameIdx, ret, got_output;

	Open_file(io_param);						//打开输入输出文件
	Open_encoder(ctx, io_param);				//根据输入参数设置并打开编码器各个部件

	/* encode 1 second of video */
	for (frameIdx = 0; frameIdx < io_param.nTotalFrames; frameIdx++)
	{
		av_init_packet(&(ctx.pkt));				//初始化AVPacket实例
		ctx.pkt.data = NULL;					// packet data will be allocated by the encoder
		ctx.pkt.size = 0;

		fflush(stdout);
				
		Read_yuv_data(ctx, io_param, 0);		//Y分量
		Read_yuv_data(ctx, io_param, 1);		//U分量
		Read_yuv_data(ctx, io_param, 2);		//V分量

		ctx.frame->pts = frameIdx;

		/* encode the image */
		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), ctx.frame, &got_output);	//将AVFrame中的像素信息编码为AVPacket中的码流
		if (ret < 0) 
		{
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) 
		{
			//获得一个完整的编码帧
			printf("Write frame %3d (size=%5d)\n", frameIdx, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
			av_packet_unref(&(ctx.pkt));
		}
	}

	/* get the delayed frames */
	for (got_output = 1; got_output; frameIdx++) 
	{
		fflush(stdout);

		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), NULL, &got_output);		//输出编码器中剩余的码流
		if (ret < 0)
		{
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) 
		{
			printf("Write frame %3d (size=%5d)\n", frameIdx, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
			av_packet_unref(&(ctx.pkt));
		}
	}

	//结尾处理
	Close_file(io_param);
	Close_encoder(ctx);

	return 0;
}