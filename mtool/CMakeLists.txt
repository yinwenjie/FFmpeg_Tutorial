# mtool：命令行媒体工具箱（综合项目）。
find_package(Threads REQUIRED)

file(GLOB MTOOL_SRCS CONFIGURE_DEPENDS src/*.c)
add_executable(mtool ${MTOOL_SRCS})
target_compile_definitions(mtool PRIVATE _GNU_SOURCE)
target_link_libraries(mtool PRIVATE PkgConfig::FFMPEG Threads::Threads m)

# 带检查的构建：cmake -DSANITIZE=address,undefined …（和 L22 一样）
if(SANITIZE)
    target_compile_options(mtool PRIVATE -O1 -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
    target_link_options(mtool PRIVATE -fsanitize=${SANITIZE})
endif()
