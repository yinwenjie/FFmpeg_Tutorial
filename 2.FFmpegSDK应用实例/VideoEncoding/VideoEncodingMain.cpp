#include <stdio.h>
#include "VideoEncodingHeader.h"
#include "Encoder.h"
#include "InputOutput.h"

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

int main(int argc, char **argv)
{
	hello();

	IO_Param io_param;
	ParseInputParam(argc, argv, io_param);
	
	Codec_Ctx ctx = { NULL, NULL, NULL};
	int i, ret, got_output;

	OpenFile(io_param);
	OpenEncoder(ctx, io_param);

	/* encode 1 second of video */
	for (i = 0; i < io_param.nTotalFrames; i++) 
	{
		av_init_packet(&(ctx.pkt));
		ctx.pkt.data = NULL;    // packet data will be allocated by the encoder
		ctx.pkt.size = 0;

		fflush(stdout);
		
		/* Y */
		ReadYUVData(ctx, io_param, 0);

		/* Cb and Cr */
		ReadYUVData(ctx, io_param, 1);
		ReadYUVData(ctx, io_param, 2);

		ctx.frame->pts = i;

		/* encode the image */
		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), ctx.frame, &got_output);
		if (ret < 0) {
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) {
			printf("Write frame %3d (size=%5d)\n", i, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
			av_packet_unref(&(ctx.pkt));
		}
	}
	/* get the delayed frames */
	for (got_output = 1; got_output; i++) {
		fflush(stdout);

		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), NULL, &got_output);
		if (ret < 0) {
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) {
			printf("Write frame %3d (size=%5d)\n", i, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, io_param.pFout);
			av_packet_unref(&(ctx.pkt));
		}
	}

	/* add sequence end code to have a real mpeg file */
//	fwrite(endcode, 1, sizeof(endcode), pFout);
	CloseFile(io_param);

	CloseEncoder(ctx);

	return 0;
}