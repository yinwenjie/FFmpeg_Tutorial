#ifndef _ENCODING_MUXING_AUDIO_H_
#define _ENCODING_MUXING_AUDIO_H_

#include "common.h"
#include "Stream.h"

void Open_audio(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg);

int Write_audio_frame(AVFormatContext *oc, OutputStream *ost);

/*************************************************
	Struct:		AudioSignalGenerator
	Description:	生成音频数据的结构
*************************************************/
typedef struct _AudioSignalGenerator
{
	int samples_count;

	float t, tincr, tincr2;
	struct SwsContext *sws_ctx;
	struct SwrContext *swr_ctx;

}AudioSignalGenerator;

/*************************************************
	Function:		Add_Audio_stream
	Description:	向打开文件中添加一路视频流
	Calls:			无
	Called By:		main
	Input:			(in/out)audioStream : 待添加的视频流
					(in/out)oc : 视频文件句柄
					(in/out)codec：编码器实例
					(in)codec_id：编码器ID
	Output:			无
	Return:			函数执行错误码
*************************************************/
int Add_Audio_stream(AVStream **audioStream, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id);


/*************************************************
	Function:		Set_audio_stream
	Description:	设置视频码流的参数，全部采用默认设置
	Calls:			无
	Called By:		main
	Input:			无
	Output:			无
	Return:			无
*************************************************/
void Set_audio_stream(AVStream **audioStream, const AVCodec *codec);

/*************************************************
	Function:		Open_audio_stream
	Description:	打开音频流的codec、frame和文件
	Calls:			无
	Called By:		main
	Input:			(int)codec : 分配完成的编码器
	Output:			(in/out)audioStream : 待处理的视频流
					(in/out)audioFrame : 保存音频数据的frame结构
	Return:			函数执行错误码
*************************************************/
int Open_audio_stream(AVStream **videoStream, AVFrame **videoFrame, AVCodec *codec, IOParam io);

#endif