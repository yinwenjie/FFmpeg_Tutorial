#ifndef _ENCODING_MUXING_HEADER_H_
#define _ENCODING_MUXING_HEADER_H_

extern "C"  
{  
	#include "libavcodec/avcodec.h"  
	#include "libavformat/avformat.h"  
	#include "libavfilter/avfiltergraph.h"  
	#include "libavfilter/buffersink.h"  
	#include "libavfilter/buffersrc.h"  
	#include "libavutil/avutil.h"  
	#include "libswscale/swscale.h"
};  

#endif