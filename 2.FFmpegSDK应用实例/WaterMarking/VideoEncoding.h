#ifndef _VIDEO_ENCODING_H_
#define _VIDEO_ENCODING_H_
#include "IOFile.h"
#include "VideoEncodingMuxingHeader.h"
#include "DemuxingContext.h"

int Application_encoding_start(IOFileName &files, EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx);

#endif