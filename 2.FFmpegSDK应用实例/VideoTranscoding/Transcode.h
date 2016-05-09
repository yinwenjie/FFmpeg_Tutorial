#ifndef _TRANSCODE_H_
#define _TRANSCODE_H_

/*************************************************
	Function:		Init_transcoder
	Description:	初始化转码器环境
	Calls:			无
	Called By:		main
	Input:			(int/out)trans_ctx : 转码器上下文对象
	Return:			无
*************************************************/
typedef struct _TranscodingContext TranscodingContext;
void Init_transcoder(TranscodingContext &trans_ctx);

#endif