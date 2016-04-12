#ifndef _ENCODING_MUXING_VIDEO_H_
#define _ENCODING_MUXING_VIDEO_H_

#include "common.h"
#include "Stream.h"

void Open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg);

int Write_video_frame(AVFormatContext *oc, OutputStream *ost);

#endif