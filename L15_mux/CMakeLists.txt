foreach(prog mux mux_av extract_h264)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG m)
endforeach()
# 官方示例 mux.c 保持原样，它的几个函数有没用到的参数，编译时会有警告，这里关掉。
target_compile_options(mux PRIVATE -Wno-unused-parameter)
