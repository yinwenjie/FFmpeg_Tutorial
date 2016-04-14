#ifndef _VIDEO_WATERMARK_HEADER_H_
#define _VIDEO_WATERMARK_HEADER_H_

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

#endif