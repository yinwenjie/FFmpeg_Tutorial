foreach(prog extract_mvs frame_info grab_frame)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# 官方示例 extract_mvs.c 带 -Wextra 编译时有一个符号比较的警告，不影响运行。
target_compile_options(extract_mvs PRIVATE -Wno-sign-compare)
