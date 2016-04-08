#ifndef _CODER_MUXER_H_
#define _CODER_MUXER_H_

typedef struct _tagCoderMuxer
{
	AVCodec *audio_codec, *video_codec;
	AVDictionary *opt;
} CoderMuxer;

int Open_coder_muxer(CoderMuxer &coderMuxer);

#endif