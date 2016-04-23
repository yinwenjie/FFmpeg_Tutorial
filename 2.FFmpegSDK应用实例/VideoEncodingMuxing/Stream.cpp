#include "common.h"
#include "Stream.h"




/* Add an output stream. */
static void add_stream(OutputStream *ost, AVFormatContext *oc,	AVCodec **codec, enum AVCodecID codec_id)
{
	AVCodecContext *c;
	int i;

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
	c = ost->st->codec;

	switch ((*codec)->type)
	{
	case AVMEDIA_TYPE_AUDIO:
		c->sample_fmt = (*codec)->sample_fmts ? (*codec)->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
		c->bit_rate = 64000;
		c->sample_rate = 44100;

		if ((*codec)->supported_samplerates)
		{
			c->sample_rate = (*codec)->supported_samplerates[0];
			for (i = 0; (*codec)->supported_samplerates[i]; i++)
			{
				if ((*codec)->supported_samplerates[i] == 44100)
					c->sample_rate = 44100;
			}
		}

		c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
		c->channel_layout = AV_CH_LAYOUT_STEREO;
		if ((*codec)->channel_layouts)
		{
			c->channel_layout = (*codec)->channel_layouts[0];
			for (i = 0; (*codec)->channel_layouts[i]; i++)
			{
				if ((*codec)->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
					c->channel_layout = AV_CH_LAYOUT_STEREO;
			}
		}
		c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
		{
			AVRational r = { 1, c->sample_rate };
			ost->st->time_base = r;
		}
		break;

	case AVMEDIA_TYPE_VIDEO:
		c->codec_id = codec_id;

		c->bit_rate = 400000;
		/* Resolution must be a multiple of two. */
		c->width = 352;
		c->height = 288;
		/* timebase: This is the fundamental unit of time (in seconds) in terms
		* of which frame timestamps are represented. For fixed-fps content,
		* timebase should be 1/framerate and timestamp increments should be
		* identical to 1. */
		{
			AVRational r = { 1, STREAM_FRAME_RATE };
			ost->st->time_base = r;
		}
		c->time_base = ost->st->time_base;

		c->gop_size = 12; /* emit one intra frame every twelve frames at most */
		c->pix_fmt = AV_PIX_FMT_YUV420P;
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
		break;

	default:
		break;
	}

	/* Some formats want stream headers to be separate. */
	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
		c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
}


int Add_audio_video_streams(OutputStream *video_st, OutputStream *audio_st, AVFormatContext *oc, AVOutputFormat *fmt, AVCodec *audio_codec, AVCodec *video_codec, IOParam &io)
{
	int ret = 0;
	if (fmt->video_codec != AV_CODEC_ID_NONE)
	{
		add_stream(video_st, oc, &video_codec, fmt->video_codec);
		video_st->st->codec->width = io.frame_width;
		video_st->st->codec->height = io.frame_height;
		ret |= HAVE_VIDEO;
		ret |= ENCODE_VIDEO;
	}
	if (fmt->audio_codec != AV_CODEC_ID_NONE)
	{
		add_stream(audio_st, oc, &audio_codec, fmt->audio_codec);
		ret |= HAVE_AUDIO;
		ret |= ENCODE_AUDIO;
	}

	return ret;
}

void Close_stream(AVFormatContext *oc, OutputStream *ost)
{
	avcodec_close(ost->st->codec);
	av_frame_free(&ost->frame);
	av_frame_free(&ost->tmp_frame);
	sws_freeContext(ost->sws_ctx);
	swr_free(&ost->swr_ctx);
}
