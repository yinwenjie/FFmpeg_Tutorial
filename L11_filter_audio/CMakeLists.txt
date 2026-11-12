foreach(prog filter_audio volume_file filter_info)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG m)
endforeach()

# 官方示例 filter_audio.c 把字符串缓冲区声明成了 uint8_t 数组，还有一处符号比较，编译时有警告，不影响运行。
target_compile_options(filter_audio PRIVATE -Wno-pointer-sign -Wno-sign-compare)
