#pragma once
#ifndef _DECODER_H_
#define _DECODER_H_
#include "VideoDecodingHeader.h"

/*************************************************
Function:		Open_deocder
Description:	打开输入输出文件
Calls:			无
Called By:		main
Input:			(in/out)ctx : 注册编解码器结构并打开AVCodec、AVCodecContext、AVCodecParserContext等结构
Output:			(in/out)ctx : 用结构体成员保存打开文件的指针
Return:			true : 打开输入输出文件正确
				false : 打开输入输出文件错误
*************************************************/
bool Open_deocder(CodecCtx &ctx);

/*************************************************
Function:		Close_decoder
Description:	打开输入输出文件
Calls:			无
Called By:		main
Input:			(in/out)ctx : 解码器结构
Output:			(in/out)ctx : 解码器结构
Return:			无
*************************************************/
void Close_decoder(CodecCtx &ctx);

#endif