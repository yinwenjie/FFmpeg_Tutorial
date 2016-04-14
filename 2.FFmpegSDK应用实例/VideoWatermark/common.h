#ifndef _COMMON_H_
#define _COMMON_H_

#define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)

#include "VideoWatermarkHeader.h"

#endif