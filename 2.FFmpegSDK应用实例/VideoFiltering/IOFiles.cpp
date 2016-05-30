#include "common.h"

//从输入yuv文件中读取数据到buffer和frame结构
bool Read_yuv_data_to_buf(unsigned char *frame_buffer_in, const IOFiles &files, AVFrame **frameIn)
{
	AVFrame *pFrameIn = *frameIn;
	int width = files.frameWidth, height = files.frameHeight;
	int frameSize = width * height * 3 / 2;

	if (fread_s(frame_buffer_in, frameSize, 1, frameSize, files.iFile) != frameSize)
	{
		return false;
	}

	pFrameIn->data[0] = frame_buffer_in;
	pFrameIn->data[1] = pFrameIn->data[0] + width * height;
	pFrameIn->data[2] = pFrameIn->data[1] + width * height / 4;

	return true;
}

//从输出frame中写出像素数据到输出文件
void Write_yuv_to_outfile(const AVFrame *frame_out, IOFiles &files)
{
	if(frame_out->format==AV_PIX_FMT_YUV420P)
	{  
		for(int i=0;i<frame_out->height;i++)
		{  
			fwrite(frame_out->data[0]+frame_out->linesize[0]*i,1,frame_out->width,files.oFile);  
		}  
		for(int i=0;i<frame_out->height/2;i++)
		{  
			fwrite(frame_out->data[1]+frame_out->linesize[1]*i,1,frame_out->width/2,files.oFile);  
		}  
		for(int i=0;i<frame_out->height/2;i++)
		{  
			fwrite(frame_out->data[2]+frame_out->linesize[2]*i,1,frame_out->width/2,files.oFile);  
		}  
	}  
}
