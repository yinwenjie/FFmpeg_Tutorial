foreach(prog remux packet_info timebase_math remux_cut)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# remux.c 是官方示例的原样拷贝，里面有一处 int 和 unsigned 的比较，
# 用 -Wextra 编译会有警告。代码不能改，所以只对这个文件关掉这一项警告。
target_compile_options(remux PRIVATE -Wno-sign-compare)
