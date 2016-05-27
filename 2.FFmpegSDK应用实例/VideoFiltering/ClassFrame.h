#ifndef _CLASS_FRAME_H_
#define _CLASS_FRAME_H_

AVFrame *avframe_allocation(int width, int height, enum AVPixelFormat pix_fmt);

void avframe_deallocation(AVFrame *frame);

int Read_yuv_to_AVFrame(AVFrame *frame, FILE **inputFile, int colorPlane);

void Write_out_yuv(AVFrame *frame, FILE **outputFile);

#endif