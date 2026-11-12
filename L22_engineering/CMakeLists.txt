find_package(Threads REQUIRED)

foreach(prog reconfig_graph log_demo err_codes version_info)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG Threads::Threads)
endforeach()

# 带检查的构建：cmake -DSANITIZE=address,undefined …  或者  cmake -DSANITIZE=thread …
# 编译和链接都要带上同样的选项。address 和 thread 不能同时用，所以要两个构建目录。
set(SANITIZE "" CACHE STRING "要启用的 sanitizer，例如 address,undefined 或 thread")
if(SANITIZE)
    foreach(prog reconfig_graph log_demo err_codes version_info)
        target_compile_options(${prog} PRIVATE -O1 -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${prog} PRIVATE -fsanitize=${SANITIZE})
    endforeach()
endif()

# fflog.h 里的锁可以去掉，用来观察多线程下不加锁的后果：cmake -DFFLOG_NO_LOCK=ON …
option(FFLOG_NO_LOCK "去掉 fflog.h 回调里的锁（只用于观察后果）" OFF)
if(FFLOG_NO_LOCK)
    target_compile_definitions(log_demo PRIVATE FFLOG_NO_LOCK)
endif()

# 回归测试：cd build && ctest --output-on-failure
# 这里只放不需要素材的几项；全部检查用 L22_engineering/regress.sh。
enable_testing()
add_test(NAME err_codes_demo COMMAND err_codes demo)
add_test(NAME version_info COMMAND version_info -q)
add_test(NAME log_threads COMMAND log_demo threads 8 2000)
set_tests_properties(log_threads PROPERTIES
    PASS_REGULAR_EXPRESSION "应该有 16000 行警告，实际 16000 行")
