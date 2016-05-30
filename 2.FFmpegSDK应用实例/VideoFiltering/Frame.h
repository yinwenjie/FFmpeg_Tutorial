#ifndef _FRAME_H_
#define _FRAME_H_

void Init_video_frame_in_out(AVFrame **frameIn, AVFrame **frameOut, unsigned char **frame_buffer_in, unsigned char **frame_buffer_out, int frameWidth, int frameHeight);

#endif