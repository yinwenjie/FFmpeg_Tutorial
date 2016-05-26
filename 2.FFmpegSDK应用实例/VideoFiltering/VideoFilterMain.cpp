#include "common.h"

extern AVFilterContext *buffersrc_ctx;
extern AVFilterContext *buffersink_ctx;

/*************************************************
	Function:		hello
	Description:	解析命令行传入的参数
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)files : 解析命令行的结果
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
static bool hello(int argc, char **argv, IOFiles &files)
{
	printf("FFMpeg Filtering Demo.\nCommand format: %s inputfile outputfile logofile\n", argv[0]);
	if (argc != 4)
	{
		printf("Error: command line error, please re-check.\n");
		return false;
	}

	files.inputName = argv[1];
	files.outputName = argv[2];
	files.logoName = argv[3];

	fopen_s(&files.outputFile, files.outputName, "wb+");
	if (!files.outputFile)
	{
		printf("Error: Cannot open output file\n");
		return false;
	}

	return true;
}

static AVFormatContext *fmt_ctx;
static AVCodecContext *dec_ctx;
static int video_stream_index = -1;
static int64_t last_pts = AV_NOPTS_VALUE;

/*************************************************
	Function:		open_input_file
	Description:	解析命令行传入的参数
	Calls:			无
	Called By:		main
	Input:			(in)argc : 默认命令行参数
					(in)argv : 默认命令行参数					
	Output:			(out)files : 解析命令行的结果
	Return:			true : 命令行解析正确
					false : 命令行解析错误
*************************************************/
static int open_input_file(const char *filename)
{
	int ret;
	AVCodec *dec;

	if ((ret = avformat_open_input(&fmt_ctx, filename, NULL, NULL)) < 0)
	{
		printf("Error: Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(fmt_ctx, NULL)) < 0) 
	{
		printf("Error: Cannot find stream information\n");
		return ret;
	}

	/* select the video stream */
	ret = av_find_best_stream(fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) 
	{
		printf("Error: Cannot find a video stream in the input file\n");
		return ret;
	}
	video_stream_index = ret;
	dec_ctx = fmt_ctx->streams[video_stream_index]->codec;
	av_opt_set_int(dec_ctx, "refcounted_frames", 1, 0);

	/* init the video decoder */
	if ((ret = avcodec_open2(dec_ctx, dec, NULL)) < 0) 
	{
		printf("Error: Cannot open video decoder\n");
		return ret;
	}

	return 0;
}
/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
//const char *filter_descr = "drawtext=fontfile=/Windows/Fonts/Tahoma.ttf:text='FFMpeg Video Filter -- Video Watermark':x=100:y=x/dar:fontsize=24:fontcolor=yellow";
const char *filter_descr = "movie=logo.png[wm];[in][wm]overlay=5:5[out]";
#define TOTAL_FRAME_NUM 200
int main(int argc, char **argv)
{
	int ret = 0;
	IOFiles files = { NULL };
	if (!hello(argc, argv, files))
	{
		return -1;
	}

	av_register_all();

	if (open_input_file(files.inputName))
	{
		return -1;
	}

	if (Init_filters(filter_descr,dec_ctx) < 0)
	{
		return -1;
	}	

	AVFrame *frameIn  = av_frame_alloc();
	AVFrame *frameOut = av_frame_alloc();
	AVPacket packet;
	int got_frame = 0, frameIdx = 0;

	while (1)
	{
		if( (ret = av_read_frame(fmt_ctx, &packet)) < 0 || frameIdx++ > TOTAL_FRAME_NUM)
		{
			break;
		}

		if (packet.stream_index == video_stream_index)
		{
			if ( (ret = avcodec_decode_video2(dec_ctx, frameIn, &got_frame, &packet)) < 0 )
			{
				printf("Error: decode video frame failed.\n");
				return -1;
			}

			if (got_frame)
			{
				frameIn->pts = av_frame_get_best_effort_timestamp(frameIn);

				/* push the decoded frame into the filtergraph */
				if (av_buffersrc_add_frame_flags(buffersrc_ctx, frameIn, AV_BUFFERSRC_FLAG_KEEP_REF) < 0) 
				{
					printf( "Error while feeding the filtergraph\n");
					break;
				}

				while (1) 
				{
					if ( (ret = av_buffersink_get_frame(buffersink_ctx, frameOut)) < 0)
					{
						break;
					}

					printf("Process %d frame!\n", frameIdx++);
					Write_out_yuv(frameOut, &files.outputFile);
					av_frame_unref(frameOut);
				}
			}
			av_frame_unref(frameIn);
		}
		av_free_packet(&packet);
	}

	return 0;
}