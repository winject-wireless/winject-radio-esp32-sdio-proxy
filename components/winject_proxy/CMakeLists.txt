# bfc (header-only, for the queue d-plane), pinned in cmake/BfcFetch.cmake.
# Not fetched during IDF's early requirement scan.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/BfcFetch.cmake)
endif()

idf_component_register(SRCS "winject_proxy.c" "mplane_route.c" "dplane_queue.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ${BFC_INCLUDE_DIRS}
                       REQUIRES sdio_link_host
                       PRIV_REQUIRES udp_ota lwip freertos log esp_system pthread)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    # bfc throws on fatal errors; with IDF's C++ exceptions off a throw aborts.
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
