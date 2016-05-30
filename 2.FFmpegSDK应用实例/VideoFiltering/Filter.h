#ifndef _FILTER_H_
#define _FILTER_H_

int Init_video_filter(const char *filter_descr, int width, int height);

bool Add_frame_to_filter(AVFrame *frameIn);

int Get_frame_from_filter(AVFrame **frameOut);

void Close_video_filter();

#endif