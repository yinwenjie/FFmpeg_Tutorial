# 通过 pkg-config 查找 FFmpeg，提供导入目标 PkgConfig::FFMPEG。
#
# 各讲的程序只需要：
#   target_link_libraries(<程序名> PRIVATE PkgConfig::FFMPEG)
#
# pkg-config 默认只搜索系统目录。课程用的 FFmpeg 装在 $HOME/ffmpeg_build，
# 所以需要先设置：
#   export PKG_CONFIG_PATH=$HOME/ffmpeg_build/lib/pkgconfig

find_package(PkgConfig REQUIRED)

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavdevice
    libavfilter
    libavformat
    libavcodec
    libswresample
    libswscale
    libavutil)

message(STATUS "FFmpeg: libavcodec ${FFMPEG_libavcodec_VERSION}，位于 ${FFMPEG_libavcodec_PREFIX}")

# 课程代码只在录制所用的版本（libavcodec 63.x）上测试过。
# 找到更旧的版本通常意味着 pkg-config 找到的是系统自带的 FFmpeg。
if(FFMPEG_libavcodec_VERSION VERSION_LESS 63)
    message(WARNING
        "找到的 libavcodec 版本是 ${FFMPEG_libavcodec_VERSION}，低于课程使用的 63.x。"
        "请检查 PKG_CONFIG_PATH 是否指向 $HOME/ffmpeg_build/lib/pkgconfig。")
endif()
