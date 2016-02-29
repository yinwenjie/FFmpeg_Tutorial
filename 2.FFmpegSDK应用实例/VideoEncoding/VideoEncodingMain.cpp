#include <stdio.h>
#include "VideoEncodingHeader.h"


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
	
	avcodec_register_all();
	/* find the mpeg1 video encoder */
	ctx.codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!ctx.codec) 
	{
		fprintf(stderr, "Codec not found\n");
		exit(1);
	}

	ctx.c = avcodec_alloc_context3(ctx.codec);
	if (!ctx.c)
	{
		fprintf(stderr, "Could not allocate video codec context\n");
		exit(1);
	}
	/* put sample parameters */
	ctx.c->bit_rate = 400000;
    /* resolution must be a multiple of two */
	ctx.c->width = 352;
	ctx.c->height = 288;
    /* frames per second */
	AVRational rational = {1,25};
	ctx.c->time_base = rational;
    /* emit one intra frame every ten frames
     * check frame pict_type before passing frame
     * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
     * then gop_size is ignored and the output of encoder
     * will always be I frame irrespective to gop_size
     */
	ctx.c->gop_size = 10;
	ctx.c->max_b_frames = 1;
	ctx.c->pix_fmt = AV_PIX_FMT_YUV420P;

	av_opt_set(ctx.c->priv_data, "preset", "slow", 0);

    /* open it */
	if (avcodec_open2(ctx.c, ctx.codec, NULL) < 0) {
        fprintf(stderr, "Could not open codec\n");
        exit(1);
    }

    f = fopen("out.h264", "wb");
    if (!f) {
        fprintf(stderr, "Could not open %s\n", "out.h264");
        exit(1);
    }

	ctx.frame = av_frame_alloc();
	if (!ctx.frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        exit(1);
    }
	ctx.frame->format = ctx.c->pix_fmt;
	ctx.frame->width = ctx.c->width;
	ctx.frame->height = ctx.c->height;

    /* the image can be allocated by any means and av_image_alloc() is
     * just the most convenient way if av_malloc() is to be used */
	ret = av_image_alloc(ctx.frame->data, ctx.frame->linesize, ctx.c->width, ctx.c->height, ctx.c->pix_fmt, 32);

	if (ret < 0) {
		fprintf(stderr, "Could not allocate raw picture buffer\n");
		exit(1);
	}

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
			av_packet_unref(&pkt);
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

	avcodec_close(ctx.c);
	av_free(ctx.c);
	av_freep(&(ctx.frame->data[0]));
	av_frame_free(&(ctx.frame));

	return 0;
}