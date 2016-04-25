#ifndef _VIDEO_TRANSCODING_HEADER_
#define _VIDEO_TRANSCODING_HEADER_

#define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)

extern "C"  
{  
#include "libavcodec/avcodec.h"  
#include "libavformat/avformat.h"  
#include "libavutil/avutil.h"  
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
};  

#endif