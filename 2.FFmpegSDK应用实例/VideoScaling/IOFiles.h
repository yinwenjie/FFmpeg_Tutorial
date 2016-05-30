#ifndef _IO_FILES_H_
#define _IO_FILES_H_

/*************************************************
	Struct:			IOFiles
	Description:	接收命令行参数
*************************************************/
typedef struct _IOFiles
{
	char *inputName;			//输入文件名
	char *outputName;			//输出文件名
	char *inputFrameSize;		//输入图像的尺寸
	char *outputFrameSize;		//输出图像的尺寸

	FILE *iFile;				//输入文件指针
	FILE *oFile;				//输出文件指针

} IOFiles;
 

#endif