#include "VideoEncoding.h"
#include "VideoMuxing.h"

int Application_encoding_start(IOFileName &files, EncodingContext &enc_ctx, DemuxingVideoAudioContex demux_ctx)
{
	/* allocate the output media context */
	avformat_alloc_output_context2(&(enc_ctx.oc), NULL, NULL, files.video_dst_filename);
	if (!enc_ctx.oc)
	{
		return -1;
	}
	enc_ctx.fmt = enc_ctx.oc->oformat;
	enc_ctx.fmt->video_codec = AV_CODEC_ID_H264;
	
	Add_video_stream(enc_ctx, demux_ctx);

	return 0;
}
