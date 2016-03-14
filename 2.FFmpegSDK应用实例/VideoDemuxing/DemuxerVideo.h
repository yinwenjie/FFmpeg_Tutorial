#pragma once
#ifndef _DEMUXER_VIDEO_H_
#define _DEMUXER_VIDEO_H_
#include "VideoDemuxingHeader.h"
#include "Stream.h"

AVFrame *Alloc_picture(enum AVPixelFormat pix_fmt, int width, int height);
void Open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg);

#endif