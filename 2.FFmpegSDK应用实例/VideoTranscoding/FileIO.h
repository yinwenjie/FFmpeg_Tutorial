#ifndef _FILE_IO_H_
#define _FILE_IO_H_

typedef struct _FileInOut
{
	const char *inputFileName;
	const char *outputFileName;
} FileInOut;

bool Open_input_file(FileInOut &files);

#endif