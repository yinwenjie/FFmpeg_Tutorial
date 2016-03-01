#include "Encoder.h"
#include "VideoEncodingHeader.h"

void setContext(Codec_Ctx &ctx)
{
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
}

bool OpenEncoder(Codec_Ctx &ctx)
{
	int ret;

	avcodec_register_all();

	/* find the mpeg1 video encoder */
	ctx.codec = avcodec_find_encoder(AV_CODEC_ID_H264);
	if (!ctx.codec) 
	{
		fprintf(stderr, "Codec not found\n");
		return false;
	}

	ctx.c = avcodec_alloc_context3(ctx.codec);
	if (!ctx.c)
	{
		fprintf(stderr, "Could not allocate video codec context\n");
		return false;
	}

	setContext(ctx);

	/* open it */
	if (avcodec_open2(ctx.c, ctx.codec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		exit(1);
	}

	ctx.frame = av_frame_alloc();
	if (!ctx.frame) {
        fprintf(stderr, "Could not allocate video frame\n");
        return false;
    }
	ctx.frame->format = ctx.c->pix_fmt;
	ctx.frame->width = ctx.c->width;
	ctx.frame->height = ctx.c->height;

    /* the image can be allocated by any means and av_image_alloc() is
     * just the most convenient way if av_malloc() is to be used */
	ret = av_image_alloc(ctx.frame->data, ctx.frame->linesize, ctx.c->width, ctx.c->height, ctx.c->pix_fmt, 32);

	if (ret < 0) {
		fprintf(stderr, "Could not allocate raw picture buffer\n");
		return false;
	}

	return true;
}

void CloseEncoder(Codec_Ctx &ctx)
{
	avcodec_close(ctx.c);
	av_free(ctx.c);
	av_freep(&(ctx.frame->data[0]));
	av_frame_free(&(ctx.frame));
}

