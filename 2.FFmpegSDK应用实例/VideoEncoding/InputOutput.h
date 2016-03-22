#pragma once
#ifndef _INPUT_OUTPUT_H_
#define _INPUT_OUTPUT_H_

#include <stdio.h>
#include <stdint.h>
#include "VideoEncodingHeader.h"

/*************************************************
	Struct:			IOParam
	Description:	接收命令行参数
*************************************************/
typedef struct
{
	FILE *pFin;				//输入的YUV文件指针
	FILE *pFout;			//输出的码流文件指针

	char *pNameIn;			//输入YUV文件名
	char *pNameOut;			//输出码流文件名

	uint16_t nImageWidth;	//图像宽度
	uint16_t nImageHeight;	//图像高度

	uint16_t nFrameRate;	//编码帧率
	uint64_t nBitRate;		//编码码率
	uint16_t nGOPSize;		//一个GOP大小
	uint16_t nMaxBFrames;	//最大B帧数量
	uint16_t nTotalFrames;	//编码总帧数
} IOParam;

/*************************************************
	Function:		Parse_input_param
	Description:	解析命令行传入的参数
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)io_param : 解析命令行的结果
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
bool Parse_input_param(int argc, char **argv, IOParam &io_param);

/*************************************************
	Function:		Open_file
	Description:	打开输入输出文件
	Calls:			无
	Called By:		main
	Input:			(in/out)io_param : 传入文件的路径					
	Output:			(in/out)io_param : 用结构体成员保存打开文件的指针
	Return:			true : 打开输入输出文件正确
					false : 打开输入输出文件错误
*************************************************/
bool Open_file(IOParam &io_param);

/*************************************************
	Function:		Close_file
	Description:	关闭输入输出文件
	Calls:			无
	Called By:		main
	Input:			(in)io_param : 保存输入输出文件的指针					
	Output:			无
	Return:			无
*************************************************/
void Close_file(IOParam &io_param);

/*************************************************
	Function:		Read_yuv_data
	Description:	从打开的输入文件中读取yuv数据，并保存到CodecCtx::AVFrame
	Calls:			无
	Called By:		main
	Input:			(in)io_param : 保存输入输出文件的指针	
					(in)color_plane ： 颜色分量标识：0-Y, 1-U, 2-V
	Output:			(out)ctx : 保存AVFrame的结构实例
	Return:			无
*************************************************/
int Read_yuv_data(CodecCtx &ctx, IOParam &io_param, int color_plane);

#endif