#ifndef _VIDEO_ENCODING_H_
#define _VIDEO_ENCODING_H_
#include "IOFile.h"
#include "VideoEncodingMuxingHeader.h"
#include "DemuxingContext.h"

int Application_encoding_start(IOFileName &files, EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx);

int Prepare_to_write_output_file(IOFileName &files, EncodingContext &enc_ctx);

int Encode_frame_to_packet(EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx);

#endif