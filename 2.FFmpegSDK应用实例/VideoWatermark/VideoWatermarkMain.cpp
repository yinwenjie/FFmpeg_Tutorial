#include "common.h"
#include "IOFile.h"
#include "DemuxingContext.h"
#include "DecodeFilterVideo.h"
#include "VideoFilter.h"

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
	files.video_dst_filename = argv[2];

	printf("Watermarking %s to %s.\n", files.src_filename, files.video_dst_filename);
	return 0;
}

const char *filter_descr = "drawtext=enable:fontfile=/Windows/Fonts/Tahoma.ttf:fontcolor='Red':fontsize=24: text='HONEYWELL'"; 

int main(int argc, char **argv)
{
	int ret = 0, gotframe;
	IOFileName files = {NULL};
	DemuxingVideoAudioContex va_ctx = {NULL};
	
	if(hello(files, argc, argv) < 0)
	{
		printf("Error: Command line format error.\n");
		return -1;
	}

	if (InitDemuxContext(files, va_ctx) < 0)	//初始化解复用上下文结构体，包括注册组件、打开输入输出文件、查找音视频流等
	{
		printf("Error: Demuxing context initialization failed.\n");
		goto end;
	}

	if (Init_video_filters(filter_descr, va_ctx.video_dec_ctx))
	{
		printf("Error: init video filter failed. Exit.\n");
		exit(1);
	}

	va_ctx.frame = av_frame_alloc();			//分配AVFrame结构对象
	if (!va_ctx.frame)
	{
		fprintf(stderr, "Could not allocate frame\n");
		ret = AVERROR(ENOMEM);
		goto end;
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&va_ctx.pkt);				//初始化AVPacket对象
	va_ctx.pkt.data = NULL;
	va_ctx.pkt.size = 0;

	/* read frames from the file */
	while (av_read_frame(va_ctx.fmt_ctx, &va_ctx.pkt) >= 0)		//从输入程序中读取一个包的数据
	{
		AVPacket orig_pkt = va_ctx.pkt;
		do 
		{
			ret = Decode_this_packet_to_frame(files, va_ctx, &gotframe, 0);
			if (ret < 0)
			{
				break;
			}
			
			va_ctx.pkt.data += ret;
			va_ctx.pkt.size -= ret;
		}
		while (va_ctx.pkt.size > 0);
		av_packet_unref(&orig_pkt);
	}

	/* flush cached frames */
	va_ctx.pkt.data = NULL;
	va_ctx.pkt.size = 0;
	do 
	{
		Decode_this_packet_to_frame(files, va_ctx, &gotframe, 1);	//解码缓存中未输出的包
	}
	while (gotframe);

end:
	CloseDemuxContext(files, va_ctx);
	return ret < 0;
}