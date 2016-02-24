@ECHO OFF
REM FF Prompt 1.2
REM Open a command prompt to run ffmpeg/ffplay/ffprobe
REM Copyright (C) 2013-2015  Kyle Schwarz

TITLE FFMpeg

IF NOT EXIST bin\ffmpeg.exe (
  CLS
  ECHO bin\ffmpeg.exe could not be found.
  GOTO:error
)

CD bin || GOTO:error

REM 封装格式转换，使用默认编码格式
REM ffmpeg -i ../video/IMG_0886.MOV ../video/output_mpeg4_mp3.avi 

REM 封装格式转换，保留编码格式
REM ffmpeg -i ../video/IMG_0886.MOV -c copy ../video/output_copy.avi 

REM 视频转码
REM ffmpeg -i ../video/IMG_0886.MOV -c:v mjpeg  ../video/output_mjpeg.avi 

@REM 提取视频流
@REM ffmpeg -i ../video/IMG_0886.MOV -c:v copy -an ../video/IMG_0886_v.MOV

@REM 提取音频流
@REM ffmpeg -i ../video/IMG_0886.MOV -c:a copy -vn ../video/IMG_0886_a.aac

@REM 视频截取
@REM ffmpeg -ss 5 -t 5 -i ../video/IMG_0886.MOV -c copy ../video/IMG_0886_cut.MOV

:error
ECHO.
ECHO Press any key to exit.
PAUSE >nul
GOTO:EOF