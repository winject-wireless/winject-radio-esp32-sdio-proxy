include(FetchContent)

# Header-only; only src/bfc is used (queue registrar on master).
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG 4b29fcc2f2932ebe6a73087af70def3b36286520
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
