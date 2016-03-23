#pragma once
#ifndef _INPUT_OUTPUT_H_
#define _INPUT_OUTPUT_H_

/*************************************************
Struct:			IOParam
Description:	接收命令行参数
*************************************************/
typedef struct
{
	FILE *pFin;
	FILE *pFout;

	char *pNameIn;
	char *pNameOut;
} IOParam;

/*************************************************
Function:		Parse_input_param
Description:	解析命令行传入的参数
Calls:			无
Called By:		main
Input:			(in)argc : 默认命令行参数
				(in)argv : 默认命令行参数
Output:			(out)io_param : 解析命令行的结果
Return:			true : 命令行解析正确
				false : 命令行解析错误
*************************************************/
void Parse(int argc, char **argv, IOParam &IOParam);

/*************************************************
Function:		Open_file
Description:	打开输入输出文件
Calls:			无
Called By:		main
Input:			(in/out)io_param : 传入文件的路径
Output:			(in/out)io_param : 用结构体成员保存打开文件的指针
Return:			true : 打开输入输出文件正确
				false : 打开输入输出文件错误
*************************************************/
bool Open_files(IOParam &IOParam);

/*************************************************
Function:		Close_file
Description:	关闭输入输出文件
Calls:			无
Called By:		main
Input:			(in)io_param : 保存输入输出文件的指针
Output:			无
Return:			无
*************************************************/
void Close_files(IOParam &IOParam);

#endif