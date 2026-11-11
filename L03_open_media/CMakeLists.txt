foreach(prog show_metadata media_info probe_stages open_with_options)
    add_executable(${prog} ${prog}.c)
    target_link_libraries(${prog} PRIVATE PkgConfig::FFMPEG)
endforeach()
