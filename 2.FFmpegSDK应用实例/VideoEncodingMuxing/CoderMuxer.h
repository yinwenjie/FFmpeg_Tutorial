#ifndef _CODER_MUXER_H_
#define _CODER_MUXER_H_
#include "common.h"

int Open_coder_muxer(AVOutputFormat **fmt, AVFormatContext **oc, const char *filename);

#endif