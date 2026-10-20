include(FetchContent)

# Header-only; only src/bfc is used (object_registry on master).
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG ae9bd7637a730ef64f24a21fe99fe7eb8fa3fcb6
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
