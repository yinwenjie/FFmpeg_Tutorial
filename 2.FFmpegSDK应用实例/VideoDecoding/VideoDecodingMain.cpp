#include <stdio.h>

#include "VideoDecodingHeader.h"
#include "InputOutput.h"

#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

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

void write_out_yuv_frame(const Codec_Ctx &ctx, IO_Param &in_out)
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

int main(int argc, char **argv)
{
	uint8_t *pDataPtr = NULL;
	int uDataSize = 0;
	int got_picture, len;

	Codec_Ctx ctx;
	IO_Param inputoutput;

	hello();
	
	Parse(argc, argv, inputoutput);

	OpenFiles(inputoutput);

	avcodec_register_all();	
	
	int frame_count;
	uint8_t inbuf[INBUF_SIZE + AV_INPUT_BUFFER_PADDING_SIZE];

	av_init_packet(&(ctx.pkt));

	/* set end of buffer to 0 (this ensures that no overreading happens for damaged mpeg streams) */
	memset(inbuf + INBUF_SIZE, 0, AV_INPUT_BUFFER_PADDING_SIZE);

	printf("Decode video file %s to %s\n", argv[1], argv[2]);

	/* find the mpeg1 video decoder */
    ctx.pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!ctx.pCodec) {
        fprintf(stderr, "Codec not found\n");
        exit(1);
    }

    ctx.pCodecContext = avcodec_alloc_context3(ctx.pCodec);
    if (!ctx.pCodecContext) {
        fprintf(stderr, "Could not allocate video codec context\n");
        exit(1);
    }

    if (ctx.pCodec->capabilities & AV_CODEC_CAP_TRUNCATED)
        ctx.pCodecContext->flags |= AV_CODEC_FLAG_TRUNCATED; // we do not send complete frames

	ctx.pCodecParserCtx = av_parser_init(AV_CODEC_ID_H264);
	if (!ctx.pCodecParserCtx){
		printf("Could not allocate video parser context\n");
		return -1;
	}

    /* open it */
    if (avcodec_open2(ctx.pCodecContext, ctx.pCodec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

	ctx.frame = av_frame_alloc();
	if (!ctx.frame) {
		fprintf(stderr, "Could not allocate video frame\n");
		exit(1);
	}

	frame_count = 0;
	for (;;) 
	{
//		uDataSize = fread(inbuf, 1, INBUF_SIZE, inputoutput.pFin);
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

			//Some Info from AVCodecParserContext
			printf("[Packet]Size:%6d\t",ctx.pkt.size);
			switch(ctx.pCodecParserCtx->pict_type)
			{
			case AV_PICTURE_TYPE_I: 
				printf("Type:I\t");
				break;
			case AV_PICTURE_TYPE_P: 
				printf("Type:P\t");
				break;
			case AV_PICTURE_TYPE_B: 
				printf("Type:B\t");
				break;
			default: 
				printf("Type:Other\t");
				break;
			}
			printf("Number:%4d\n",ctx.pCodecParserCtx->output_picture_number);

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
		}
	}

    	
	/* some codecs, such as MPEG, transmit the I and P frame with a
    latency of one frame. You must do the following to have a
    chance to get the last frame of the video */

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
		if (!got_picture)
			break;
		if (got_picture) 
		{
			write_out_yuv_frame(ctx, inputoutput);
			printf("Flush Decoder: Succeed to decode 1 frame!\n");
		}
	}

    fclose(inputoutput.pFin);
	fclose(inputoutput.pFout);

    avcodec_close(ctx.pCodecContext);
    av_free(ctx.pCodecContext);
    av_frame_free(&(ctx.frame));
    printf("\n");
}