#pragma once
#ifndef _VIDEO_DEMUXING_HEADER_H
#define _VIDEO_DEMUXING_HEADER_H

extern "C"
{
	#include "libavutil/imgutils.h"
	#include "libavutil/samplefmt.h"
	#include "libavformat/avformat.h"
}

typedef struct 
{
	const char *src_filename;
	const char *video_dst_filename;
	const char *audio_dst_filename;
} IOFileName;

typedef struct
{
	AVFormatContext *fmt_ctx;
	AVCodecContext *video_dec_ctx, *audio_dec_ctx;
	AVStream *video_stream, *audio_stream;
	int video_stream_idx, audio_stream_idx;
} DemuxingVideoAudioContex;

#endif