#include "common.h"
#include "FileIO.h"

bool Open_input_file(FileInOut &files, TranscodingContext &trans_ctx)
{
	int ret = 0;

	
	//打开输入文件，并查找其中的流信息
	if ((ret = avformat_open_input(&(trans_ctx.ifmt_ctx), files.inputFileName, NULL, NULL)) < 0)
	{
		printf("Error: Open input file failed.\n");
		return false;
	}
	if ((ret = avformat_find_stream_info(trans_ctx.ifmt_ctx, NULL)) < 0)
	{
		printf("Error: Cannot find stream info.\n");
		return false;
	}

	//打开输入视频文件中的各种解码器
	AVStream *stream = NULL;
	AVCodecContext *codecCtx = NULL;
	for (int streamIdx = 0; streamIdx < trans_ctx.ifmt_ctx->nb_streams; streamIdx++)
	{
		stream = trans_ctx.ifmt_ctx->streams[streamIdx];
		codecCtx = stream->codec;

		if (codecCtx->codec_type == AVMEDIA_TYPE_VIDEO || codecCtx->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if ((ret = avcodec_open2(codecCtx, avcodec_find_decoder(codecCtx->codec_id), NULL)) < 0)
			{
				printf("Error: Open decoder for stream %d failed.\n", streamIdx);
				return false;
			}
		}
	}

	return true;
}

bool Open_output_file(FileInOut &files, TranscodingContext &trans_ctx)
{
	int ret = 0;
	AVStream *out_stream;
	AVStream *in_stream;
	AVCodecContext *dec_ctx, *enc_ctx;
	AVCodec *encoder;

	//
	avformat_alloc_output_context2(&(trans_ctx.ofmt_ctx), NULL, NULL, files.outputFileName);
	if (!trans_ctx.ofmt_ctx)
	{
		printf("Error: Allocating output file format failed.\n");
		return false;
	}

	for (int stream_idx = 0; stream_idx < trans_ctx.ifmt_ctx->nb_streams; stream_idx++)
	{
		out_stream = avformat_new_stream(trans_ctx.ofmt_ctx, NULL);
		if (!out_stream)
		{
			printf("Error: Adding new stream failed.\n");
			return false;
		}

		in_stream = trans_ctx.ifmt_ctx->streams[stream_idx];
		dec_ctx = in_stream->codec;
		enc_ctx = out_stream->codec;

		if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO || dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) 
			{
				encoder = avcodec_find_encoder(AV_CODEC_ID_HEVC);
				if (!encoder) 
				{
					printf("Error: Necessary encoder not found\n");
					return false;
				}

				enc_ctx->height = dec_ctx->height;
				enc_ctx->width = dec_ctx->width;
				enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
				enc_ctx->pix_fmt = encoder->pix_fmts[0];
				enc_ctx->time_base = dec_ctx->time_base;
			}
			else 
			{
				encoder = avcodec_find_encoder(dec_ctx->codec_id);
				if (!encoder) 
				{
					printf("Error: Necessary encoder not found\n");
					return false;
				}

				enc_ctx->sample_rate = dec_ctx->sample_rate;
				enc_ctx->channel_layout = dec_ctx->channel_layout;
				enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
				enc_ctx->sample_fmt = encoder->sample_fmts[0];
				AVRational r = {1, enc_ctx->sample_rate};
				enc_ctx->time_base = r;
			}
			ret = avcodec_open2(enc_ctx, encoder, NULL);
			if (ret < 0) 
			{
				printf("Error: Cannot open video encoder for stream #%u\n");
				return false;
			}
		}
		else
		{
			ret = avcodec_copy_context(trans_ctx.ofmt_ctx->streams[stream_idx]->codec,	trans_ctx.ifmt_ctx->streams[stream_idx]->codec);
			if (ret < 0) 
			{
				printf("Error: Copying stream context failed\n");
				return false;
			}
		}

		if (trans_ctx.ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		{
			enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
		}
	}
	av_dump_format(trans_ctx.ofmt_ctx, 0, files.outputFileName, 1);

	if (!(trans_ctx.ofmt_ctx->oformat->flags & AVFMT_NOFILE)) 
	{
		ret = avio_open(&(trans_ctx.ofmt_ctx->pb), files.outputFileName, AVIO_FLAG_WRITE);
		if (ret < 0) 
		{
			printf("Error: Could not open output file '%s'", files.outputFileName);
			return false;
		}
	}

	ret = avformat_write_header(trans_ctx.ofmt_ctx, NULL);
	if (ret < 0) 
	{
		printf("Error: Error occurred when opening output file\n");
		return false;
	}

	return true;
}

bool Read_packets_from_input_file(TranscodingContext &trans_ctx)
{
	int ret = av_read_frame(trans_ctx.ifmt_ctx, &(trans_ctx.packet));
	if (ret < 0)
	{
		return false;
	}
}
