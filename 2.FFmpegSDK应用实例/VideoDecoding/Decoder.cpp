#include "Decoder.h"
#include "VideoDecodingHeader.h"

bool Open_deocder(CodecCtx &ctx)
{
	//注册编解码器对象
	avcodec_register_all();	

	//初始化AVPacket对象
	av_init_packet(&(ctx.pkt));

	//根据CODEC_ID查找AVCodec对象
	ctx.pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!ctx.pCodec) 
	{
		fprintf(stderr, "Codec not found\n");
		return false;
	}

	//根据AVCodec对象分配AVCodecContext
	ctx.pCodecContext = avcodec_alloc_context3(ctx.pCodec);
	if (!ctx.pCodecContext)
	{
		fprintf(stderr, "Could not allocate video codec context\n");
		return false;
	}

	if (ctx.pCodec->capabilities & AV_CODEC_CAP_TRUNCATED)
		ctx.pCodecContext->flags |= AV_CODEC_FLAG_TRUNCATED; // we do not send complete frames

	//根据CODEC_ID初始化AVCodecParserContext对象
	ctx.pCodecParserCtx = av_parser_init(AV_CODEC_ID_H264);
	if (!ctx.pCodecParserCtx)
	{
		printf("Could not allocate video parser context\n");
		return false;
	}

	//打开AVCodec对象
	if (avcodec_open2(ctx.pCodecContext, ctx.pCodec, NULL) < 0)
	{
		fprintf(stderr, "Could not open codec\n");
		return false;
	}

	//分配AVFrame对象
	ctx.frame = av_frame_alloc();
	if (!ctx.frame) 
	{
		fprintf(stderr, "Could not allocate video frame\n");
		return false;
	}

	return true;
}

void Close_decoder(CodecCtx &ctx)
{
	avcodec_close(ctx.pCodecContext);
	av_free(ctx.pCodecContext);
	av_frame_free(&(ctx.frame));
}
