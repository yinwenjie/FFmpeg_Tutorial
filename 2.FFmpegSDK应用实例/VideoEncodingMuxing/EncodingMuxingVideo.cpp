#include "EncodingMuxingVideo.h"


FILE *g_inputYUVFile = NULL;

static AVFrame *alloc_picture(enum AVPixelFormat pix_fmt, int width, int height)
{
	AVFrame *picture;
	int ret;

	picture = av_frame_alloc();
	if (!picture)
	{
		return NULL;
	}

	picture->format = pix_fmt;
	picture->width = width;
	picture->height = height;

	/* allocate the buffers for the frame data */
	ret = av_frame_get_buffer(picture, 32);
	if (ret < 0)
	{
		fprintf(stderr, "Could not allocate frame data.\n");
		exit(1);
	}

	return picture;
}

void Open_video(AVFormatContext *oc, AVCodec *codec, OutputStream *ost, AVDictionary *opt_arg, IOParam &io)
{
	int ret;
	AVCodecContext *c = ost->st->codec;
	AVDictionary *opt = NULL;

	av_dict_copy(&opt, opt_arg, 0);

	/* open the codec */
	ret = avcodec_open2(c, codec, &opt);
	av_dict_free(&opt);
	if (ret < 0)
	{
		fprintf(stderr, "Could not open video codec: %d\n", ret);
		exit(1);
	}

	/* allocate and init a re-usable frame */
	ost->frame = alloc_picture(c->pix_fmt, c->width, c->height);
	if (!ost->frame)
	{
		fprintf(stderr, "Could not allocate video frame\n");
		exit(1);
	}

	/* If the output format is not YUV420P, then a temporary YUV420P
	* picture is needed too. It is then converted to the required
	* output format. */
	ost->tmp_frame = NULL;
	if (c->pix_fmt != AV_PIX_FMT_YUV420P)
	{
		ost->tmp_frame = alloc_picture(AV_PIX_FMT_YUV420P, c->width, c->height);
		if (!ost->tmp_frame)
		{
			fprintf(stderr, "Could not allocate temporary picture\n");
			exit(1);
		}
	}

	//打开输入YUV文件
	fopen_s(&g_inputYUVFile, io.input_file_name, "rb+");
	if (g_inputYUVFile == NULL)
	{
		fprintf(stderr, "Open input yuv file failed.\n");
		exit(1);
	}
}


static int write_frame(AVFormatContext *fmt_ctx, const AVRational *time_base, AVStream *st, AVPacket *pkt)
{
	/* rescale output packet timestamp values from codec to stream timebase */
	av_packet_rescale_ts(pkt, *time_base, st->time_base);
	pkt->stream_index = st->index;

	/* Write the compressed frame to the media file. */
	//	log_packet(fmt_ctx, pkt);
	return av_interleaved_write_frame(fmt_ctx, pkt);
}

static void fill_yuv_image(AVFrame *pict, int frame_index, int width, int height)
{
	int x, y, i, ret;

	/* when we pass a frame to the encoder, it may keep a reference to it
	* internally;
	* make sure we do not overwrite it here
	*/
	ret = av_frame_make_writable(pict);
	if (ret < 0)
	{
		exit(1);
	}

	i = frame_index;

	/* Y */
	for (y = 0; y < height; y++)
	{
		ret = fread_s(&pict->data[0][y * pict->linesize[0]], pict->linesize[0], 1, width, g_inputYUVFile);
		if (ret != width)
		{
			printf("Error: Read Y data error.\n");
			exit(1);
		}
	}

	/* U */
	for (y = 0; y < height / 2; y++) 
	{
		ret = fread_s(&pict->data[1][y * pict->linesize[1]], pict->linesize[1], 1, width / 2, g_inputYUVFile);
		if (ret != width / 2)
		{
			printf("Error: Read U data error.\n");
			exit(1);
		}
	}

	/* V */
	for (y = 0; y < height / 2; y++) 
	{
		ret = fread_s(&pict->data[2][y * pict->linesize[2]], pict->linesize[2], 1, width / 2, g_inputYUVFile);
		if (ret != width / 2)
		{
			printf("Error: Read V data error.\n");
			exit(1);
		}
	}
}


static AVFrame *get_video_frame(OutputStream *ost)
{
	AVCodecContext *c = ost->st->codec;

	/* check if we want to generate more frames */
	{
		AVRational r = { 1, 1 };
		if (av_compare_ts(ost->next_pts, ost->st->codec->time_base, STREAM_DURATION, r) >= 0)
		{
			return NULL;
		}
	}

	fill_yuv_image(ost->frame, ost->next_pts, c->width, c->height);

	ost->frame->pts = ost->next_pts++;

	return ost->frame;
}

int Write_video_frame(AVFormatContext *oc, OutputStream *ost)
{
	int ret;
	AVCodecContext *c;
	AVFrame *frame;
	int got_packet = 0;
	AVPacket pkt = { 0 };

	c = ost->st->codec;

	frame = get_video_frame(ost);

	av_init_packet(&pkt);

	/* encode the image */
	ret = avcodec_encode_video2(c, &pkt, frame, &got_packet);
	if (ret < 0) 
	{
		fprintf(stderr, "Error encoding video frame: %d\n", ret);
		exit(1);
	}

	if (got_packet)
	{
		ret = write_frame(oc, &c->time_base, ost->st, &pkt);
	}
	else 
	{
		ret = 0;
	}

	if (ret < 0)
	{
		fprintf(stderr, "Error while writing video frame: %d\n", ret);
		exit(1);
	}

	return (frame || got_packet) ? 0 : 1;
}

int Add_video_stream(AVStream **videoStream, AVFormatContext *oc, AVCodec **codec, enum AVCodecID codec_id)
{
	AVCodecContext *codecCtx = NULL;

	//查找编解码器
	if(!(*codec = avcodec_find_encoder(codec_id)))
	{
		printf("Error: Failed to find encoder while adding video stream.\n");
		return 0;
	}

	//生成视频流AVStream结构
	if (!(*videoStream = avformat_new_stream(oc, *codec)))
	{
		printf("Error: Failed to generate video stream while adding video stream.\n");
		return 0;
	}

	(*videoStream)->id = oc->nb_streams -1;
	codecCtx = (*videoStream)->codec;
	codecCtx->codec_id = codec_id;

	if (oc->oformat->flags & AVFMT_GLOBALHEADER)
	{
		codecCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}

	return 1;
}

void Set_video_stream(AVStream **videoStream, const VideoEncodingParam &encParam)
{
	AVCodecContext *codecCtx = (*videoStream)->codec;

	codecCtx->bit_rate = encParam.bitRate;
	codecCtx->width = encParam.frameWidth;
	codecCtx->height = encParam.frameHeight;
	codecCtx->gop_size = encParam.gopSize;
	if (codecCtx->codec_id == AV_CODEC_ID_MPEG2VIDEO)
	{
		codecCtx->max_b_frames = encParam.maxBFrames;
	}
	if (codecCtx->codec_id == AV_CODEC_ID_MPEG1VIDEO)
	{
		codecCtx->mb_decision = encParam.maxBFrames;
	}

	(*videoStream)->time_base = encParam.timeBase;
}
