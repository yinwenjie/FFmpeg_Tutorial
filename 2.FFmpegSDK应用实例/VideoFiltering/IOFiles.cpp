#include "common.h"

bool Read_yuv_data_to_buf(unsigned char *frame_buffer_in, const IOFiles &files, AVFrame **frameIn)
{
	AVFrame *pFrameIn = *frameIn;
	int width = files.frameWidth, height = files.frameHeight;
	int frameSize = width * height * 3 / 2;

	if (fread_s(frame_buffer_in, frameSize, 1, frameSize, files.iFile) != frameSize)
	{
		return false;
	}

	pFrameIn->data[0] = frame_buffer_in;
	pFrameIn->data[1] = pFrameIn->data[0] + width * height;
	pFrameIn->data[2] = pFrameIn->data[1] + width * height / 4;

	return true;
}
