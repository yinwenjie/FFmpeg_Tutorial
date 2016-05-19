#include "common.h"

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
static bool hello(int argc, char **argv, IOFiles &io_param)
{
	printf("FFMpeg Remuxing Demo.\nCommand format: %s inputfile outputfile\n", argv[0]);
	if (argc != 3)
	{
		printf("Error: command line error, please re-check.\n");
		return false;
	}

	io_param.inputName = argv[1];
	io_param.outputName = argv[2];

	return true;
}


/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	IOFiles io_param;
	if (!hello(argc, argv, io_param))
	{
		return -1;
	}

	AVOutputFormat *ofmt = NULL;
	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
	AVPacket pkt;
	int ret = 0;

	av_register_all();

	if ((ret = avformat_open_input(&ifmt_ctx, io_param.inputName, NULL, NULL)) < 0)
	{
		printf("Error: Open input file failed.\n");
		goto end;
	}
	if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0)
	{
		printf("Error: Failed to retrieve input stream information.\n");
		goto end;
	}
	av_dump_format(ifmt_ctx, 0, io_param.inputName, 0);

	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, io_param.outputName);
	if (!ofmt_ctx)
	{
		printf("Error: Could not create output context.\n");
		goto end;
	}
	ofmt = ofmt_ctx->oformat;

	for (unsigned int i = 0; i < ifmt_ctx->nb_streams ; i++)
	{
		AVStream *inStream = ifmt_ctx->streams[i];
		AVStream *outStream = avformat_new_stream(ofmt_ctx, inStream->codec->codec);
		if (!outStream)
		{
			printf("Error: Could not allocate output stream.\n");
			goto end;
		}

		ret = avcodec_copy_context(outStream->codec, inStream->codec);
		outStream->codec->codec_tag = 0;
		if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		{
			outStream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
	}

	av_dump_format(ofmt_ctx, 0, io_param.outputName, 1);

	if (!(ofmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&ofmt_ctx->pb, io_param.outputName, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			printf("Error: Could not open output file.\n");
			goto end;
		}
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) 
	{
		printf("Error: Could not write output file header.\n");
		goto end;
	}

	while (1) 
	{
		AVStream *in_stream, *out_stream;

		ret = av_read_frame(ifmt_ctx, &pkt);
		if (ret < 0)
			break;

		in_stream  = ifmt_ctx->streams[pkt.stream_index];
		out_stream = ofmt_ctx->streams[pkt.stream_index];
		
		/* copy packet */
		pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
		pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
		pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
		pkt.pos = -1;

		ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
		if (ret < 0) 
		{
			fprintf(stderr, "Error muxing packet\n");
			break;
		}
		av_free_packet(&pkt);
	}

	av_write_trailer(ofmt_ctx);

end:
	avformat_close_input(&ifmt_ctx);

	/* close output */
	if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
		avio_closep(&ofmt_ctx->pb);

	avformat_free_context(ofmt_ctx);

	if (ret < 0 && ret != AVERROR_EOF) 
	{
		fprintf(stderr, "Error failed to write packet to output file.\n");
		return 1;
	}
	return 0;
}