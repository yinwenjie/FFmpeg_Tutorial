#ifndef _VIDEO_FILTER_H_
#define _VIDEO_FILTER_H_

int Init_video_filters(const char *filters_descr, AVCodecContext *pCodecCtx);

void Close_video_filters();

int Push_into_filter_graph(AVFrame *pFrame);

int Get_filtered_frame(AVFrame *pFrame);

#endif