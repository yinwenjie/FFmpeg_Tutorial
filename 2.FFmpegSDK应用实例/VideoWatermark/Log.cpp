#include <stdio.h>
#include "common.h"
#include "Log.h"


FILE *pInputLogFile = NULL;
FILE *pOutputLogFile = NULL;

int InputLogInit()
{
#if LOG_OUT
	fopen_s(&pInputLogFile, "in_log.txt", "w+");
	if (!pInputLogFile)
	{
		return 0;
	}
#endif

	return 1;
}

void InputLogEnd()
{
#if LOG_OUT
	fclose(pInputLogFile);
	pInputLogFile = NULL;
#endif
}

void InputTextLogOut(char *pText, int64_t nPmt)
{
#if LOG_OUT
	fprintf_s(pInputLogFile, "%s: %ld", pText, nPmt);
#endif
}

void InputTextLogOut(char *pText)
{
#if LOG_OUT
	fprintf_s(pInputLogFile, "%s", pText);
#endif
}

int OutputLogInit()
{
#if LOG_OUT
	fopen_s(&pOutputLogFile, "out_log.txt", "w+");
	if (!pOutputLogFile)
	{
		return 0;
	}
#endif

	return 1;
}

void OutputLogEnd()
{
#if LOG_OUT
	fclose(pOutputLogFile);
	pOutputLogFile = NULL;
#endif
}

void OutputTextLogOut(char *pText, int64_t nPmt)
{
#if LOG_OUT
	fprintf_s(pOutputLogFile, "%s: %ld", pText, nPmt);
#endif
}

void OutputTextLogOut(char *pText)
{
#if LOG_OUT
	fprintf_s(pOutputLogFile, "%s", pText);
#endif
}