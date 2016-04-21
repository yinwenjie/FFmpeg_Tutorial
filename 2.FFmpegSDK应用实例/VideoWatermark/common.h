#ifndef _COMMON_H_
#define _COMMON_H_

#include "Log.h"

#define LOG_OUT 1

#define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)

extern "C"
{
	#include "libavutil/opt.h"
	#include "libavutil/channel_layout.h"
	#include "libavutil/common.h"
	#include "libavutil/imgutils.h"
	#include "libavutil/mathematics.h"
	#include "libavutil/samplefmt.h"
	#include "libavutil/imgutils.h"
	
	#include "libavcodec/avcodec.h"
	#include "libavformat/avformat.h"
	
	#include "libavfilter/avfilter.h"  
	#include "libavfilter/avfiltergraph.h"  
	#include "libavfilter/buffersink.h"  
	#include "libavfilter/buffersrc.h"  
	#include "libavfilter/avfiltergraph.h"
	
	#include "libswscale/swscale.h"
	#include "libswresample/swresample.h"
}

#define SCALE_FLAGS SWS_BICUBIC

#endif