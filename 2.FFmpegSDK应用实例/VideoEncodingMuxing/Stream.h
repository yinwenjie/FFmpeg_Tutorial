#ifndef _STREAM_H_
#define _STREAM_H_

#include "common.h"

#define HAVE_VIDEO		1			//目标文件包含视频
#define ENCODE_VIDEO	1 << 1		//编码视频数据
#define HAVE_AUDIO		1 << 2		//目标文件包含音频
#define ENCODE_AUDIO	1 << 3		//编码音频数据

typedef struct OutputStream 
{
	AVStream *st;

	/* pts of the next frame that will be generated */
	int64_t next_pts;
	int samples_count;

	AVFrame *frame;
	AVFrame *tmp_frame;

	float t, tincr, tincr2;

	struct SwsContext *sws_ctx;
	struct SwrContext *swr_ctx;
} OutputStream;

int Add_audio_video_streams(OutputStream *video_st, OutputStream *audio_st, AVFormatContext *oc, AVOutputFormat *fmt, AVCodec *audio_codec, AVCodec *video_codec, IOParam &io);

void Close_stream(AVFormatContext *oc, OutputStream *ost);

#endif