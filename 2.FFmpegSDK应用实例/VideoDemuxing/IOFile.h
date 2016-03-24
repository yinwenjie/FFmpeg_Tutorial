#ifndef _IOFILE_H_
#define _IOFILE_H_

typedef struct 
{
	const char *src_filename;
	const char *video_dst_filename;
	const char *audio_dst_filename;

	FILE *video_dst_file;
	FILE *audio_dst_file;
} IOFileName;

#endif