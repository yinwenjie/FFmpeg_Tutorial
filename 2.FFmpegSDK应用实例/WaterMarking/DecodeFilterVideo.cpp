#include "common.h"
#include "DecodeFilterVideo.h"
#include "VideoFilter.h"


int Decode_this_packet_to_frame(IOFileName &files, DemuxingVideoAudioContex &demux_ctx, int *got_frame, int cached)
{
	int ret = 0;
	int decoded = demux_ctx.pkt.size;
	static int video_frame_count = 0;
	static int audio_frame_count = 0;

	*got_frame = 0;

	if (demux_ctx.pkt.stream_index == demux_ctx.video_stream_idx)
	{
		/* decode video frame */
		ret = avcodec_decode_video2(demux_ctx.video_dec_ctx, demux_ctx.frame, got_frame, &demux_ctx.pkt);
		if (ret < 0)
		{
			printf("Error decoding video frame (%d)\n", ret);
			return ret;
		}

		if (*got_frame)
		{
			printf("video_frame%s n:%d coded_n:%d pts:%s\n", cached ? "(cached)" : "", video_frame_count++, demux_ctx.frame->coded_picture_number, demux_ctx.frame->pts);
			
			if (Push_into_filter_graph(demux_ctx.frame) < 0)
			{
				return -1;
			}

			Get_filtered_frame(demux_ctx.frame);

			/* copy decoded frame to destination buffer:
			* this is required since rawvideo expects non aligned data */
			av_image_copy(demux_ctx.video_dst_data, demux_ctx.video_dst_linesize,
				(const uint8_t **)(demux_ctx.frame->data), demux_ctx.frame->linesize,
				demux_ctx.pix_fmt, demux_ctx.width, demux_ctx.height);

			/* write to rawvideo file */
			fwrite(demux_ctx.video_dst_data[0], 1, demux_ctx.video_dst_bufsize, files.temp_file);
		}
	}
	return decoded;
}
