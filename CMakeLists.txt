cmake_minimum_required(VERSION 3.13)
project(Mill C)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m68000 -Os -Wall -Wno-pointer-sign")
add_application(Mill
    src/mac_start.c src/mac_main.c src/mac_net.c src/mac_local.c src/mac_save.c src/mac_sys.c src/mac_login.c
    src/core/auth.c src/core/charset.c src/core/cookie.c src/core/filetypes.c src/core/form.c src/core/html.c src/core/imagemap.c src/core/image.c src/core/layout.c src/core/url.c
    src/third_party/tjpgd/tjpgd.c
    src/mill.r
    CREATOR "MiLL")
# The splash in a small segment of its own, so it shows while the rest of Mill loads
target_link_options(Mill PRIVATE "LINKER:--mac-segments,${CMAKE_CURRENT_SOURCE_DIR}/mill.segmap")
# The resources also come from these, which Rez's own dependency doesn't know about
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mill.r.rsrc.bin APPEND
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h ${CMAKE_CURRENT_SOURCE_DIR}/src/icons.r
            ${CMAKE_CURRENT_SOURCE_DIR}/src/splash.r ${CMAKE_CURRENT_SOURCE_DIR}/src/pages.r)
