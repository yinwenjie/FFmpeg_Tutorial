add_executable(hello_ffmpeg hello_ffmpeg.c)
target_link_libraries(hello_ffmpeg PRIVATE PkgConfig::FFMPEG)
