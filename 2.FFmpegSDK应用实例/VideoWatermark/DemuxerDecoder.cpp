#include "common.h"
#include "DemuxerDecoder.h"

static AVFormatContext		*fmt_ctx = NULL;
static AVStream				*video_stream = NULL;
static AVCodecContext		*video_dec_ctx = NULL;
static enum AVPixelFormat	pix_fmt;
static AVFrame				*frame = NULL;
static AVPacket				pkt;

static int		width, height;
static uint8_t	*video_dst_data[4] = {NULL};
static int      video_dst_linesize[4];
static int		video_dst_bufsize;
static int		video_stream_idx = -1;
static int		refcount = 0;
static int		input_packet_count = 0;

static FILE		*video_dst_file = NULL;

//**************************************************************************
//---------------------------------私有函数---------------------------------
//**************************************************************************
static int decode_packet(int *got_frame, int cached)
{
	static int frameIdx = 0;
	int ret = 0;
	int decoded = pkt.size;

	*got_frame = 0;
	if (pkt.stream_index == video_stream_idx) 
	{
		/* decode video frame */
		ret = avcodec_decode_video2(video_dec_ctx, frame, got_frame, &pkt);
		if (ret < 0) 
		{
			fprintf(stderr, "DemuxerDecoder Error: Error decoding video frame (%s)\n", ret);
			return ret;
		}
		if (*got_frame) 
		{
			if (frame->width != width || frame->height != height || frame->format != pix_fmt) 
			{
                /* To handle this change, one could call av_image_alloc again and
                 * decode the following frames into another rawvideo file. */
                fprintf(stderr, "Error: Width, height and pixel format have to be "
                        "constant in a rawvideo file, but the width, height or "
                        "pixel format of the input video changed:\n"
                        "old: width = %d, height = %d, format = %s\n"
                        "new: width = %d, height = %d, format = %s\n",
                        width, height, av_get_pix_fmt_name(pix_fmt),
                        frame->width, frame->height,
                        av_get_pix_fmt_name((AVPixelFormat)frame->format));
                return -1;
            }

			printf("Decoded: %d frames\n", frameIdx++);
		}
	}

	return decoded;
}

static int open_codec_context(int *stream_idx, AVFormatContext *fmt_ctx, enum AVMediaType type)
{
	int ret, stream_index;
	AVStream *st;
	AVCodecContext *dec_ctx = NULL;
	AVCodec *dec = NULL;
	AVDictionary *opts = NULL;

	ret = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);
	if (ret < 0) 
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not find %s stream in input file \n", av_get_media_type_string(type));
		return ret;
	}
	else 
	{
		stream_index = ret;
		st = fmt_ctx->streams[stream_index];

		/* find decoder for the stream */
		dec_ctx = st->codec;
		dec = avcodec_find_decoder(dec_ctx->codec_id);
		if (!dec)
		{
			fprintf(stderr, "DemuxerDecoder Error: Failed to find %s codec\n", av_get_media_type_string(type));
			return AVERROR(EINVAL);
		}

		/* Init the decoders, with or without reference counting */
		av_dict_set(&opts, "refcounted_frames", refcount ? "1" : "0", 0);
		if ((ret = avcodec_open2(dec_ctx, dec, &opts)) < 0)
		{
			fprintf(stderr, "DemuxerDecoder Error: Failed to open %s codec\n", av_get_media_type_string(type));
			return ret;
		}
		*stream_idx = stream_index;
	}

	return 0;
}

//**************************************************************************
//---------------------------------接口函数---------------------------------
//**************************************************************************

//初始化解复用-解码器
int InitDemuxerDecoder(const char *src_filename, const int yuv_out)
{
	int ret = 0;

	/*Hints*/
	printf("Prepare to Demuxer source file %s.\n", src_filename);

	/* open input file, and allocate format context */
	if (avformat_open_input(&fmt_ctx, src_filename, NULL, NULL) < 0)
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not open source file %s\n", src_filename);
		return -1;;
	}

	/* retrieve stream information */
	if (avformat_find_stream_info(fmt_ctx, NULL) < 0) 
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not find stream information\n");
		return -1;
	}

	if (open_codec_context(&video_stream_idx, fmt_ctx, AVMEDIA_TYPE_VIDEO) < 0) 
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not find stream information\n");
		return -1;
	}

	video_stream = fmt_ctx->streams[video_stream_idx];
	video_dec_ctx = video_stream->codec;

	if (yuv_out)
	{
		fopen_s(&video_dst_file,"temp.yuv", "wb");
		if (!video_dst_file) 
		{
			fprintf(stderr, "DemuxerDecoder Error: Could not open destination file temp.yuv\n");
			return -1;
		}
	}

	/* allocate image where the decoded image will be put */
	width = video_dec_ctx->width;
	height = video_dec_ctx->height;
	pix_fmt = video_dec_ctx->pix_fmt;
	ret = av_image_alloc(video_dst_data, video_dst_linesize, width, height, pix_fmt, 1);
	if (ret < 0) 
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not allocate raw video buffer\n");
		return -1;
	}
	video_dst_bufsize = ret;

	/* dump input information to stderr */
	av_dump_format(fmt_ctx, 0, src_filename, 0);

	if (!video_stream) 
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not find video stream in the input, aborting\n");
		return -1;
	}

	return 0;
}

//初始化AVFrame和AVPacket对象
int InitFramePacket()
{
	frame = av_frame_alloc();
	if (!frame) 
	{
		fprintf(stderr, "DemuxerDecoder Error: Could not allocate frame\n");
		return -1;
	}

	/* initialize packet, set data to NULL, let the demuxer fill it */
	av_init_packet(&pkt);
	pkt.data = NULL;
	pkt.size = 0;

	return 0;
}

//从文件中读取一个Packet
int ReadFramesFromFile()
{
	int ret = av_read_frame(fmt_ctx, &pkt);

	if (ret >= 0)
	{
		input_packet_count++;
		return 1;
	} 
	else
	{
		return 0;
	}
}

//解码出各帧图像
int DecodeOutFrames(const int yuv_out, void (*AVFrame_callback)(AVFrame *frame), int (*AVFrame_callback2)(AVFrame *frame))
{
	int ret, got_frame;
	AVPacket orig_pkt = pkt;
	do 
	{
		ret = decode_packet(&got_frame, 0);
		if (ret < 0)
		{
			break;
		}
		pkt.data += ret;
		pkt.size -= ret;

		if (got_frame)
		{
			frame->pts = frame->pkt_dts = frame->pkt_pts = pkt.dts;
			if (AVFrame_callback != NULL)
			{
				//AVFrame回调函数
				AVFrame_callback(frame);
			}

			if (AVFrame_callback2 != NULL)
			{
				AVFrame_callback2(frame);
			}

			if (yuv_out)
			{
				//写出yuv图像
				av_image_copy(video_dst_data, video_dst_linesize, (const uint8_t **)(frame->data), frame->linesize, pix_fmt, width, height);

				/* write to rawvideo file */
				fwrite(video_dst_data[0], 1, video_dst_bufsize, video_dst_file);
			}

//			av_frame_unref(frame);
		}
	} 
	while (pkt.size > 0);
	av_packet_unref(&orig_pkt);

	return got_frame;
}

//关闭解复用-解码器
void CloseDemuxerDecoder()
{
	avcodec_close(video_dec_ctx);
	avformat_close_input(&fmt_ctx);
	if (video_dst_file)
	{
		fclose(video_dst_file);
	}
	av_frame_free(&frame);
	av_free(video_dst_data[0]);

	printf("**************************\nTotal num of packets in input file; %d\n**************************\n", input_packet_count);
}

const AVCodecContext* GetAVCodecContextAddress()
{
	return video_dec_ctx;
}
