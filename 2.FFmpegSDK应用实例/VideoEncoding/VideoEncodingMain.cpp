#include <stdio.h>
#include "Encoder.h"

void hello()
{
	printf("*********************************\n");
	printf("VideoEncoding: A FFmpeg SDK demo.\nDeveloped by Yin Wenjie\n");
	printf("*********************************\n");
}

int main(int argc, char **argv)
{
	CodecCtx ctx = { NULL, NULL, NULL};
	int i, ret, x, y, got_output;
	FILE *f;

	OpenFile(f);

	hello();	

	OpenEncoder(ctx);

	/* encode 1 second of video */
	for (i = 0; i < 25; i++) {
		av_init_packet(&(ctx.pkt));
		ctx.pkt.data = NULL;    // packet data will be allocated by the encoder
		ctx.pkt.size = 0;

		fflush(stdout);
		/* prepare a dummy image */
		/* Y */
		for (y = 0; y < ctx.c->height; y++) {
			for (x = 0; x < ctx.c->width; x++) {
				ctx.frame->data[0][y * ctx.frame->linesize[0] + x] = x + y + i * 3;
			}
		}

		/* Cb and Cr */
		for (y = 0; y < ctx.c->height / 2; y++) {
			for (x = 0; x < ctx.c->width / 2; x++) {
				ctx.frame->data[1][y * ctx.frame->linesize[1] + x] = 128 + y + i * 2;
				ctx.frame->data[2][y * ctx.frame->linesize[2] + x] = 64 + x + i * 5;
			}
		}

		ctx.frame->pts = i;

		/* encode the image */
		ret = avcodec_encode_video2(ctx.c, &(ctx.pkt), ctx.frame, &got_output);
		if (ret < 0) {
			fprintf(stderr, "Error encoding frame\n");
			exit(1);
		}

		if (got_output) {
			printf("Write frame %3d (size=%5d)\n", i, ctx.pkt.size);
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, f);
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
			fwrite(ctx.pkt.data, 1, ctx.pkt.size, f);
			av_packet_unref(&(ctx.pkt));
		}
	}

	/* add sequence end code to have a real mpeg file */
//	fwrite(endcode, 1, sizeof(endcode), f);
	fclose(f);

	CloseEncoder(ctx);

	return 0;
}