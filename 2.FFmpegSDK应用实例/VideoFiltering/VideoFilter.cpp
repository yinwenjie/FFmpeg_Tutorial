#include "common.h"

AVFilterGraph *filter_graph;
AVFilterContext *buffersrc_ctx;
AVFilterContext *buffersink_ctx;

int Init_filters(const char *filters_descr,  const AVCodecContext *decCtx)
{
	int ret = 0;
	char args[512];

	avfilter_register_all();
	AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut *outputs = avfilter_inout_alloc();
	AVFilterInOut *inputs  = avfilter_inout_alloc();
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_GRAY8, AV_PIX_FMT_NONE };
	AVBufferSinkParams *buffersink_params;

	filter_graph = avfilter_graph_alloc();
	if (!outputs || !inputs || !filter_graph) 
	{
		printf("Error: filter structures allocation failed.\n");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d", decCtx->width, decCtx->height, decCtx->pix_fmt, decCtx->time_base.num, decCtx->time_base.den, decCtx->sample_aspect_ratio.num, decCtx->sample_aspect_ratio.den);
	ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in", args, NULL, filter_graph);
	if (ret < 0)
	{
		printf("Error: cannot create buffer source\n");
		goto end;
	}

	buffersink_params = av_buffersink_params_alloc();
	buffersink_params->pixel_fmts = pix_fmts;
	ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",NULL, buffersink_params, filter_graph);
	av_free(buffersink_params);
	if (ret < 0)
	{
		printf("Error: cannot create buffer sink\n");
		goto end;
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

	if ((ret = avfilter_graph_parse_ptr(filter_graph, filters_descr, &inputs, &outputs, NULL)) < 0)
	{
		printf("Error: avfilter_graph_parse_ptr failed.\n");
		goto end;
	}
	if ((ret = avfilter_graph_config(filter_graph, NULL)) < 0)
	{
		printf("Error: avfilter_graph_config failed.\n");
		goto end;
	}

end:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	return ret;
}

int Feed_filter_graph(AVFrame *frame)
{
	frame->pts = av_frame_get_best_effort_timestamp(frame);

	if (av_buffersrc_add_frame(buffersrc_ctx, frame) < 0) 
	{
		printf( "Error while feeding the filtergraph\n");
		return false;
	}

	return true;
}

int Get_buffersink_frame(AVFrame *frameOut)
{
	int ret = av_buffersink_get_frame(buffersink_ctx, frameOut);
	if (ret < 0)
	{
		return false;
	}

	return true;
}
