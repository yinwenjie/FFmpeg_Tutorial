@ECHO OFF

TITLE FFPlay

IF NOT EXIST bin\ffplay.exe (
  CLS
  ECHO bin\ffplay.exe could not be found.
  GOTO:error
)

ECHO ffplay found.
CD bin || GOTO:error

ffplay.exe -window_title "FFPlay Demo" -x 720 -y 576 -t 00:10 -autoexit -i ../video/2_football.h264

:error
ECHO.
ECHO Press any key to exit.
PAUSE >nul
GOTO:EOF