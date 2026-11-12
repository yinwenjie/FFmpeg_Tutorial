foreach(prog demux_decode codec_params decode_av)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# 官方示例 demux_decode.c 带 -Wextra 编译时有一个符号比较的警告，不影响运行。
target_compile_options(demux_decode PRIVATE -Wno-sign-compare)
