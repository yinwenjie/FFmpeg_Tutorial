#ifndef _ENCODING_MUXING_VIDEO_H_
#define _ENCODING_MUXING_VIDEO_H_

#include "common.h"
#include "Stream.h"

typedef struct _IOParam IOParam;

void Open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg, IOParam &io);

int Write_video_frame(AVFormatContext *oc, OutputStream *ost);

#endif