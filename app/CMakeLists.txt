# Standalone P4 proxy image: Ethernet + winject_proxy, exposing the C6 as a
# winject radio on UDP 22 (m-plane) and 9000 (d-plane).
cmake_minimum_required(VERSION 3.16)
if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "1.0.0")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../cmake/winject_sdio_proxy.cmake)
set(EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS})
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(winject-p4-proxy)
