#include "Encoder.h"
#include "VideoEncodingHeader.h"

/*************************************************
	Function:		setContext
	Description:	根据输入参数设置编码器上下文
	Calls:			av_opt_set
	Called By:		Open_encoder
	Input:			(in)io_param : 命令行输入参数
	Output:			(out)ctx : 编码器上下文部件					
	Return:			无
*************************************************/
void setContext(CodecCtx &ctx, IOParam io_param)
{
	/* put sample parameters */
	ctx.c->bit_rate = io_param.nBitRate;
    /* resolution must be a multiple of two */
	ctx.c->width = io_param.nImageWidth;
	ctx.c->height = io_param.nImageHeight;
    /* frames per second */
	AVRational rational = {1,25};
	ctx.c->time_base = rational;
    /* emit one intra frame every ten frames
     * check frame pict_type before passing frame
     * to encoder, if frame->pict_type is AV_PICTURE_TYPE_I
     * then gop_size is ignored and the output of encoder
     * will always be I frame irrespective to gop_size
     */
	ctx.c->gop_size = io_param.nGOPSize;
	ctx.c->max_b_frames = io_param.nMaxBFrames;
	ctx.c->pix_fmt = AV_PIX_FMT_YUV420P;

	av_opt_set(ctx.c->priv_data, "preset", "slow", 0);
}

bool Open_encoder(CodecCtx &ctx, IOParam io_param)
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

	setContext(ctx,io_param);

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

void Close_encoder(CodecCtx &ctx)
{
	avcodec_close(ctx.c);
	av_free(ctx.c);
	av_freep(&(ctx.frame->data[0]));
	av_frame_free(&(ctx.frame));
}

