#ifndef _COMMON_H_
#define _COMMON_H_

#include "VideoFilterHeader.h"
#include "VideoFilter.h"

//输入输出文件结构
typedef struct _IOFiles
{
	const char *inputName;
	const char *outputName;
	const char *logoName;
} IOFiles;

#endif