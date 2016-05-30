#ifndef _IO_FILES_H_
#define _IO_FILES_H_

/*************************************************
	Struct:			IOParam
	Description:	接收命令行参数
*************************************************/
typedef struct _IOFiles
{
	const char *inputFileName;		//输入文件名
	const char *outputFileName;		//输出文件名

	FILE *iFile;					//输入文件指针
	FILE *oFile;					//输出文件指针

	uint8_t filterIdx;				//Filter索引

	unsigned int frameWidth;		//图像宽度
	unsigned int frameHeight;		//图像高度
}IOFiles;

/*************************************************
	Function:		Read_yuv_data_to_buf
	Description:	从输入yuv文件中读取数据到buffer和frame结构
	Calls:			无
	Called By:		main
	Input:			(in)files : 包含输入文件的结构
	Output:			(out)frame_buffer_in : 指向缓存区的指针	
					(out)frameIn : 指向输入frame的指针
	Return:			无
*************************************************/
bool Read_yuv_data_to_buf(unsigned char *frame_buffer_in, const IOFiles &files, AVFrame **frameIn);

/*************************************************
	Function:		Write_yuv_to_outfile
	Description:	从输出frame中写出像素数据到输出文件
	Calls:			无
	Called By:		main
	Input:			(in)frame_out : filter graph输出的frame
	Output:			(out)files : 包含输出文件的结构	
	Return:			无
*************************************************/
void Write_yuv_to_outfile(const AVFrame *frame_out, IOFiles &files);

#endif