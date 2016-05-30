#ifndef _FILTER_H_
#define _FILTER_H_

/*************************************************
	Function:		Init_video_filter
	Description:	初始化video filter相关的结构
	Calls:			无
	Called By:		main
	Input:			(in)filter_descr : 指定的filter描述
					(in)width : 输入图像宽度
					(in)height : 输入图像高度
	Output:			无
	Return:			0 : video filter初始化正确
					< 0 : video filter初始化正确
*************************************************/
int Init_video_filter(const char *filter_descr, int width, int height);

/*************************************************
	Function:		Add_frame_to_filter
	Description:	将待处理的输入frame添加进filter graph
	Calls:			无
	Called By:		main
	Input:			(in)frameIn : 待处理的输入frame
	Output:			无
	Return:			true : 添加输入frame正确
					false : 添加输入frame正确
*************************************************/
bool Add_frame_to_filter(AVFrame *frameIn);

/*************************************************
	Function:		Get_frame_from_filter
	Description:	从filter graph中获取输出frame
	Calls:			无
	Called By:		main
	Input:			无
	Output:			(in)frameOut : 保存输出像素的frame
	Return:			0 : 添加输入frame正确
					< 0 : 添加输入frame正确
*************************************************/
int Get_frame_from_filter(AVFrame **frameOut);

/*************************************************
	Function:		Close_video_filter
	Description:	关闭video filter相关结构
	Calls:			无
	Called By:		main
	Input:			无
	Output:			无
	Return:			无
*************************************************/
void Close_video_filter()

#endif