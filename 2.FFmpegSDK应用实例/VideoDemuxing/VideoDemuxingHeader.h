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

#endif