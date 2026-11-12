cmake_minimum_required(VERSION 3.16)
project(ffmpeg_tutorial LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 默认带调试信息，方便用 gdb 单步跟进示例和 FFmpeg 内部。
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra)

include(cmake/FFmpeg.cmake)

# 每一讲一个子目录。L01 只有概念和命令行练习，没有代码。
add_subdirectory(L02_environment)
add_subdirectory(L03_open_media)
add_subdirectory(L04_remux_timestamps)
add_subdirectory(L05_decode_video)
add_subdirectory(L06_decode_audio)
add_subdirectory(L07_demux_decode)
add_subdirectory(L08_frame_types)
add_subdirectory(L09_scale_video)
add_subdirectory(L10_resample_audio)
add_subdirectory(L11_filter_audio)
add_subdirectory(L12_decode_filter)
add_subdirectory(L13_encode_video)
add_subdirectory(L14_encode_audio)
add_subdirectory(L15_mux)
add_subdirectory(L16_transcode)
add_subdirectory(L17_transcode_aac)
add_subdirectory(L18_avio_callbacks)
