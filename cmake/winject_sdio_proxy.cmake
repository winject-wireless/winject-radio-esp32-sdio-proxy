# Component dirs of the winject SDIO proxy, for an ESP-IDF project's
# EXTRA_COMPONENT_DIRS. Include before project.cmake:
#
#   include(<this repo>/cmake/winject_sdio_proxy.cmake)
#   list(APPEND EXTRA_COMPONENT_DIRS ${WINJECT_SDIO_PROXY_COMPONENT_DIRS})
#
# sdio_msg (the SDIO wire format) lives with the C6 firmware in
# winject-radio-esp32c6, so both ends of the link build from one copy.
# Override the checkout with -DFETCHCONTENT_SOURCE_DIR_WINJECT_RADIO_ESP32C6=<dir>.
include_guard(GLOBAL)
include(FetchContent)

set(WINJECT_SDIO_PROXY_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

FetchContent_Declare(winject_radio_esp32c6
    GIT_REPOSITORY https://github.com/winject-wireless/winject-radio-esp32c6.git
    GIT_TAG fd918b30879b9535922ef282e0133656941895a6
)
FetchContent_GetProperties(winject_radio_esp32c6)
if(NOT winject_radio_esp32c6_POPULATED)
    FetchContent_Populate(winject_radio_esp32c6)
endif()

set(WINJECT_SDIO_PROXY_COMPONENT_DIRS
    ${winject_radio_esp32c6_SOURCE_DIR}/components/sdio_msg
    ${WINJECT_SDIO_PROXY_ROOT}/components/sdio_link_host
    ${WINJECT_SDIO_PROXY_ROOT}/components/winject_proxy
)
