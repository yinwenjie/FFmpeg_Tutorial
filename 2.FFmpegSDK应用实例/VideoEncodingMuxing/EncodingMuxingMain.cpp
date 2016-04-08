#include "Stream.h"
#include "CoderMuxer.h"

#define STREAM_DURATION   10.0
#define STREAM_FRAME_RATE 25 /* 25 images/s */
#define STREAM_PIX_FMT    AV_PIX_FMT_YUV420P /* default pix_fmt */
#define SCALE_FLAGS SWS_BICUBIC

static int hello(int argc, char **argv, AVDictionary *opt, const char **filename)
{
	if (argc < 2) 
	{
		printf("usage: %s output_file\n"
			"API example program to output a media file with libavformat.\n"
			"This program generates a synthetic audio and video stream, encodes and\n"
			"muxes them into a file named output_file.\n"
			"The output format is automatically guessed according to the file extension.\n"
			"Raw images can also be output by using '%%d' in the filename.\n"
			"\n", argv[0]);
		return 1;
	}

	*filename = argv[1];
	if (argc > 3 && !strcmp(argv[2], "-flags")) 
	{
		av_dict_set(&opt, argv[2]+1, argv[3], 0);
	}

	return 0;
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	const char *filename = NULL;
	AVDictionary *opt = NULL;
	if (hello(argc, argv, opt, &filename))
	{
		return 1;
	}

	int ret;
	int have_video = 0, have_audio = 0;
	int encode_video = 0, encode_audio = 0;

	OutputStream video_st = { 0 }, audio_st = { 0 };
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	AVCodec *audio_codec, *video_codec;

	/* Initialize libavcodec, and register all codecs and formats. */
	av_register_all();

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

	return 0;
}