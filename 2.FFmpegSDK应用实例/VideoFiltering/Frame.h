#ifndef _FRAME_H_
#define _FRAME_H_

/*************************************************
	Function:		Init_video_frame_in_out
	Description:	初始化输入输出frame对象和像素缓存
	Calls:			无
	Called By:		main
	Input:			(in)frameWidth : 图像宽度
					(in)frameHeight : 图像高度
	Output:			(out)frameIn : 输入frame
					(out)frameOut : 输出frame
					(out)frame_buffer_in ：输入frame的像素缓存
					(out)frame_buffer_out：输出frame的像素缓存
	Return:			无
*************************************************/
void Init_video_frame_in_out(AVFrame **frameIn, AVFrame **frameOut, unsigned char **frame_buffer_in, unsigned char **frame_buffer_out, int frameWidth, int frameHeight);

#endif