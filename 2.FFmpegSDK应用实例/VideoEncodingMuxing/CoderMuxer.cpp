#include "common.h"
#include "CoderMuxer.h"



int Open_coder_muxer(AVOutputFormat **fmt, AVFormatContext **oc, const char *filename)
{
	/* Initialize libavcodec, and register all codecs and formats. */
	av_register_all();

	/* allocate the output media context */
	avformat_alloc_output_context2(oc, NULL, NULL, filename);
	if (!oc)
	{
		printf("Could not deduce output format from file extension: using MPEG.\n");
		avformat_alloc_output_context2(oc, NULL, "mpeg", filename);
	}
	if (!oc)
	{
		return 1;
	}

	*fmt = (*oc)->oformat;

	return 0;
}
