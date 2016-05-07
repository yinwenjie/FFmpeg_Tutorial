#ifndef _FILE_IO_H_
#define _FILE_IO_H_

typedef struct _FileInOut
{
	const char *inputFileName;
	const char *outputFileName;
} FileInOut;

/*************************************************
	Function:		Open_input_file
	Description:	打开输入文件
	Calls:			无
	Called By:		main
	Input:			(in) FileInOut &files : 输入和输出文件名									
	Output:			无
	Return:			true : 打开文件成功
					false : 打开文件失败
*************************************************/
bool Open_input_file(FileInOut &files);

/*************************************************
	Function:		Open_Output_file
	Description:	打开输出文件
	Calls:			无
	Called By:		main
	Input:			(in) FileInOut &files : 输入和输出文件名									
	Output:			无
	Return:			true : 打开文件成功
					false : 打开文件失败
*************************************************/
bool Open_output_file(FileInOut &files);

#endif