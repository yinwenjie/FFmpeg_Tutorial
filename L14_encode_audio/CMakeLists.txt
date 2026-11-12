foreach(prog encode_audio encode_wav)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG m)
endforeach()
