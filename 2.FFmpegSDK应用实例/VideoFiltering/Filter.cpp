#include "common.h"

AVFilterContext *buffersink_ctx;  
AVFilterContext *buffersrc_ctx;  
AVFilterGraph *filter_graph;  

//初始化video filter相关的结构
int Init_video_filter(const char *filter_descr, int width, int height)
{
	char args[512];  
	AVFilter *buffersrc  = avfilter_get_by_name("buffer");  
	AVFilter *buffersink = avfilter_get_by_name("buffersink");  
	AVFilterInOut *outputs = avfilter_inout_alloc();  
	AVFilterInOut *inputs  = avfilter_inout_alloc();  
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };  
	AVBufferSinkParams *buffersink_params;  

	filter_graph = avfilter_graph_alloc();  

	/* buffer video source: the decoded frames from the decoder will be inserted here. */  
	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", width,height,AV_PIX_FMT_YUV420P, 1, 25,1,1);
	int ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);  
	if (ret < 0) 
	{
		printf("Error: cannot create buffer source.\n");  
		return ret;  
	}  

	/* buffer video sink: to terminate the filter chain. */  
	buffersink_params = av_buffersink_params_alloc();  
	buffersink_params->pixel_fmts = pix_fmts;  
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out", NULL, buffersink_params, filter_graph);  
	av_free(buffersink_params);  
	if (ret < 0) 
	{
		printf("Error: cannot create buffer sink\n");  
		return ret;
	}  

	/* Endpoints for the filter graph. */  
	outputs->name       = av_strdup("in");  
	outputs->filter_ctx = buffersrc_ctx;  
	outputs->pad_idx    = 0;  
	outputs->next       = NULL;  

	inputs->name       = av_strdup("out");  
	inputs->filter_ctx = buffersink_ctx;  
	inputs->pad_idx    = 0;  
	inputs->next       = NULL;  

	if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_descr,	&inputs, &outputs, NULL)) < 0)
	{
		printf("Error: avfilter_graph_parse_ptr failed.\n");
		return ret;  
	}

	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)  
	{
		printf("Error: avfilter_graph_config");
		return ret;  
	}

	return 0;
}

//将待处理的输入frame添加进filter graph
bool Add_frame_to_filter(AVFrame *frameIn)
{
	if (av_buffersrc_add_frame(buffersrc_ctx, frameIn) < 0) 
	{  
		return false;  
	}  

	return true;
}

//从filter graph中获取输出frame
int Get_frame_from_filter(AVFrame **frameOut)
{
	if (av_buffersink_get_frame(buffersink_ctx, *frameOut) < 0)
	{
		return false;
	}

	return true;
}

//关闭video filter相关结构
void Close_video_filter()
{
	avfilter_graph_free(&filter_graph);
}