#ifndef _VIDEO_FILTER_H_
#define _VIDEO_FILTER_H_

int Init_filters(const char *filters_descr,  const AVCodecContext *decCtx);

int Feed_filter_graph(AVFrame *frame);

int Get_buffersink_frame(AVFrame *frameOut);

#endif