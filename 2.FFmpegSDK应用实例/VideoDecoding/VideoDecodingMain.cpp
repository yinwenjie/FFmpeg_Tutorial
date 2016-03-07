#include <stdio.h>

#include "VideoDecodingHeader.h"

#define INBUF_SIZE 4096
#define AUDIO_INBUF_SIZE 20480
#define AUDIO_REFILL_THRESH 4096

void hello()
{
	printf("*********************************\n");
	printf("VideoDecoding: A FFmpeg SDK demo.\nDeveloped by Yin Wenjie\n");
	printf("*********************************\n");
	printf("=================================\nCompulsory Paramaters:\n");
	printf("\t-i:\tInput YUV file name\n");
	printf("\t-o:\tOutput stream file name\n");
	printf("*********************************\n");
}


int main(int argc, char **argv)
{
	hello();

	avcodec_register_all();

	Codec_Ctx ctx;
	
	int frame_count;
	FILE *f;
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

	f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "Could not open %s\n", argv[1]);
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

    }
	
	/* some codecs, such as MPEG, transmit the I and P frame with a
    latency of one frame. You must do the following to have a
    chance to get the last frame of the video */

    ctx.pkt.data = NULL;
    ctx.pkt.size = 0;
//    decode_write_frame(argv[2], c, frame, &frame_count, &avpkt, 1);

    fclose(f);

    avcodec_close(ctx.pCodecContext);
    av_free(ctx.pCodecContext);
    av_frame_free(&(ctx.frame));
    printf("\n");
}