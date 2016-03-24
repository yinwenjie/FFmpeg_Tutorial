#include "DemuxingContext.h"


/* Enable or disable frame reference counting. You are not supposed to support
 * both paths in your application but pick the one most appropriate to your
 * needs. Look for the use of refcount in this example to see what are the
 * differences of API usage between them. */
int refcount = 0;

static int open_codec_context(IOFileName &files, DemuxingVideoAudioContex &va_ctx, enum AVMediaType type)
{
	int ret, stream_index;
	AVStream *st;
	AVCodecContext *dec_ctx = NULL;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	ret = av_find_best_stream(va_ctx.fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) 
	{
		fprintf(stderr, "Could not find %s stream in input file '%s'\n", av_get_media_type_string(type), files.src_filename);
		return ret;
	} 
	else 
	{
		stream_index = ret;
		st = va_ctx.fmt_ctx->streams[stream_index];

		/* find decoder for the stream */
		dec_ctx = st->codec;
		dec = avcodec_find_decoder(dec_ctx->codec_id);
		if (!dec) 
		{
			fprintf(stderr, "Failed to find %s codec\n", av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Init the decoders, with or without reference counting */
		av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
		if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0) 
		{
			fprintf(stderr, "Failed to open %s codec\n", av_get_media_type_string(type));
			return ret;
		}

		switch (type)
		{
		case AVMEDIA_TYPE_VIDEO:
			va_ctx.video_stream_idx = stream_index;
			va_ctx.video_stream = va_ctx.fmt_ctx->streams[stream_index];
			va_ctx.video_dec_ctx = va_ctx.video_stream->codec;
			break;
		case AVMEDIA_TYPE_AUDIO:
			va_ctx.audio_stream_idx = stream_index;
			va_ctx.audio_stream = va_ctx.fmt_ctx->streams[stream_index];
			va_ctx.audio_dec_ctx = va_ctx.audio_stream->codec;
			break;
		default:
			fprintf(stderr, "Error: unsupported MediaType: %s\n", av_get_media_type_string(type));
			return -1;
		}
	}

	return 0;
}


int InitDemuxContext(IOFileName &files, DemuxingVideoAudioContex &va_ctx)
{
	int ret = 0, width, height;
	enum AVPixelFormat pix_fmt;

	/* register all formats and codecs */
	av_register_all();

	/* open input file, and allocate format context */
	if (avformat_open_input(&(va_ctx.fmt_ctx), files.src_filename, NULL, NULL) < 0)
	{
		fprintf(stderr, "Could not open source file %s\n", files.src_filename);
		return -1;
	}

	/* retrieve stream information */
	if (avformat_find_stream_info(va_ctx.fmt_ctx, NULL) < 0) 
	{
		fprintf(stderr, "Could not find stream information\n");
		return -1;
	}

	if (open_codec_context(files, va_ctx, AVMEDIA_TYPE_VIDEO) >= 0) 
	{
		files.video_dst_file = fopen(files.video_dst_filename, "wb");
		if (!files.video_dst_file) 
		{
			fprintf(stderr, "Could not open destination file %s\n", files.video_dst_filename);
			return -1;
		}

		/* allocate image where the decoded image will be put */
		width = va_ctx.video_dec_ctx->width;
		height = va_ctx.video_dec_ctx->height;
		pix_fmt = va_ctx.video_dec_ctx->pix_fmt;
		ret = av_image_alloc(va_ctx.video_dst_data, va_ctx.video_dst_linesize, width, height, pix_fmt, 1);
		if (ret < 0) 
		{
			fprintf(stderr, "Could not allocate raw video buffer\n");
			return -1;
		}
		va_ctx.video_dst_bufsize = ret;
	}

	if (open_codec_context(files, va_ctx, AVMEDIA_TYPE_AUDIO) >= 0) 
	{
		files.audio_dst_file = fopen(files.audio_dst_filename, "wb");
		if (!files.audio_dst_file) 
		{
			fprintf(stderr, "Could not open destination file %s\n", files.audio_dst_filename);
			return -1;
		}
	}

	if (va_ctx.video_stream)
	{
		printf("Demuxing video from file '%s' into '%s'\n", files.src_filename, files.video_dst_filename);
	}

	if (va_ctx.audio_stream)
	{
		printf("Demuxing audio from file '%s' into '%s'\n", files.src_filename, files.audio_dst_filename);
	}

	/* dump input information to stderr */
	av_dump_format(va_ctx.fmt_ctx, 0, files.src_filename, 0);

	if (!va_ctx.audio_stream && !va_ctx.video_stream) 
	{
		fprintf(stderr, "Could not find audio or video stream in the input, aborting\n");
		return -1;
	}

	return 0;
}

void CloseDemuxContext(IOFileName &files, DemuxingVideoAudioContex &va_ctx)
{
	avcodec_close(va_ctx.video_dec_ctx);
	avcodec_close(va_ctx.audio_dec_ctx);
	avformat_close_input(&(va_ctx.fmt_ctx));
	if (files.video_dst_file)
		fclose(files.video_dst_file);
	if (files.audio_dst_file)
		fclose(files.audio_dst_file);
}

