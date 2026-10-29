include(FetchContent)

# Header-only; only src/bfc is used (the queue d-plane's queues, cv_reactor and
# registries). FetchContent keeps the first bfc declared, so in a project that
# also builds winject-l3 or vstreamer keep this pin equal to theirs.
FetchContent_Declare(bfc
    GIT_REPOSITORY https://github.com/therooftopprinz/BFC.git
    GIT_TAG f6fe9784bb8379eaa72a1597fe9e9638b97fb5fe
)
FetchContent_GetProperties(bfc)
if(NOT bfc_POPULATED)
    FetchContent_Populate(bfc)
endif()
