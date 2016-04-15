#include "VideoDemuxing.h"

int Application_demuxing_start(IOFileName &files, DemuxingVideoAudioContex &demux_ctx)
{
	if (InitDemuxContext(files, demux_ctx) < 0)	//初始化解复用上下文结构体，包括注册组件、打开输入输出文件、查找音视频流等
	{		
		return -1;
	}

	demux_ctx.frame = av_frame_alloc();			//分配AVFrame结构对象
	if (!demux_ctx.frame)
	{
		fprintf(stderr, "Could not allocate frame\n");
		return -1;		
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&demux_ctx.pkt);				//初始化AVPacket对象
	demux_ctx.pkt.data = NULL;
	demux_ctx.pkt.size = 0;

	return 0;
}

void Application_demuxing_end(IOFileName &files, DemuxingVideoAudioContex &demux_ctx)
{
	CloseDemuxContext(files, demux_ctx);
}
