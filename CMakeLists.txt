cmake_minimum_required(VERSION 3.13)
project(Mill C)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m68000 -Os -Wall -Wno-pointer-sign")
# The Plug-In SDK's glue (sdk/Glue) finds MillPlugIn.h on the include path
include_directories(sdk/Interfaces)
# mill://license, written into the build as a 'TEXT' resource
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/pages.r
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/tools/pages.py ${CMAKE_CURRENT_BINARY_DIR}/pages.r
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/pages.py
            ${CMAKE_CURRENT_SOURCE_DIR}/src/license.html)
list(APPEND REZ_INCLUDE_PATH ${CMAKE_CURRENT_BINARY_DIR})
add_application(Mill
    src/mac_start.c src/mac_main.c src/mac_net.c src/mac_local.c src/mac_save.c src/mac_sys.c src/mac_login.c
    src/mac_init.c src/mac_memreport.c src/mac_upload.c src/mac_pages.c src/mac_plugin.c src/plug_http.c src/plug_https.c src/plug_file.c
    src/plug_gif.c src/plug_jpeg.c src/plug_pnm.c src/plug_gzip.c
    src/core/auth.c src/core/httpauth.c src/core/charset.c src/core/cookie.c src/core/filetypes.c src/core/fmt.c src/core/form.c src/core/gif.c src/core/inflate.c src/core/jpeg.c src/core/macenc.c src/core/pnm.c src/core/html.c src/core/imagemap.c src/core/image.c src/core/layout.c src/core/update.c src/core/url.c
    src/third_party/tjpgd/tjpgd.c
    src/mill.r
    CREATOR "MiLL")
# Where Check for updates looks, for a test copy; empty is the real site (version.h)
set(MILL_UPDATE_URL "" CACHE STRING "Address of latest.txt for Check for updates (empty: the default)")
# A test copy that writes a memory report (build.sh: MILL_MEMREPORT=1)
set(MILL_MEMREPORT "" CACHE STRING "Write the \"Mill Memory\" report (test builds only)")
if(MILL_MEMREPORT)
    target_compile_definitions(Mill PRIVATE MILL_MEMREPORT=1)
endif()
if(MILL_UPDATE_URL)
    target_compile_definitions(Mill PRIVATE "MILL_UPDATE_URL=\"${MILL_UPDATE_URL}\"")
endif()
# The splash in a small segment of its own, so it shows while the rest of Mill loads; and
# each built-in Plug-In in its own, unloaded when nothing uses it
target_link_options(Mill PRIVATE "LINKER:--mac-segments,${CMAKE_CURRENT_SOURCE_DIR}/mill.segmap")
# ...with their constants too (tools/segment-rodata.sh), not in Mill's globals
set_property(TARGET Mill PROPERTY RULE_LAUNCH_COMPILE "sh ${CMAKE_CURRENT_SOURCE_DIR}/tools/segment-rodata.sh ${CMAKE_CURRENT_SOURCE_DIR}/mill.segmap")
# The resources also come from these, which Rez's own dependency doesn't know about
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mill.r.rsrc.bin APPEND
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/version.h ${CMAKE_CURRENT_SOURCE_DIR}/src/icons.r
            ${CMAKE_CURRENT_SOURCE_DIR}/src/splash.r ${CMAKE_CURRENT_SOURCE_DIR}/src/listicons.r ${CMAKE_CURRENT_BINARY_DIR}/pages.r)
