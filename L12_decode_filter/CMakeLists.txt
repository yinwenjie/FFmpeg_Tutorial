foreach(prog decode_filter_video decode_filter_audio filter_video)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()
