#include "Stream.h"
#include "CoderMuxer.h"
#include "EncodingMuxingVideo.h"
#include "EncodingMuxingAudio.h"
#include "VideoFilter.h"


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

const char *filter_descr = "drawtext=enable:fontfile=/Windows/Fonts/Tahoma.ttf:fontcolor='Red':fontsize=24: text='HONEYWELL'";  

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
	AVCodec *audio_codec = NULL, *video_codec = NULL;

	if (Open_coder_muxer(&fmt, &oc, filename) < 0)
	{
		printf("Error: Open coder/muxer failed. Exit.\n");
		exit(1);
	}
	
	/* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
	ret = Add_audio_video_streams(&video_st, &audio_st, oc, fmt, audio_codec, video_codec);
	have_video = ret & HAVE_VIDEO;
	encode_video = ret & ENCODE_VIDEO;
	have_audio = ret & HAVE_AUDIO;
	encode_audio = ret & ENCODE_AUDIO;

	 /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
    if (have_video)
        Open_video(oc, video_codec, &video_st, opt);

	if (have_audio)
		Open_audio(oc, audio_codec, &audio_st, opt);

	if (Init_video_filters(filter_descr, video_st.st->codec))
	{
		printf("Error: init video filter failed. Exit.\n");
		exit(1);
	}

	av_dump_format(oc, 0, filename, 1);
	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&oc->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			fprintf(stderr, "Could not open '%s': %d\n", filename, ret);
			return 1;
		}
	}

	/* Write the stream header, if any. */
	ret = avformat_write_header(oc, &opt);
	if (ret < 0)
	{
		fprintf(stderr, "Error occurred when opening output file: %d\n",ret);
		return 1;
	}

	while (encode_video || encode_audio) 
	{
		/* select the stream to encode */
		if (encode_video &&	(!encode_audio || av_compare_ts(video_st.next_pts, video_st.st->codec->time_base, audio_st.next_pts, audio_st.st->codec->time_base) <= 0))
		{
			encode_video = !Write_video_frame(oc, &video_st);
		}
		else 
		{
			encode_audio = !Write_audio_frame(oc, &audio_st);
		}
	}

	/* Write the trailer, if any. The trailer must be written before you
	* close the CodecContexts open when you wrote the header; otherwise
	* av_write_trailer() may try to use memory that was freed on
	* av_codec_close(). */
	av_write_trailer(oc);
	printf("Procssing succeeded.\n");

end:
	Close_video_filters();

	/* Close each codec. */
	if (have_video)
		Close_stream(oc, &video_st);
	if (have_audio)
		Close_stream(oc, &audio_st);

	if (!(fmt->flags & AVFMT_NOFILE))
		/* Close the output file. */
		avio_closep(&oc->pb);

	/* free the stream */
	avformat_free_context(oc);
	
	return 0;
}