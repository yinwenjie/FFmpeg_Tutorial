#include "common.h"
#include "IOFile.h"
#include "VideoDemuxing.h"
#include "VideoFilter.h"
#include "DecodeFilterVideo.h"
#include "VideoEncoding.h"

#define TWO_STEP 0

const int c_maxDecodedFrames = 1000;

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
	if (argc != 3) 
	{
		fprintf(stderr, "usage: %s input_file video_output_file\n", argv[0]);
		return -1;
	}

	files.src_filename = argv[1];

#if TWO_STEP
	files.dst_filename = "temp.yuv";
#else
	files.dst_filename = argv[2];
#endif

	printf("Watermarking %s to %s.\n", files.src_filename, files.dst_filename);
	return 0;
}

const char *filter_descr = "drawtext=enable:fontfile=arial.ttf:fontcolor='Red':fontsize=24: text='HONEYWELL'"; 

/*************************************************
Function:		main
Description:	入口点函数
*************************************************/
int main(int argc, char **argv)
{
	int ret = 0, gotframe = 0, frameIdx = 0;
	IOFileName files = {NULL};
	DemuxingVideoAudioContex demux_ctx = {NULL};
	EncodingContext enc_ctx = {NULL};

	//========================Initializtion=========================
	if(hello(files, argc, argv) < 0)
	{
		printf("Error: Command line format error.\n");
		return -1;
	}
	if (Application_demuxing_start(files, demux_ctx) < 0)
	{
		printf("Error: Demuxing context initialization failed.\n");
		goto decode_end;
	}
	if (Application_encoding_start(files, enc_ctx, demux_ctx))
	{
		printf("Error: Encoding context initialization failed.\n");
		goto end;
	}
	if (Init_video_filters(filter_descr, demux_ctx.video_dec_ctx))
	{
		printf("Error: init video filter failed. Exit.\n");
		goto decode_end;
	}

	if (Prepare_to_write_output_file(files, enc_ctx) < 0)
	{
		fprintf(stderr, "Could not open output file: '%s'.\n", files.dst_filename);
		goto decode_end;
	}

	while (av_read_frame(demux_ctx.fmt_ctx, &demux_ctx.pkt) >= 0 && (frameIdx++ < c_maxDecodedFrames))		//从输入程序中读取一个包的数据
	{
		do 
		{
			ret = Decode_this_packet_to_frame(files, demux_ctx, &gotframe, 0);
			if (ret < 0)
			{
				break;
			}

			if (gotframe)
			{
//				Encode_frame_to_packet(enc_ctx, demux_ctx);
				av_frame_unref(demux_ctx.frame);
			}
			demux_ctx.pkt.data += ret;
			demux_ctx.pkt.size -= ret;
		}
		while (demux_ctx.pkt.size > 0);
	}

	demux_ctx.pkt.data = NULL;
	demux_ctx.pkt.size = 0;
	do 
	{
		Decode_this_packet_to_frame(files, demux_ctx, &gotframe, 1);	//解码缓存中未输出的包
	}
	while (gotframe);

decode_end:
	Application_demuxing_end(files, demux_ctx);
	
end:
	return 0;
}