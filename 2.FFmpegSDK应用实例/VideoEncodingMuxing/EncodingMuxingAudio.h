#ifndef _ENCODING_MUXING_AUDIO_H_
#define _ENCODING_MUXING_AUDIO_H_

#include "common.h"
#include "Stream.h"

void Open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg);

int Write_audio_frame(AVFormatContext *oc, OutputStream *ost);

#endif