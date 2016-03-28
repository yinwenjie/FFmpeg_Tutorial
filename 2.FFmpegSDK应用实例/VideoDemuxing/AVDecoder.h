#ifndef _AV_DECODER_H_
#define _AV_DECODER_H_

#include "IOFile.h"
#include "DemuxingContext.h"

/*************************************************
Function:		Get_format_from_sample_fmt
Description:	获取音频采样格式
Calls:			FFMpeg API
Called By:		main
Input:			(in)sample_fmt : 采样格式结构体
Output:			(out)fmt : 标识采样格式的字符串
Return:			错误码
*************************************************/
int Get_format_from_sample_fmt(const char **fmt, enum AVSampleFormat sample_fmt);

/*************************************************
Function:		Decode_packet
Description:	解码一个音频或视频数据包
Calls:			FFMpeg API
Called By:		main
Input:			(in)files : 输入参数
				(in)va_ctx : 编码器上下文部件
				(in)cached : 是否处理解码器中的缓存数据
Output:			(out)got_frame : 完整解码一帧标识
Return:			错误码
*************************************************/
int Decode_packet(IOFileName &files, DemuxingVideoAudioContex &va_ctx, int *got_frame, int cached);

#endif