#ifndef _ENCODING_MUXING_HEADER_H_
#define _ENCODING_MUXING_HEADER_H_

#define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)

extern "C"  
{  
	#include "libavcodec/avcodec.h"  
	#include "libavformat/avformat.h"  
	#include "libavfilter/avfiltergraph.h"  
	#include "libavfilter/buffersink.h"  
	#include "libavfilter/buffersrc.h"  
	#include "libavutil/avutil.h"  
	#include "libswscale/swscale.h"
	#include "libswresample/swresample.h"

	#include "libavutil/avassert.h"
	#include "libavutil/channel_layout.h"
	#include "libavutil/opt.h"
	#include "libavutil/mathematics.h"
	#include "libavutil/timestamp.h"
};  

/*************************************************
	Struct:			IOParam
	Description:	接收命令行参数
*************************************************/
typedef struct _IOParam
{
	const char *input_file_name;	//输入的像素文件名
	const char *output_file_name;	//输出的封装视频文件名
	int frame_width;				//视频帧宽度
	int frame_height;				//视频帧高度
} IOParam;

#endif