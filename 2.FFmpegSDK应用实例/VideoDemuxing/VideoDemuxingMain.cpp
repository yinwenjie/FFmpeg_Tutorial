#include <stdio.h>
#include "VideoDemuxingHeader.h"

int main(int argc, char **argv)
{
	OutputStream video_st = { 0 }, audio_st = { 0 };
	const char *filename;
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	AVCodec *audio_codec, *video_codec;
	int ret;
	int have_video = 0, have_audio = 0;
	int encode_video = 0, encode_audio = 0;
	AVDictionary *opt = NULL;

	/* Initialize libavcodec, and register all codecs and formats. */
//	av_register_all();

	return 1;
}