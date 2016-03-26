#include <stdio.h>
#include "AVDecoder.h"

extern int refcount;
extern int width, height;
extern enum AVPixelFormat pix_fmt;

int Get_format_from_sample_fmt(const char **fmt, enum AVSampleFormat sample_fmt)
{
	int i;
	struct sample_fmt_entry {
		enum AVSampleFormat sample_fmt; const char *fmt_be, *fmt_le;
	} sample_fmt_entries[] = {
		{ AV_SAMPLE_FMT_U8, "u8", "u8" },
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

int Decode_packet(IOFileName &files, DemuxingVideoAudioContex &va_ctx, int *got_frame, int cached)
{
	int ret = 0;
	int decoded = va_ctx.pkt.size;
	static int video_frame_count = 0;
	static int audio_frame_count = 0;

	*got_frame = 0;

	if (va_ctx.pkt.stream_index == va_ctx.video_stream_idx)
	{
		/* decode video frame */
		ret = avcodec_decode_video2(va_ctx.video_dec_ctx, va_ctx.frame, got_frame, &va_ctx.pkt);
		if (ret < 0)
		{
			printf("Error decoding video frame (%d)\n", ret);
			return ret;
		}

		if (*got_frame)
		{
			if (va_ctx.frame->width != width || va_ctx.frame->height != height ||
				va_ctx.frame->format != pix_fmt)
			{
				/* To handle this change, one could call av_image_alloc again and
				* decode the following frames into another rawvideo file. */
				printf("Error: Width, height and pixel format have to be "
					"constant in a rawvideo file, but the width, height or "
					"pixel format of the input video changed:\n"
					"old: width = %d, height = %d, format = %s\n"
					"new: width = %d, height = %d, format = %s\n",
					width, height, av_get_pix_fmt_name((AVPixelFormat)pix_fmt),
					va_ctx.frame->width, va_ctx.frame->height,
					av_get_pix_fmt_name((AVPixelFormat)va_ctx.frame->format));
				return -1;
			}

			printf("video_frame%s n:%d coded_n:%d pts:%s\n", cached ? "(cached)" : "", video_frame_count++, va_ctx.frame->coded_picture_number, va_ctx.frame->pts);

			/* copy decoded frame to destination buffer:
			* this is required since rawvideo expects non aligned data */
			av_image_copy(va_ctx.video_dst_data, va_ctx.video_dst_linesize,
				(const uint8_t **)(va_ctx.frame->data), va_ctx.frame->linesize,
				pix_fmt, width, height);

			/* write to rawvideo file */
			fwrite(va_ctx.video_dst_data[0], 1, va_ctx.video_dst_bufsize, files.video_dst_file);
		}
	}
	else if (va_ctx.pkt.stream_index == va_ctx.audio_stream_idx)
	{
		/* decode audio frame */
		ret = avcodec_decode_audio4(va_ctx.audio_dec_ctx, va_ctx.frame, got_frame, &va_ctx.pkt);
		if (ret < 0)
		{
			printf("Error decoding audio frame (%s)\n", ret);
			return ret;
		}
		/* Some audio decoders decode only part of the packet, and have to be
		* called again with the remainder of the packet data.
		* Sample: fate-suite/lossless-audio/luckynight-partial.shn
		* Also, some decoders might over-read the packet. */
		decoded = FFMIN(ret, va_ctx.pkt.size);

		if (*got_frame)
		{
			size_t unpadded_linesize = va_ctx.frame->nb_samples * av_get_bytes_per_sample((AVSampleFormat)va_ctx.frame->format);
			printf("audio_frame%s n:%d nb_samples:%d pts:%s\n",
				cached ? "(cached)" : "",
				audio_frame_count++, va_ctx.frame->nb_samples,
				va_ctx.frame->pts);

			/* Write the raw audio data samples of the first plane. This works
			* fine for packed formats (e.g. AV_SAMPLE_FMT_S16). However,
			* most audio decoders output planar audio, which uses a separate
			* plane of audio samples for each channel (e.g. AV_SAMPLE_FMT_S16P).
			* In other words, this code will write only the first audio channel
			* in these cases.
			* You should use libswresample or libavfilter to convert the frame
			* to packed data. */
			fwrite(va_ctx.frame->extended_data[0], 1, unpadded_linesize, files.audio_dst_file);
		}
	}

	/* If we use frame reference counting, we own the data and need
	* to de-reference it when we don't use it anymore */
	if (*got_frame && refcount)
		av_frame_unref(va_ctx.frame);

	return decoded;
}
