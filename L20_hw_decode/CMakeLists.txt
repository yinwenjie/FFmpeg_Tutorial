foreach(prog hw_decode hw_info hw_decode_av)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# 带检查的构建：cmake -DSANITIZE=address,undefined …
set(SANITIZE "" CACHE STRING "要启用的 sanitizer，例如 address,undefined")
if(SANITIZE)
    foreach(prog hw_decode hw_info hw_decode_av)
        target_compile_options(${prog} PRIVATE -O1 -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${prog} PRIVATE -fsanitize=${SANITIZE})
    endforeach()
endif()
