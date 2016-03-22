#pragma once
#ifndef _ENCODER_H_
#define _ENCODER_H_
#include "VideoEncodingHeader.h"
#include "InputOutput.h"

/*************************************************
	Function:		Open_encoder
	Description:	根据输入参数配置AVCodecContext，并打开AVCodec等结构对象
	Calls:			avcodec_register_all
					avcodec_find_encoder
					avcodec_alloc_context3
					setContext
					avcodec_open2
					av_frame_alloc
					av_image_alloc
	Called By:		main
	Input:			(in)io_param : 保存输入输出文件的指针					
	Output:			(out)ctx : 包含编码器部件的结构对象
	Return:			true : 打开各个编码器部件成功
					false : 打开编码器部件失败
*************************************************/
bool Open_encoder(CodecCtx &ctx, IOParam io_param);

/*************************************************
	Function:		Close_encoder
	Description:	关闭编码器上下文部件
	Calls:			avcodec_close
					av_free
					av_freep
					av_frame_free
	Called By:		main
	Input:			(in)ctx : 编码器上下文部件					
	Output:			无
	Return:			无
*************************************************/
void Close_encoder(CodecCtx &ctx);

#endif