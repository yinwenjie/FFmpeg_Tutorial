#ifndef _DECODE_FILTER_VIDEO_H_
#define _DECODE_FILTER_VIDEO_H_

#include "IOFile.h"
#include "DemuxingContext.h"

int Decode_this_packet_to_frame(IOFileName &files, DemuxingVideoAudioContex &va_ctx, int *got_frame, int cached);

#endif