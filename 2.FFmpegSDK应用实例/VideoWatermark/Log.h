#ifndef _LOG_H_
#define _LOG_H_
#include "stdint.h"

int InputLogInit();
void InputTextLogOut(char *pText);
void InputTextLogOut(char *pText, int64_t nPmt);
void InputLogEnd();

int OutputLogInit();
void OutputTextLogOut(char *pText);
void OutputTextLogOut(char *pText, int64_t nPmt);
void OutputLogEnd();


#endif