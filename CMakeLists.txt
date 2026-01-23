add_library(ThumbnailProvider SHARED)

target_sources(ThumbnailProvider PRIVATE
    ThumbnailProvider.cpp
    ThumbnailProvider.def
)

target_include_directories(ThumbnailProvider PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ImageCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/DirectXTex/DirectXTex
)

target_compile_definitions(ThumbnailProvider PRIVATE
    THUMBNAILPROVIDER_EXPORTS
    UNICODE
    _UNICODE
    _WINDOWS
)

target_link_libraries(ThumbnailProvider PRIVATE
    ImageCore
    DirectXTex
    windowscodecs
    shlwapi
    ole32
    uuid
)

if(MSVC)
    target_link_options(ThumbnailProvider PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/ThumbnailProvider.def")
    target_compile_options(ThumbnailProvider PRIVATE /permissive-)
endif()
