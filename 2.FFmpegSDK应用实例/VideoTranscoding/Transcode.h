#ifndef _TRANSCODE_H_
#define _TRANSCODE_H_

/*************************************************
	Function:		Init_transcoder
	Description:	初始化转码器环境
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)files : 输入输出文件对象
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
typedef struct _TranscodingContext TranscodingContext;
void Init_transcoder(TranscodingContext &trans_ctx);

#endif