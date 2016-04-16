#ifndef _DEMUXER_DECODER_H_
#define _DEMUXER_DECODER_H_

int InitDemuxerDecoder(const char *src_filename, const int yuv_out);

int InitFramePacket();

int ReadFramesFromFile();

int DecodeOutFrames(const int yuv_out, void (*AVFrame_callback)(AVFrame *frame), int (*AVFrame_callback2)(AVFrame *frame));

void CloseDemuxerDecoder();

const AVCodecContext* GetAVCodecContextAddress();

#endif