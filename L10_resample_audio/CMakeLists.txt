foreach(prog resample_audio resample_file)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG m)
endforeach()

# 官方示例 resample_audio.c 带 -Wextra 编译时有一个符号比较的警告，不影响运行。
target_compile_options(resample_audio PRIVATE -Wno-sign-compare)
