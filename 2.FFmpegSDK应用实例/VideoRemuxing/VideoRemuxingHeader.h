#ifndef _VIDEO_REMUXING_HEADER_H_
#define _VIDEO_REMUXING_HEADER_H_

#define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)

extern "C"
{
#include "libavutil/timestamp.h"
#include "libavformat/avformat.h"
};

#endif