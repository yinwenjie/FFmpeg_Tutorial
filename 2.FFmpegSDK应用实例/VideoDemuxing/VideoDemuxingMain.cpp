#include <stdio.h>
#include "DemuxingContext.h"

static int width, height;
static enum AVPixelFormat pix_fmt;

static uint8_t *video_dst_data[4] = {NULL};
static int video_dst_linesize[4];
static int video_dst_bufsize;

static AVFrame *frame = NULL;
static AVPacket pkt;
static int video_frame_count = 0;
static int audio_frame_count = 0;

extern int refcount;
static int get_format_from_sample_fmt(const char **fmt,	enum AVSampleFormat sample_fmt)
{
	int i;
	struct sample_fmt_entry {
		enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
	} sample_fmt_entries[] = {
		{ AV_SAMPLE_FMT_U8,  "u8",    "u8"    },
		{ AV_SAMPLE_FMT_S16, "s16be", "s16le" },
		{ AV_SAMPLE_FMT_S32, "s32be", "s32le" },
		{ AV_SAMPLE_FMT_FLT, "f32be", "f32le" },
		{ AV_SAMPLE_FMT_DBL, "f64be", "f64le" },
	};
	*fmt = NULL;

	for (i = 0; i < FF_ARRAY_ELEMS(sample_fmt_entries); i++) {
		struct sample_fmt_entry *entry = &sample_fmt_entries[i];
		if (sample_fmt == entry->sample_fmt) {
			*fmt = AV_NE(entry->fmt_be, entry->fmt_le);
			return 0;
		}
	}

	fprintf(stderr,
		"sample format %s is not supported as output format\n",
		av_get_sample_fmt_name(sample_fmt));
	return -1;
}

static int decode_packet(IOFileName &files, DemuxingVideoAudioContex &va_ctx, int *got_frame, int cached)
{
    int ret = 0;
    int decoded = pkt.size;

    *got_frame = 0;

    if (pkt.stream_index == va_ctx.video_stream_idx) 
	{
        /* decode video frame */
        ret = avcodec_decode_video2(va_ctx.video_dec_ctx, frame, got_frame, &pkt);
        if (ret < 0)
		{
            printf("Error decoding video frame (%d)\n", ret);
            return ret;
        }

        if (*got_frame)
		{
            if (frame->width != width || frame->height != height ||
                frame->format != pix_fmt) 
			{
                /* To handle this change, one could call av_image_alloc again and
                 * decode the following frames into another rawvideo file. */
                printf("Error: Width, height and pixel format have to be "
                        "constant in a rawvideo file, but the width, height or "
                        "pixel format of the input video changed:\n"
                        "old: width = %d, height = %d, format = %s\n"
                        "new: width = %d, height = %d, format = %s\n",
                        width, height, av_get_pix_fmt_name((AVPixelFormat)pix_fmt),
                        frame->width, frame->height,
                        av_get_pix_fmt_name((AVPixelFormat)frame->format));
                return -1;
            }

            printf("video_frame%s n:%d coded_n:%d pts:%s\n", cached ? "(cached)" : "", video_frame_count++, frame->coded_picture_number, frame->pts);

            /* copy decoded frame to destination buffer:
             * this is required since rawvideo expects non aligned data */
            av_image_copy(video_dst_data, video_dst_linesize,
                          (const uint8_t **)(frame->data), frame->linesize,
                          pix_fmt, width, height);

            /* write to rawvideo file */
            fwrite(video_dst_data[0], 1, video_dst_bufsize, files.video_dst_file);
        }
    } 
	else if (pkt.stream_index == va_ctx.audio_stream_idx) 
	{
        /* decode audio frame */
        ret = avcodec_decode_audio4(va_ctx.audio_dec_ctx, frame, got_frame, &pkt);
        if (ret < 0) 
		{
            printf("Error decoding audio frame (%s)\n", ret);
            return ret;
        }
        /* Some audio decoders decode only part of the packet, and have to be
         * called again with the remainder of the packet data.
         * Sample: fate-suite/lossless-audio/luckynight-partial.shn
         * Also, some decoders might over-read the packet. */
        decoded = FFMIN(ret, pkt.size);

        if (*got_frame) 
		{
            size_t unpadded_linesize = frame->nb_samples * av_get_bytes_per_sample((AVSampleFormat)frame->format);
            printf("audio_frame%s n:%d nb_samples:%d pts:%s\n",
                   cached ? "(cached)" : "",
                   audio_frame_count++, frame->nb_samples,
                   frame->pts);

            /* Write the raw audio data samples of the first plane. This works
             * fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
             * most audio decoders output planar audio, which uses a separate
             * plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
             * In other words, this code will write only the first audio channel
             * in these cases.
             * You should use libswresample or libavfilter to convert the frame
             * to packed data. */
            fwrite(frame->extended_data[0], 1, unpadded_linesize, files.audio_dst_file);
        }
    }

    /* If we use frame reference counting, we own the data and need
     * to de-reference it when we don't use it anymore */
    if (*got_frame && refcount)
        av_frame_unref(frame);

    return decoded;
}

static int hello(IOFileName &files, int argc, char **argv)
{
	if (argc != 4 && argc != 5) 
	{
		fprintf(stderr, "usage: %s [-refcount] input_file video_output_file audio_output_file\n"
			"API example program to show how to read frames from an input file.\n"
			"This program reads frames from a file, decodes them, and writes decoded\n"
			"video frames to a rawvideo file named video_output_file, and decoded\n"
			"audio frames to a rawaudio file named audio_output_file.\n\n"
			"If the -refcount option is specified, the program use the\n"
			"reference counting frame system which allows keeping a copy of\n"
			"the data for longer than one decode call.\n"
			"\n", argv[0]);
		return -1;
	}

	if (argc == 5 && !strcmp(argv[1], "-refcount"))
	{
		refcount = 1;
		argv++;
	}

	files.src_filename = argv[1];
	files.video_dst_filename = argv[2];
	files.audio_dst_filename = argv[3];
}

int main(int argc, char **argv)
{
	int ret = 0, got_frame;
	IOFileName files = {NULL};
	DemuxingVideoAudioContex va_ctx = {NULL};

	hello(files, argc, argv);

	if (InitDemuxContext(files, va_ctx) < 0)
	{
		goto end;
	}

	frame = av_frame_alloc();
	if (!frame) 
	{
		fprintf(stderr, "Could not allocate frame\n");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;
	
	/* read frames from the file */
	while (av_read_frame(va_ctx.fmt_ctx, &pkt) >= 0) 
	{
		AVPacket orig_pkt = pkt;
		do {
			ret = decode_packet(files, va_ctx, &got_frame, 0);
			if (ret < 0)
				break;
			pkt.data += ret;
			pkt.size -= ret;
		} while (pkt.size > 0);
		av_packet_unref(&orig_pkt);
	}

	/* flush cached frames */
	pkt.data = NULL;
	pkt.size = 0;
	do 
	{
		decode_packet(files, va_ctx, &got_frame, 1);
	}
	while (got_frame);

	printf("Demuxing succeeded.\n");

	if (va_ctx.video_stream) 
	{
		printf("Play the output video file with the command:\n"
			"ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
			av_get_pix_fmt_name(pix_fmt), width, height, files.video_dst_filename);
	}

	if (va_ctx.audio_stream) 
	{
		enum AVSampleFormat sfmt = va_ctx.audio_dec_ctx->sample_fmt;
		int n_channels = va_ctx.audio_dec_ctx->channels;
		const char *fmt;

		if (av_sample_fmt_is_planar(sfmt))
		{
			const char *packed = av_get_sample_fmt_name(sfmt);
			printf("Warning: the sample format the decoder produced is planar "
				"(%s). This example will output the first channel only.\n",
				packed ? packed : "?");
			sfmt = av_get_packed_sample_fmt(sfmt);
			n_channels = 1;
		}

		if ((ret = get_format_from_sample_fmt(&fmt, sfmt)) < 0)
			goto end;

		printf("Play the output audio file with the command:\n"
			"ffplay -f %s -ac %d -ar %d %s\n",
			fmt, n_channels, va_ctx.audio_dec_ctx->sample_rate,
			files.audio_dst_filename);
	}

end:
	CloseDemuxContext(files, va_ctx);
	av_frame_free(&frame);
	av_free(video_dst_data[0]);

	return ret < 0;
}