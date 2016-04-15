#include "VideoMuxing.h"
#include "VideoEncoding.h"

static int add_stream(OutputStream *ost, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id)
{
	/* find the encoder */
	*codec = avcodec_find_encoder(codec_id);
	if (!(*codec))
	{
		fprintf(stderr, "Could not find encoder for '%s'\n",
			avcodec_get_name(codec_id));
		exit(1);
	}

	ost->st = avformat_new_stream(oc, *codec);
	if (!ost->st)
	{
		fprintf(stderr, "Could not allocate stream\n");
		exit(1);
	}
	ost->st->id = oc->nb_streams - 1;
	
	AVRational r = { 1, STREAM_FRAME_RATE };
	ost->st->time_base = r;	
};

static int set_output_avcodeccontext(OutputStream *ost, DemuxingVideoAudioContex demux_ctx)
{
	AVCodecContext *c = ost->st->codec;
	AVCodecContext *c_in = demux_ctx.video_dec_ctx;

	c->codec_id = c_in->codec_id;
	c->bit_rate = c_in->bit_rate;
	c->width = c_in->width;
	c->height = c_in->height;
	c->time_base = c_in->time_base;
	c->gop_size = c_in->gop_size;
	c->pix_fmt = c_in->pix_fmt;
	if (c->codec_id == AV_CODEC_ID_MPEG2VIDEO)
	{
		/* just for testing, we also add B frames */
		c->max_b_frames = 2;
	}
	if (c->codec_id == AV_CODEC_ID_MPEG1VIDEO)
	{
		/* Needed to avoid using macroblocks in which some coeffs overflow.
		* This does not happen with normal video, it just happens here as
		* the motion of the chroma plane does not match the luma plane. */
		c->mb_decision = 2;
	}

	return 0;
}

int Add_video_stream(EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx)
{
	OutputStream *ost = &(enc_ctx.video_st);
	add_stream(ost, enc_ctx.oc, &(enc_ctx.video_codec), enc_ctx.fmt->video_codec);
	set_output_avcodeccontext(ost,demux_ctx);

	AVCodecContext *c = ost->st->codec;
	/* Some formats want stream headers to be separate. */
	if (enc_ctx.oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	return 0;
}
