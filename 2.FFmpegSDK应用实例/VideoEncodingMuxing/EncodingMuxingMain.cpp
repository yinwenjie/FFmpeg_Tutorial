#include "Stream.h"
#include "CoderMuxer.h"
#include "EncodingMuxingVideo.h"
#include "EncodingMuxingAudio.h"


/*************************************************
	Function:		hello
	Description:	解析命令行传入的参数
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)io_param : 解析命令行的结果
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
static bool hello(int argc, char **argv, AVDictionary *opt, IOParam &ioParam)
{
	if (argc < 3) 
	{
		printf("usage: %s output_file input_file frame_width frame_height\n"
			"API example program to output a media file with libavformat.\n"
			"This program generates a synthetic audio and video stream, encodes and\n"
			"muxes them into a file named output_file.\n"
			"The output format is automatically guessed according to the file extension.\n"
			"Raw images can also be output by using '%%d' in the filename.\n"
			"\n", argv[0]);
		return false;
	}

	ioParam.output_file_name = argv[1];
	ioParam.input_file_name = argv[2];
	ioParam.frame_width = atoi(argv[3]);
	ioParam.frame_height = atoi(argv[4]);

	return true;
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	AVDictionary *opt = NULL;
	IOParam io = {NULL};
	if (!hello(argc, argv, opt, io))
	{
		return 1;
	}

	int ret;
	int have_video = 0, have_audio = 0;
	int encode_video = 0, encode_audio = 0;
	int videoFrameIdx = 0, audioFrameIdx = 0;

	OutputStream video_st = { 0 }, audio_st = { 0 };
	AVOutputFormat *fmt;
	AVFormatContext *oc;
	AVCodec *audio_codec = NULL, *video_codec = NULL;

	Open_coder_muxer(&fmt, &oc, io.output_file_name);

	/* Add the audio and video streams using the default format codecs
     * and initialize the codecs. */
	ret = Add_audio_video_streams(&video_st, &audio_st, oc, fmt, audio_codec, video_codec, io);
	have_video = ret & HAVE_VIDEO;
	encode_video = ret & ENCODE_VIDEO;
	have_audio = ret & HAVE_AUDIO;
	encode_audio = ret & ENCODE_AUDIO;

	 /* Now that all the parameters are set, we can open the audio and
     * video codecs and allocate the necessary encode buffers. */
	if (have_video)
	{
		Open_video(oc, video_codec, &video_st, opt, io);
	}
	if (have_audio)
	{
		Open_audio(oc, audio_codec, &audio_st, opt);
	}		

	av_dump_format(oc, 0, io.output_file_name, 1);
	/* open the output file, if needed */
	if (!(fmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&oc->pb, io.output_file_name, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			fprintf(stderr, "Could not open '%s': %d\n", io.output_file_name, ret);
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
		if (encode_video && (!encode_audio || av_compare_ts(video_st.next_pts, video_st.st->codec->time_base, audio_st.next_pts, audio_st.st->codec->time_base) <= 0))
		{
			encode_video = !Write_video_frame(oc, &video_st);
			if (encode_video)
			{
				printf("Write %d video frame.\n", videoFrameIdx++);
			}
			else
			{
				printf("Video ended, exit.\n");
			}
		}
		else 
		{
			encode_audio = !Write_audio_frame(oc, &audio_st);
			if (encode_audio)
			{
				printf("Write %d audio frame.\n", audioFrameIdx++);
			}
			else
			{
				printf("Audio ended, exit.\n");
			}
		}
	}
	
	//写入文件尾数据
	av_write_trailer(oc);

	/* Close each codec. */
	if (have_video)
	{
		Close_stream(oc, &video_st);
	}	
	if (have_audio)
	{
		Close_stream(oc, &audio_st);
	}

	if (!(fmt->flags & AVFMT_NOFILE))
	{
		//关闭输出文件
		avio_closep(&oc->pb);
	}

	//关闭输出文件的上下文句柄
	avformat_free_context(oc);

	printf("Procssing succeeded.\n");
	return 0;
}