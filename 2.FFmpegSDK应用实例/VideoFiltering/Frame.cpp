#include "common.h"

void Init_video_frame_in_out(AVFrame **frameIn, AVFrame **frameOut, unsigned char **frame_buffer_in, unsigned char **frame_buffer_out, int frameWidth, int frameHeight)
{
	*frameIn = av_frame_alloc();  
	*frame_buffer_in = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frameWidth,frameHeight,1));  
	av_image_fill_arrays((*frameIn)->data, (*frameIn)->linesize,*frame_buffer_in, AV_PIX_FMT_YUV420P,frameWidth,frameHeight,1);  

	*frameOut = av_frame_alloc();  
	*frame_buffer_out = (unsigned char *)av_malloc(av_image_get_buffer_size(AV_PIX_FMT_YUV420P, frameWidth,frameHeight,1));  
	av_image_fill_arrays((*frameOut)->data, (*frameOut)->linesize,*frame_buffer_out, AV_PIX_FMT_YUV420P,frameWidth,frameHeight,1);  

	(*frameIn)->width = frameWidth;  
	(*frameIn)->height = frameHeight;  
	(*frameIn)->format = AV_PIX_FMT_YUV420P;  
}
