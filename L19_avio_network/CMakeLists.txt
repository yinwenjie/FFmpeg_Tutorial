find_package(Threads REQUIRED)

foreach(prog avio_list_dir avio_http_serve_files avio_ls avio_cat avio_serve)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()

# 官方示例 avio_http_serve_files.c 用了 fork()，-std=c11 下要这个宏才看得到它的声明。
target_compile_definitions(avio_http_serve_files PRIVATE _POSIX_C_SOURCE=200809L)
# avio_serve.c 每个客户端一个线程。
target_link_libraries(avio_serve PRIVATE Threads::Threads)

# avio_serve 默认用 POSIX 的 opendir() 列目录；打开这个选项换成 libavformat 的 avio_open_dir()
# （要打了 patch 0093 的 libavformat，原因见 README 第 9 步）：cmake -DUSE_AVIO_DIR=ON …
option(USE_AVIO_DIR "avio_serve 用 avio_open_dir() 列目录" OFF)
if(USE_AVIO_DIR)
    target_compile_definitions(avio_serve PRIVATE USE_AVIO_DIR)
endif()
