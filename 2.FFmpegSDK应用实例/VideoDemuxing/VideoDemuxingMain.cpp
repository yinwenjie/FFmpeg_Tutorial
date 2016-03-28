#include <stdio.h>
#include "DemuxingContext.h"
#include "AVDecoder.h"


/*************************************************
Function:		hello
Description:	输出提示信息和命令行格式
Calls:			无
Called By:		main
Input:			无
Output:			无
Return:			无
*************************************************/
static int hello(IOFileName &files, int argc, char **argv)
{
	if (argc != 4 && argc != 5) 
	{
		fprintf(stderr, "usage: %s [-refcount] input_file video_output_file audio_output_file\n"
			"API example program to show how to read frames from an input file.\n"
			"This program reads frames from a file, decodes them, and writes decoded\n"
			"video frames to a rawvideo file named video_output_file, and decoded\n"
			"audio frames to a rawaudio file named audio_output_file.\n\n"
			"If the -refcount option is specified, the program use the\n"
			"reference counting frame system which allows keeping a copy of\n"
			"the data for longer than one decode call.\n"
			"\n", argv[0]);
		return -1;
	}

	if (argc == 5 && !strcmp(argv[1], "-refcount"))
	{
		files.refcount = 1;
		argv++;
	}

	files.src_filename = argv[1];
	files.video_dst_filename = argv[2];
	files.audio_dst_filename = argv[3];

	return 0;
}

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	int ret = 0, got_frame;
	IOFileName files = {NULL};
	DemuxingVideoAudioContex va_ctx = {NULL};

	hello(files, argc, argv);

	if (InitDemuxContext(files, va_ctx) < 0)
	{
		goto end;
	}

	va_ctx.frame = av_frame_alloc();
	if (!va_ctx.frame)
	{
		fprintf(stderr, "Could not allocate frame\n");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&va_ctx.pkt);
	va_ctx.pkt.data = NULL;
	va_ctx.pkt.size = 0;
	
	/* read frames from the file */
	while (av_read_frame(va_ctx.fmt_ctx, &va_ctx.pkt) >= 0)
	{
		AVPacket orig_pkt = va_ctx.pkt;
		do {
			ret = Decode_packet(files, va_ctx, &got_frame, 0);
			if (ret < 0)
				break;
			va_ctx.pkt.data += ret;
			va_ctx.pkt.size -= ret;
		} while (va_ctx.pkt.size > 0);
		av_packet_unref(&orig_pkt);
	}

	/* flush cached frames */
	va_ctx.pkt.data = NULL;
	va_ctx.pkt.size = 0;
	do 
	{
		Decode_packet(files, va_ctx, &got_frame, 1);
	}
	while (got_frame);

	printf("Demuxing succeeded.\n");

	if (va_ctx.video_stream) 
	{
		printf("Play the output video file with the command:\n"
			"ffplay -f rawvideo -pix_fmt %s -video_size %dx%d %s\n",
			av_get_pix_fmt_name(va_ctx.pix_fmt), va_ctx.width, va_ctx.height, files.video_dst_filename);
	}

	if (va_ctx.audio_stream) 
	{
		enum AVSampleFormat sfmt = va_ctx.audio_dec_ctx->sample_fmt;
		int n_channels = va_ctx.audio_dec_ctx->channels;
		const char *fmt;

		if (av_sample_fmt_is_planar(sfmt))
		{
			const char *packed = av_get_sample_fmt_name(sfmt);
			printf("Warning: the sample format the decoder produced is planar "
				"(%s). This example will output the first channel only.\n",
				packed ? packed : "?");
			sfmt = av_get_packed_sample_fmt(sfmt);
			n_channels = 1;
		}

		if ((ret = Get_format_from_sample_fmt(&fmt, sfmt)) < 0)
			goto end;

		printf("Play the output audio file with the command:\n"
			"ffplay -f %s -ac %d -ar %d %s\n",
			fmt, n_channels, va_ctx.audio_dec_ctx->sample_rate,
			files.audio_dst_filename);
	}

end:
	CloseDemuxContext(files, va_ctx);

	return ret < 0;
}