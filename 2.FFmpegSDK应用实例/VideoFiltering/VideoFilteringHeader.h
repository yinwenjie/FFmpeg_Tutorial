#ifndef _VIDEO_FILTERING_HEADER_H_
#define _VIDEO_FILTERING_HEADER_H_

//#define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)

extern "C"
{
#include "libavfilter/avfiltergraph.h"  
#include "libavfilter/buffersink.h"  
#include "libavfilter/buffersrc.h"  
#include "libavutil/avutil.h"  
#include "libavutil/imgutils.h" 
};

#endif