#pragma once
#ifndef _DEMUXING_CONTEXT_H_
#define _DEMUXING_CONTEXT_H_

#include "VideoDemuxingHeader.h"
#include "IOFile.h"

/*************************************************
Struct:			DemuxingVideoAudioContex
Description:	保存解复用器和解码器的上下文组件
*************************************************/
typedef struct
{
	AVFormatContext *fmt_ctx;
	AVCodecContext *video_dec_ctx, *audio_dec_ctx;
	AVStream *video_stream, *audio_stream;
	AVFrame *frame;
	AVPacket pkt;

	int video_stream_idx, audio_stream_idx;
	int width, height;

	uint8_t *video_dst_data[4];
	int video_dst_linesize[4];
	int video_dst_bufsize;
	enum AVPixelFormat pix_fmt;
} DemuxingVideoAudioContex;

/*************************************************
Function:		InitDemuxContext
Description:	根据输入参数设置编码器上下文
Calls:			open_codec_context
Called By:		main
Input:			(in)files : 输入参数
Output:			(out)va_ctx : 编码器上下文部件
Return:			错误码
*************************************************/
int InitDemuxContext(IOFileName &files, DemuxingVideoAudioContex &va_ctx);

/*************************************************
Function:		CloseDemuxContext
Description:	关闭编码器上下文
Calls:			无
Called By:		main
Input:			(in)files : 输入参数
Output:			(out)va_ctx : 编码器上下文部件
Return:			无
*************************************************/
void CloseDemuxContext(IOFileName &files, DemuxingVideoAudioContex &va_ctx);

#endif