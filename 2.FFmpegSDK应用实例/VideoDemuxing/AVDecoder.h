#ifndef _AV_DECODER_H_
#define _AV_DECODER_H_

#include "IOFile.h"
#include "DemuxingContext.h"

int Get_format_from_sample_fmt(const char **fmt, enum AVSampleFormat sample_fmt);

int Decode_packet(IOFileName &files, DemuxingVideoAudioContex &va_ctx, int *got_frame, int cached);

#endif