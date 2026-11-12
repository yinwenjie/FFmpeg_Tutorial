foreach(prog encode_video encode_yuv encoder_info)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()
