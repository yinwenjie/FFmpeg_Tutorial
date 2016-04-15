#ifndef _VIDEO_DEMUXING_H_
#define _VIDEO_DEMUXING_H_
#include "DemuxingContext.h"

int Application_demuxing_start(IOFileName &files, DemuxingVideoAudioContex &demux_ctx);

void Application_demuxing_end(IOFileName &files, DemuxingVideoAudioContex &demux_ctx);

#endif