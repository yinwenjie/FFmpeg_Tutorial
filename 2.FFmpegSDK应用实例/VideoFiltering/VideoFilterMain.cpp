#include "common.h"

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

static AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
static AVCodecContext *dec_ctx = NULL, *enc_ctx = NULL;
static AVOutputFormat *ofmt = NULL;
static int video_stream_index = -1;

/*************************************************
	Function:		open_input_file
	Description:	按格式打开输入文件
	Calls:			无
	Called By:		main
	Input:			(in)filename : 传入的输入文件名							
	Output:			无
	Return:			函数执行错误码：非负：成功；负数：失败
*************************************************/
static int open_input_file(const char *filename)
{
	int ret;
	AVCodec *dec;

	if ((ret = avformat_open_input(&ifmt_ctx, filename, NULL, NULL)) < 0)
	{
		printf("Error: Cannot open input file\n");
		return ret;
	}

	if ((ret = avformat_find_stream_info(ifmt_ctx, NULL)) < 0) 
	{
		printf("Error: Cannot find stream information\n");
		return ret;
	}

	/* select the video stream */
	ret = av_find_best_stream(ifmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &dec, 0);
	if (ret < 0) 
	{
		printf("Error: Cannot find a video stream in the input file\n");
		return ret;
	}
	video_stream_index = ret;
	dec_ctx = ifmt_ctx->streams[video_stream_index]->codec;
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
	Function:		open_output_file
	Description:	按格式打开输入文件、写入文件头
	Calls:			无
	Called By:		main
	Input:			(in)filename : 传入的输入文件名							
	Output:			无
	Return:			函数执行错误码：非负：成功；负数：失败
*************************************************/
static int open_output_file(const char *filename)
{
	int ret = 0;

	avformat_alloc_output_context2(&ofmt_ctx, NULL, NULL, filename);
	if (!ofmt_ctx)
	{
		printf("Error: Could not create output context.\n");
		return -1;
	}
	ofmt = ofmt_ctx->oformat;

	for (unsigned int i = 0; i < ifmt_ctx->nb_streams ; i++)
	{
		AVStream *inStream = ifmt_ctx->streams[i];
		if (AV_CODEC_ID_NONE != inStream->codec->codec_id)
		{
			AVStream *outStream = avformat_new_stream(ofmt_ctx, inStream->codec->codec);
			if (!outStream)
			{
				printf("Error: Could not allocate output stream.\n");
				return -1;
			}

			ret = avcodec_copy_context(outStream->codec, inStream->codec);
			outStream->codec->codec_tag = 0;
			if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
			{
				outStream->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
			}
		}
		
	}
	av_dump_format(ofmt_ctx, 0, filename, 1);

	if (!(ofmt->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			printf("Error: Could not open output file.\n");
			return -1;
		}
	}

	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) 
	{
		printf("Error: Could not write output file header.\n");
		return -1;
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

	if (open_output_file("output.mp4") < 0)
	{
		return -1;
	}

	AVFrame *frameIn  = av_frame_alloc();
	AVFrame *frameOut = av_frame_alloc();
	AVPacket packet, outPacket;
	int got_frame = 0, frameIdx = 0;

	while (1)
	{
		if( (ret = av_read_frame(ifmt_ctx, &packet)) < 0)
		{
			break;
		}

		if (packet.stream_index == video_stream_index)
		{	
			//视频码流包
			if ( frameIdx > TOTAL_FRAME_NUM )
			{
				break;
			}

			if ( (ret = avcodec_decode_video2(dec_ctx, frameIn, &got_frame, &packet)) < 0 )
			{
				printf("Error: decode video frame failed.\n");
				return -1;
			}

			if (got_frame)
			{
				if (!Feed_filter_graph(frameIn))
				{
					break;
				}

				while (1) 
				{
					if (!Get_buffersink_frame(frameOut))
					{
						break;
					}

					printf("Process %d frame!\n", frameIdx++);
					Write_out_yuv(frameOut, &files.outputFile);
					
					{
						int got_packet = 0;
						av_init_packet(&outPacket);

						/* encode the image */
						ret = avcodec_encode_video2(enc_ctx, &outPacket, frameOut, &got_packet);
						if (ret < 0) 
						{
							fprintf(stderr, "Error encoding video frame: %d\n", ret);
							break;
						}
						if (got_packet)
						{
							//获得添加水印后的视频码流包
						}
					}
					
					av_frame_unref(frameOut);
				}
			}
			av_frame_unref(frameIn);
		}
		av_free_packet(&packet);
	}

	return 0;
}