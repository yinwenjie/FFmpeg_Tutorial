#include "VideoEncoding.h"
#include "VideoMuxing.h"

int Application_encoding_start(IOFileName &files, EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx)
{
	/* allocate the output media context */
	avformat_alloc_output_context2(&(enc_ctx.oc), NULL, NULL, files.dst_filename);
	if (!enc_ctx.oc)
	{
		return -1;
	}
	enc_ctx.fmt = enc_ctx.oc->oformat;
/*	enc_ctx.fmt->video_codec = AV_CODEC_ID_H264;*/
	
	Add_video_stream(enc_ctx, demux_ctx);

	return 0;
}

int Prepare_to_write_output_file(IOFileName &files, EncodingContext &enc_ctx)
{
	AVFormatContext *oc = enc_ctx.oc;

	av_dump_format(oc, 0, files.dst_filename, 1);
	if (avio_open(&oc->pb, files.dst_filename, AVIO_FLAG_WRITE) < 0)
	{
		return -1;
	}

	/* Write the stream header, if any. */
	if (avformat_write_header(oc, NULL) < 0)
	{
		fprintf(stderr, "Error occurred when opening output file.\n");
		return -1;
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

int Encode_frame_to_packet(EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx)
{
	int got_packet = 0, ret = 0;
	AVCodecContext *c = enc_ctx.video_st.st->codec;
	AVFormatContext *oc = enc_ctx.oc;
	OutputStream *ost = &enc_ctx.video_st;

	AVFrame *frame = av_frame_alloc();
	frame->width = demux_ctx.width;
	frame->height = demux_ctx.height;
	frame->format = demux_ctx.pix_fmt;
	av_image_alloc(frame->data, frame->linesize, frame->width, frame->height, demux_ctx.pix_fmt, 32);
//	av_image_copy(frame->data, frame->linesize, demux_ctx.frame->data, demux_ctx.frame->linesize, demux_ctx.pix_fmt, frame->width, frame->height);

	c->codec_id = AV_CODEC_ID_H264;
	av_init_packet(&enc_ctx.outPacket);

	/* encode the image */
	if (avcodec_encode_video2(c, &enc_ctx.outPacket, frame, &got_packet) < 0) 
	{
		fprintf(stderr, "Error encoding video frame\n");
		return -1;
	}
	if (got_packet) 
	{
		ret = write_frame(oc, &c->time_base, ost->st, &enc_ctx.outPacket);
	}
	else 
	{
		ret = 0;
	}

	if (ret < 0)
	{
		fprintf(stderr, "Error while writing video frame: %d\n", ret);
		return -1;
	}

	return (frame || got_packet) ? 0 : 1;
}
