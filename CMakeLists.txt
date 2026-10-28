# The provider code is an OBJECT library shared by the DLL and the test
# executable, so the tests exercise the same objects and link one ImageCore
# copy with them (ImageCore keeps per-thread state in a thread_local).
add_library(ThumbnailProviderCore OBJECT)

target_sources(ThumbnailProviderCore PRIVATE
    ThumbnailProvider.cpp
    ../ImageAlphaPresentation.cpp
)

target_include_directories(ThumbnailProviderCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../ImageCore
    ${CMAKE_CURRENT_SOURCE_DIR}/../external/DirectXTex/DirectXTex
)

target_compile_definitions(ThumbnailProviderCore PRIVATE
    THUMBNAILPROVIDER_EXPORTS
    UNICODE
    _UNICODE
    _WINDOWS
)

target_link_libraries(ThumbnailProviderCore PUBLIC
    ImageCore
    DirectXTex
    windowscodecs
    shlwapi
    ole32
    uuid
)

if(MSVC)
    target_compile_options(ThumbnailProviderCore PRIVATE /permissive-)
endif()

add_library(ThumbnailProvider SHARED)

target_sources(ThumbnailProvider PRIVATE
    ThumbnailProvider.def
)

target_link_libraries(ThumbnailProvider PRIVATE
    ThumbnailProviderCore
)

if(MSVC)
    target_link_options(ThumbnailProvider PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/ThumbnailProvider.def")
endif()

option(THUMBNAILPROVIDER_BUILD_TESTS "Build ThumbnailProvider unit tests (configure from the FICture2 tree)" OFF)
if(THUMBNAILPROVIDER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
