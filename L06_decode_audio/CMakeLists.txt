foreach(prog decode_audio sample_formats decode_wav)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# 官方示例 decode_audio.c 带 -Wextra 编译时有一个符号比较的警告，不影响运行。
target_compile_options(decode_audio PRIVATE -Wno-sign-compare)

# decode_wav.c 用到了 math.h 里的 lrint()，要链接数学库。
target_link_libraries(decode_wav PRIVATE m)
