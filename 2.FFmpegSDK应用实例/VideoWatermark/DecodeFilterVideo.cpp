#include "common.h"
#include "DecodeFilterVideo.h"
#include "VideoFilter.h"


int Decode_this_packet_to_frame(IOFileName &files, DemuxingVideoAudioContex &va_ctx, int *got_frame, int cached)
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
			printf("video_frame%s n:%d coded_n:%d pts:%s\n", cached ? "(cached)" : "", video_frame_count++, va_ctx.frame->coded_picture_number, va_ctx.frame->pts);
			
			if (Push_into_filter_graph(va_ctx.frame) < 0)
			{
				return -1;
			}

			Get_filtered_frame(va_ctx.frame);

			/* copy decoded frame to destination buffer:
			* this is required since rawvideo expects non aligned data */
			av_image_copy(va_ctx.video_dst_data, va_ctx.video_dst_linesize,
				(const uint8_t **)(va_ctx.frame->data), va_ctx.frame->linesize,
				va_ctx.pix_fmt, va_ctx.width, va_ctx.height);

			/* write to rawvideo file */
			fwrite(va_ctx.video_dst_data[0], 1, va_ctx.video_dst_bufsize, files.video_dst_file);
		}
	}

	if (*got_frame && files.refcount)
	{
		av_frame_unref(va_ctx.frame);
	}

	return decoded;
}
