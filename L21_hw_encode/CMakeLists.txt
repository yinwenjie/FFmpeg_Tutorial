set(L21_PROGRAMS vaapi_encode vaapi_transcode hw_enc_info hw_encode_av nv12_linesize)

foreach(prog ${L21_PROGRAMS})
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# 带检查的构建：cmake -DSANITIZE=address,undefined …
set(SANITIZE "" CACHE STRING "要启用的 sanitizer，例如 address,undefined")
if(SANITIZE)
    foreach(prog ${L21_PROGRAMS})
        target_compile_options(${prog} PRIVATE -O1 -fsanitize=${SANITIZE} -fno-omit-frame-pointer)
        target_link_options(${prog} PRIVATE -fsanitize=${SANITIZE})
    endforeach()
endif()
