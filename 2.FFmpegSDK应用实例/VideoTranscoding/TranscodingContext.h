#ifndef _TRANSCODING_CONTEXT_H_
#define _TRANSCODING_CONTEXT_H_

typedef struct _TranscodingContext
{
	AVPacket packet;
	AVFrame *frame;
	unsigned int stream_index;;
} TranscodingContext;

#endif