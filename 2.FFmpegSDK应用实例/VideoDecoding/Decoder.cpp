#include "Decoder.h"
#include "VideoDecodingHeader.h"

bool OpenDeocder(Codec_Ctx &ctx)
{
	avcodec_register_all();	
	av_init_packet(&(ctx.pkt));

	ctx.pCodec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!ctx.pCodec) {
		fprintf(stderr, "Codec not found\n");
		return false;
	}

	ctx.pCodecContext = avcodec_alloc_context3(ctx.pCodec);
	if (!ctx.pCodecContext) {
		fprintf(stderr, "Could not allocate video codec context\n");
		return false;
	}

	if (ctx.pCodec->capabilities & AV_CODEC_CAP_TRUNCATED)
		ctx.pCodecContext->flags |= AV_CODEC_FLAG_TRUNCATED; // we do not send complete frames

	ctx.pCodecParserCtx = av_parser_init(AV_CODEC_ID_H264);
	if (!ctx.pCodecParserCtx){
		printf("Could not allocate video parser context\n");
		return false;
	}

	/* open it */
	if (avcodec_open2(ctx.pCodecContext, ctx.pCodec, NULL) < 0) {
		fprintf(stderr, "Could not open codec\n");
		return false;
	}

	ctx.frame = av_frame_alloc();
	if (!ctx.frame) {
		fprintf(stderr, "Could not allocate video frame\n");
		return false;
	}

	return true;
}

void CloseDecoder(Codec_Ctx &ctx)
{
	avcodec_close(ctx.pCodecContext);
	av_free(ctx.pCodecContext);
	av_frame_free(&(ctx.frame));
}
