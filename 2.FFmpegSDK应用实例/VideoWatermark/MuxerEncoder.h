#ifndef _MUXER_ENCODER_H_
#define _MUXER_ENCODER_H_

int InitMuxerEncoder(const char *dst_filename, const AVCodecContext *pCodecCtx);

int EncodeOneFrame();

void CloseMuxerEncoder();

int FetchAVFrameToEncoder(AVFrame *src);

#endif