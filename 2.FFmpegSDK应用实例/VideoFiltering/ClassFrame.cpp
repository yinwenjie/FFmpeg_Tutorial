#include "common.h"
#include "ClassFrame.h"

AVFrame * avframe_allocation(int width, int height, AVPixelFormat pix_fmt)
{
	AVFrame *frame = av_frame_alloc();
	if (!frame)
	{
		printf("Error: AVFrame allocation failed.\n");
		return NULL;
	}

	frame->width = 720;
	frame->height = 480;
	frame->format = pix_fmt;

	int ret = av_image_alloc(frame->data,frame->linesize, frame->width, frame->height, pix_fmt, 32);
	if (ret < 0)
	{
		printf("Error: AVFrame buffer allocation failed.\n");
		return NULL;
	}
	
	return frame;
}

void avframe_deallocation(AVFrame *frame)
{
	av_freep(&(frame->data[0]));
	av_frame_free(&frame);
}

int Read_yuv_to_AVFrame(AVFrame *frame, FILE **inputFile, int colorPlane)
{
	int frame_height	= colorPlane == 0? frame->height : frame->height / 2;
	int frame_width		= colorPlane == 0? frame->width : frame->width / 2;
	int frame_size		= frame_width * frame_height;
	int frame_stride	= frame->linesize[colorPlane];

	if (frame_width == frame_stride)
	{
		//宽度和跨度相等，像素信息连续存放
		fread_s(frame->data[colorPlane], frame_size, 1, frame_size, *inputFile);
	} 
	else
	{
		//宽度小于跨度，像素信息保存空间之间存在间隔
		for (int row_idx = 0; row_idx < frame_height; row_idx++)
		{
			fread_s(frame->data[colorPlane] + row_idx * frame_stride, frame_width, 1, frame_width, *inputFile);
		}
	}

	return frame_size;
}

void Write_out_yuv(AVFrame *frame, FILE **outputFile)
{
	uint8_t **pBuf	= frame->data;
	int*	pStride = frame->linesize;

	for (int color_idx = 0; color_idx < 3; color_idx++)
	{
		int		nWidth	= color_idx == 0 ? frame->width  : frame->width / 2;
		int		nHeight = color_idx == 0 ? frame->height : frame->height / 2;
		for(int idx=0;idx < nHeight; idx++)
		{
			fwrite(pBuf[color_idx],1, nWidth, *outputFile);
			pBuf[color_idx] += pStride[color_idx];
		}
		fflush(*outputFile);
	}
}
