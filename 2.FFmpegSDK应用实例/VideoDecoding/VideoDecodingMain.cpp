#include <stdio.h>

#include "InputOutput.h"
#include "Decoder.h"

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
	printf("VideoDecoding: A FFmpeg SDK demo.\nDeveloped by Yin Wenjie\n");
	printf("*********************************\n");
	printf("=================================\nCompulsory Paramaters:\n");
	printf("Input YUV file name");
	printf("\tOutput stream file name\n");
	printf("*********************************\n");
}


/*************************************************
Function:		write_out_yuv_frame
Description:	将解码完成的YUV数据写出到文件
Calls:			无
Called By:		main
Input:			(in)ctx : 获取AVFrame中的数据地址
Output:			(out)in_out : 写出到in_out指定的输出文件
Return:			无
*************************************************/
void write_out_yuv_frame(const CodecCtx &ctx, IOParam &in_out)
{
	uint8_t **pBuf	= ctx.frame->data;
	int*	pStride = ctx.frame->linesize;
	
	for (int color_idx = 0; color_idx < 3; color_idx++)
	{
		int		nWidth	= color_idx == 0 ? ctx.frame->width : ctx.frame->width / 2;
		int		nHeight = color_idx == 0 ? ctx.frame->height : ctx.frame->height / 2;
		for(int idx=0;idx < nHeight; idx++)
		{
			fwrite(pBuf[color_idx],1, nWidth, in_out.pFout);
			pBuf[color_idx] += pStride[color_idx];
		}
		fflush(in_out.pFout);
	}
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	uint8_t *pDataPtr = NULL;
	int uDataSize = 0;
	int got_picture, len;

	CodecCtx ctx;
	IOParam inputoutput;

	hello();
	
	Parse(argc, argv, inputoutput);

	Open_files(inputoutput);
	
	uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];
	
	memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);
	
	printf("Decode video file %s to %s\n", argv[1], argv[2]);

	OpenDeocder(ctx);    

	while(1)
	{
		uDataSize = fread_s(inbuf,INBUF_SIZE, 1, INBUF_SIZE, inputoutput.pFin);
		if (0 == uDataSize)
		{
			break;
		}

		pDataPtr = inbuf;

		while(uDataSize > 0)
		{
			len = av_parser_parse2(ctx.pCodecParserCtx, ctx.pCodecContext, 
										&(ctx.pkt.data), &(ctx.pkt.size), 
										pDataPtr, uDataSize, 
										AV_NOPTS_VALUE, AV_NOPTS_VALUE, AV_NOPTS_VALUE);
			pDataPtr += len;
			uDataSize -= len;

			if (0 == ctx.pkt.size)
			{
				continue;
			}

			int ret = avcodec_decode_video2(ctx.pCodecContext, ctx.frame, &got_picture, &(ctx.pkt));
			if (ret < 0) 
			{
				printf("Decode Error.\n");
				return ret;
			}

			if (got_picture) 
			{
				write_out_yuv_frame(ctx, inputoutput);
				printf("Succeed to decode 1 frame!\n");
			}
		} //while(uDataSize > 0)
	}

    ctx.pkt.data = NULL;
    ctx.pkt.size = 0;
	while(1)
	{
		int ret = avcodec_decode_video2(ctx.pCodecContext, ctx.frame, &got_picture, &(ctx.pkt));
		if (ret < 0) 
		{
			printf("Decode Error.\n");
			return ret;
		}

		if (got_picture) 
		{
			write_out_yuv_frame(ctx, inputoutput);
			printf("Flush Decoder: Succeed to decode 1 frame!\n");
		}
		else
		{
			break;
		}
	} //while(1)

	Close_files(inputoutput);
	CloseDecoder(ctx);

	return 1;
}