#include <stdio.h>

#include "VideoDemuxingHeader.h"
#include "Stream.h"

int main(int argc, char **argv)
{
	OutputStream video_st = { 0 }, audio_st = { 0 };
	const char *filename;
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	AVCodec *audio_codec, *video_codec;
	int ret = 0;
	int have_video = 0, have_audio = 0;
	int encode_video = 0, encode_audio = 0;
	AVDictionary *opt = NULL;

	/* Initialize libavcodec, and register all codecs and formats. */
	av_register_all();

	if (argc < 2) {
		printf("usage: %s output_file\n"
			"API example program to output a media file with libavformat.\n"
			"This program generates a synthetic audio and video stream, encodes and\n"
			"muxes them into a file named output_file.\n"
			"The output format is automatically guessed according to the file extension.\n"
			"Raw images can also be output by using '%%d' in the filename.\n"
			"\n", argv[0]);
		return 1;
	}

	filename = argv[1];
	if (argc > 3 && !strcmp(argv[2], "-flags")) 
	{
		av_dict_set(&opt, argv[2]+1, argv[3], 0);
	}

	/* allocate the output media context */
	avformat_alloc_output_context2(&oc, NULL, NULL, filename);
	if (!oc) 
	{
		printf("Could not deduce output format from file extension: using MPEG.\n");
		avformat_alloc_output_context2(&oc, NULL, "mpeg", filename);
	}
	if (!oc)
		return 1;

	fmt = oc->oformat;
	/* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
    if (fmt->video_codec != AV_CODEC_ID_NONE) 
	{
        Add_stream(&video_st, oc, &video_codec, fmt->video_codec);
        have_video = 1;
        encode_video = 1;
    }
    if (fmt->audio_codec != AV_CODEC_ID_NONE)
	{
        Add_stream(&audio_st, oc, &audio_codec, fmt->audio_codec);
        have_audio = 1;
        encode_audio = 1;
    }

	return ret;
}