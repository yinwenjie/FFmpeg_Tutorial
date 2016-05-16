#include "EncodingMuxingAudio.h"

static AudioSignalGenerator g_audioContext = {0};

static AVFrame *alloc_audio_frame(enum AVSampleFormat sample_fmt, uint64_t channel_layout,	int sample_rate, int nb_samples)
{
	AVFrame *frame = av_frame_alloc();
	int ret;

	if (!frame)
	{
		fprintf(stderr, "Error allocating an audio frame\n");
		exit(1);
	}

	frame->format = sample_fmt;
	frame->channel_layout = channel_layout;
	frame->sample_rate = sample_rate;
	frame->nb_samples = nb_samples;

	if (nb_samples)
	{
		ret = av_frame_get_buffer(frame, 0);
		if (ret < 0)
		{
			fprintf(stderr, "Error allocating an audio buffer\n");
			exit(1);
		}
	}

	return frame;
}


void Open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg)
{
	AVCodecContext *c;
	int nb_samples;
	int ret;
	AVDictionary *opt = NULL;

	c = ost->st->codec;

	/* open it */
	av_dict_copy(&opt, opt_arg, 0);
	ret = avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);
	if (ret < 0) 
	{
		fprintf(stderr, "Could not open audio codec: %d\n", ret);
		exit(1);
	}

	/* init signal generator */
	ost->t = 0;
	ost->tincr = 2 * M_PI * 110.0 / c->sample_rate;
	/* increment frequency by 110 Hz per second */
	ost->tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

	if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
		nb_samples = 10000;
	else
		nb_samples = c->frame_size;

	ost->frame = alloc_audio_frame(c->sample_fmt, c->channel_layout, c->sample_rate, nb_samples);
	ost->tmp_frame = alloc_audio_frame(AV_SAMPLE_FMT_S16, c->channel_layout, c->sample_rate, nb_samples);

	/* create resampler context */
	ost->swr_ctx = swr_alloc();
	if (!ost->swr_ctx) 
	{
		fprintf(stderr, "Could not allocate resampler context\n");
		exit(1);
	}

	/* set options */
	av_opt_set_int(ost->swr_ctx, "in_channel_count", c->channels, 0);
	av_opt_set_int(ost->swr_ctx, "in_sample_rate", c->sample_rate, 0);
	av_opt_set_sample_fmt(ost->swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	av_opt_set_int(ost->swr_ctx, "out_channel_count", c->channels, 0);
	av_opt_set_int(ost->swr_ctx, "out_sample_rate", c->sample_rate, 0);
	av_opt_set_sample_fmt(ost->swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

	/* initialize the resampling context */
	if ((ret = swr_init(ost->swr_ctx)) < 0)
	{
		fprintf(stderr, "Failed to initialize the resampling context\n");
		exit(1);
	}
}

static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/* Write the compressed frame to the media file. */
	//	log_packet(fmt_ctx, pkt);
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

/* Prepare a 16 bit dummy audio frame of 'frame_size' samples and
* 'nb_channels' channels. */
static AVFrame *get_audio_frame(OutputStream *ost)
{
	AVFrame *frame = ost->tmp_frame;
	int j, i, v;
	int16_t *q = (int16_t*)frame->data[0];

	{
		AVRational r = { 1, 1 };
		/* check if we want to generate more frames */
		if (av_compare_ts(ost->next_pts, ost->st->codec->time_base,	STREAM_DURATION, r) >= 0)
			return NULL;
	}

	for (j = 0; j < frame->nb_samples; j++)
	{
		v = (int)(sin(ost->t) * 10000);
		for (i = 0; i < ost->st->codec->channels; i++)
			*q++ = v;
		ost->t += ost->tincr;
		ost->tincr += ost->tincr2;
	}

	frame->pts = ost->next_pts;
	ost->next_pts += frame->nb_samples;

	return frame;
}

int Write_audio_frame(AVFormatContext *oc, OutputStream *ost)
{
	AVCodecContext *c;
	AVPacket pkt = { 0 }; // data and size must be 0;
	AVFrame *frame;
	int ret;
	int got_packet;
	int dst_nb_samples;

	av_init_packet(&pkt);
	c = ost->st->codec;

	frame = get_audio_frame(ost);

	if (frame)
	{
		/* convert samples from native format to destination codec format, using the resampler */
		/* compute destination number of samples */
		dst_nb_samples = av_rescale_rnd(swr_get_delay(ost->swr_ctx, c->sample_rate) + frame->nb_samples, c->sample_rate, c->sample_rate, AV_ROUND_UP);
		av_assert0(dst_nb_samples == frame->nb_samples);

		/* when we pass a frame to the encoder, it may keep a reference to it
		* internally;
		* make sure we do not overwrite it here
		*/
		ret = av_frame_make_writable(ost->frame);
		if (ret < 0)
			exit(1);

		/* convert to destination format */
		ret = swr_convert(ost->swr_ctx,
			ost->frame->data, dst_nb_samples,
			(const uint8_t **)frame->data, frame->nb_samples);
		if (ret < 0)
		{
			fprintf(stderr, "Error while converting\n");
			exit(1);
		}
		frame = ost->frame;

		{
			AVRational r = { 1, c->sample_rate };
			frame->pts = av_rescale_q(ost->samples_count, r, c->time_base);
		}

		ost->samples_count += dst_nb_samples;
	}

	ret = avcodec_encode_audio2(c, &pkt, frame, &got_packet);
	if (ret < 0)
	{
		fprintf(stderr, "Error encoding audio frame: %d\n", ret);
		exit(1);
	}

	if (got_packet) 
	{
		ret = write_frame(oc, &c->time_base, ost->st, &pkt);
		if (ret < 0) 
		{
			fprintf(stderr, "Error while writing audio frame: %d\n", ret);
			exit(1);
		}
	}

	return (frame || got_packet) ? 0 : 1;
}

int Add_Audio_stream(AVStream **audioStream, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id)
{
	AVCodecContext *codecCtx = NULL;

	//查找编解码器
	if(!(*codec = avcodec_find_encoder(codec_id)))
	{
		printf("Error: Failed to find encoder while adding video stream.\n");
		return 0;
	}

	//生成音频流AVStream结构
	if (!(*audioStream = avformat_new_stream(oc, *codec)))
	{
		printf("Error: Failed to generate audio stream while adding audio stream.\n");
		return 0;
	}
	(*audioStream)->id = oc->nb_streams -1;
	codecCtx = (*audioStream)->codec;
	codecCtx->codec_id = codec_id;

	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
	{
		codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	return 1;
}

void Set_audio_stream(AVStream **audioStream, const AVCodec *codec)
{
	AVCodecContext *c = (*audioStream)->codec;

	c->sample_fmt = codec->sample_fmts ? codec->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
	c->bit_rate = 64000;
	c->sample_rate = 44100;

	if ( codec->supported_samplerates )
	{
		c->sample_rate = codec->supported_samplerates[0];
		for (int i = 0; codec->supported_samplerates[i]; i++)
		{
			if (codec->supported_samplerates[i] == 44100)
			{
				c->sample_rate = 44100;
			}
		}
	}

	c->channels = av_get_channel_layout_nb_channels(c->channel_layout);
	c->channel_layout = AV_CH_LAYOUT_STEREO;
	if (codec->channel_layouts)
	{
		c->channel_layout = codec->channel_layouts[0];
		for (int i = 0; codec->channel_layouts[i]; i++)
		{
			if (codec->channel_layouts[i] == AV_CH_LAYOUT_STEREO)
			{
				c->channel_layout = AV_CH_LAYOUT_STEREO;
			}
		}
	}
	c->channels = av_get_channel_layout_nb_channels(c->channel_layout);

	AVRational r = { 1, c->sample_rate };
	(*audioStream)->time_base = r;
}

int Open_audio_stream(AVStream **audioStream, AVFrame **audioFrame, AVCodec *codec, IOParam io)
{
	AVCodecContext *c = (*audioStream)->codec;
	int nb_samples = 0;
	int ret = avcodec_open2(c, codec, NULL);
	if (ret < 0)
	{
		printf("Error: Could not open video encoder in stream #%d", (*audioStream)->id);
		return 0;
	}

	g_audioContext.t = 0;
	g_audioContext.tincr = 2 * M_PI * 110.0 / c->sample_rate;
	g_audioContext.tincr2 = 2 * M_PI * 110.0 / c->sample_rate / c->sample_rate;

	if (c->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
		nb_samples = 10000;
	else
		nb_samples = c->frame_size;

	*audioFrame = alloc_audio_frame(c->sample_fmt, c->channel_layout, c->sample_rate, nb_samples);

	g_audioContext.swr_ctx = swr_alloc();
	if (!g_audioContext.swr_ctx)
	{
		printf("Error: Could not allocate resampler context.\n");
		return 0;
	}

	/* set options */
	av_opt_set_int(g_audioContext.swr_ctx, "in_channel_count", c->channels, 0);
	av_opt_set_int(g_audioContext.swr_ctx, "in_sample_rate", c->sample_rate, 0);
	av_opt_set_sample_fmt(g_audioContext.swr_ctx, "in_sample_fmt", AV_SAMPLE_FMT_S16, 0);
	av_opt_set_int(g_audioContext.swr_ctx, "out_channel_count", c->channels, 0);
	av_opt_set_int(g_audioContext.swr_ctx, "out_sample_rate", c->sample_rate, 0);
	av_opt_set_sample_fmt(g_audioContext.swr_ctx, "out_sample_fmt", c->sample_fmt, 0);

	/* initialize the resampling context */
	if ((ret = swr_init(g_audioContext.swr_ctx)) < 0)
	{
		fprintf(stderr, "Failed to initialize the resampling context\n");
		exit(1);
	}

	return 1;
}
