#ifndef _IO_FILES_H_
#define _IO_FILES_H_

typedef struct _IOFiles
{
	char *inputName;
	char *outputName;
	char *inputFrameSize;
	char *outputFrameSize;

	FILE *iFile;
	FILE *oFile;

} IOFiles;
 

#endif