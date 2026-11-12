foreach(prog avio_read_callback avio_mem_in avio_mem_out)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()
