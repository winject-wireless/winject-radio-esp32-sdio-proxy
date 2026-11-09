include(FetchContent)

# bfc, pinned once per build. The top-level project (rov-esp32p4, gnd-esp32p4)
# sets BFC_PIN and BFC_CONFIG_DIR (the directory holding its bfc_config.hpp)
# before including this file. This repo's own builds (host tests, IDF examples
# under it) fall back to its pin and to its default config in bfc_config/.
# A mismatched pin, or an outside project without a config, stops the configure.
set(SDIO_PROXY_BFC_PIN 3d68f8e61ef933718a51f248c76be8894d0cb491)
get_filename_component(SDIO_PROXY_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
string(FIND "${CMAKE_SOURCE_DIR}/" "${SDIO_PROXY_ROOT}/" SDIO_PROXY_own_build)

if(NOT DEFINED BFC_PIN)
    set(BFC_PIN ${SDIO_PROXY_BFC_PIN})
endif()
if(NOT "${BFC_PIN}" STREQUAL "${SDIO_PROXY_BFC_PIN}")
    message(FATAL_ERROR "winject-radio-esp32-sdio-proxy pins bfc ${SDIO_PROXY_BFC_PIN}, but the build uses bfc ${BFC_PIN}. Bump this repo's pin to match.")
endif()

if(NOT DEFINED BFC_CONFIG_DIR)
    if(SDIO_PROXY_own_build EQUAL 0)
        set(BFC_CONFIG_DIR ${SDIO_PROXY_ROOT}/bfc_config)
    else()
        message(FATAL_ERROR "winject-radio-esp32-sdio-proxy: the top-level project must set BFC_CONFIG_DIR to the directory of its bfc_config.hpp")
    endif()
endif()
if(NOT EXISTS ${BFC_CONFIG_DIR}/bfc_config.hpp)
    message(FATAL_ERROR "BFC_CONFIG_DIR ${BFC_CONFIG_DIR} has no bfc_config.hpp")
endif()

FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG ${BFC_PIN}
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
# A cached checkout can sit on another commit: check what is actually there.
execute_process(COMMAND git -C ${bfc_SOURCE_DIR} rev-parse HEAD
    OUTPUT_VARIABLE BFC_CHECKED_OUT OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR NOT BFC_CHECKED_OUT STREQUAL BFC_PIN)
    message(FATAL_ERROR "bfc at ${bfc_SOURCE_DIR} is ${BFC_CHECKED_OUT}, expected ${BFC_PIN}. Delete the bfc source dir and reconfigure.")
endif()

# What a consumer puts on its include path: bfc's headers and the project config.
set(BFC_INCLUDE_DIRS ${bfc_SOURCE_DIR}/src ${BFC_CONFIG_DIR})
