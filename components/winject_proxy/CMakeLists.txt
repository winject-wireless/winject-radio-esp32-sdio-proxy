# bfc (header-only, for the queue d-plane), pinned in cmake/BfcFetch.cmake.
# Not fetched during IDF's early requirement scan.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/BfcFetch.cmake)
endif()

idf_component_register(SRCS "winject_proxy.c" "ota_client.c" "dplane_queue.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS ${bfc_SOURCE_DIR}/src
                       REQUIRES sdio_link_host
                       PRIV_REQUIRES lwip freertos log esp_http_server app_update esp_system pthread)

target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
    # bfc throws on fatal errors; with IDF's C++ exceptions off a throw aborts.
    $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
)
