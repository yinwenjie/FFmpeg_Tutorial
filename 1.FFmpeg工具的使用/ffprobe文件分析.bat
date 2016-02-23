@ECHO OFF

TITLE FFprobe

IF NOT EXIST bin\ffprobe.exe (
  CLS
  ECHO bin\ffprobe.exe could not be found.
  GOTO:error
)

ECHO ffprobe found.
CD bin || GOTO:error

ffprobe.exe -i ../video/VideoCameraRecorder.avi

:error
ECHO.
ECHO Press any key to exit.
PAUSE >nul
GOTO:EOF