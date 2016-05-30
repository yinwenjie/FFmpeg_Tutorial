#ifndef _IO_FILES_H_
#define _IO_FILES_H_

typedef struct _IOFiles
{
	const char *inputFileName;
	const char *outputFileName;

	FILE *iFile;
	FILE *oFile;

	uint8_t filterIdx;

	unsigned int frameWidth;
	unsigned int frameHeight;
}IOFiles;

bool Read_yuv_data_to_buf(unsigned char *frame_buffer_in, const IOFiles &files, AVFrame **frameIn);

#endif