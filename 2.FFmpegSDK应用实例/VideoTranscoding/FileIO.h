#ifndef _FILE_IO_H_
#define _FILE_IO_H_

typedef struct _FileInOut
{
	const char *inputFileName;
	const char *outputFileName;
} FileInOut;

typedef struct _TranscodingContext TranscodingContext;
/*************************************************
	Function:		Open_input_file
	Description:	打开输入文件
	Calls:			无
	Called By:		main
	Input:			(in/out) FileInOut &files : 输入和输出文件名	
					(in/out)trans_ctx : 转码器上下文对象
	Output:			无
	Return:			true : 打开文件成功
					false : 打开文件失败
*************************************************/
bool Open_input_file(FileInOut &files, TranscodingContext &trans_ctx);

/*************************************************
	Function:		Open_Output_file
	Description:	打开输出文件
	Calls:			无
	Called By:		main
	Input:			(in/out) FileInOut &files : 输入和输出文件名	
					(in/out)trans_ctx : 转码器上下文对象
	Output:			无
	Return:			true : 打开文件成功
					false : 打开文件失败
*************************************************/
bool Open_output_file(FileInOut &files, TranscodingContext &trans_ctx);

/*************************************************
	Function:		Read_packets_from_input_file
	Description:	从输入文件中读取packet
	Calls:			无
	Called By:		main
	Input:			(in/out)trans_ctx : 转码器上下文对象
	Output:			无
	Return:			true : 打开文件成功
					false : 打开文件失败
*************************************************/
bool Read_packets_from_input_file(TranscodingContext &trans_ctx);
#endif