#pragma once
#ifndef _DEMUXING_CONTEXT_H_
#define _DEMUXING_CONTEXT_H_

#include "VideoDemuxingHeader.h"
#include "IOFile.h"

typedef struct
{
	AVFormatContext *fmt_ctx;
	AVCodecContext *video_dec_ctx, *audio_dec_ctx;
	AVStream *video_stream, *audio_stream;
	int video_stream_idx, audio_stream_idx;

	uint8_t *video_dst_data[4];
	int video_dst_linesize[4];
	int video_dst_bufsize;
} DemuxingVideoAudioContex;

int InitDemuxContext(IOFileName &files, DemuxingVideoAudioContex &va_ctx);

void CloseDemuxContext(IOFileName &files, DemuxingVideoAudioContex &va_ctx);

#endif