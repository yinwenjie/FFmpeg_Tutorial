#include <stdio.h>

#include "VideoDecodingHeader.h"

void hello()
{
	printf("*********************************\n");
	printf("VideoDecoding: A FFmpeg SDK demo.\nDeveloped by Yin Wenjie\n");
	printf("*********************************\n");
	printf("=================================\nCompulsory Paramaters:\n");
	printf("\t-i:\tInput YUV file name\n");
	printf("\t-o:\tOutput stream file name\n");
	printf("*********************************\n");
}

int main(int argc, char **argv)
{
	hello();
}