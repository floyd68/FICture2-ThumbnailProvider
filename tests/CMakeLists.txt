# ThumbnailProvider unit tests (enabled with -DTHUMBNAILPROVIDER_BUILD_TESTS=ON
# when configuring the FICture2 tree with FICTURE2_BUILD_THUMBNAIL_PROVIDER on;
# ThumbnailProvider has no standalone CMake project).
#
# The executable links the provider's OBJECT library, not the DLL, so the test
# code and the provider code share one ImageCore copy.

add_executable(ThumbnailProviderTests
    ThumbnailProviderTests.cpp
)

target_compile_definitions(ThumbnailProviderTests PRIVATE
    UNICODE
    _UNICODE
)

target_link_libraries(ThumbnailProviderTests PRIVATE
    ThumbnailProviderCore
)

if(MSVC)
    target_compile_options(ThumbnailProviderTests PRIVATE /permissive- /utf-8)
endif()

add_test(NAME ThumbnailProvider.Stream COMMAND ThumbnailProviderTests Stream_)
add_test(NAME ThumbnailProvider.GetThumbnail COMMAND ThumbnailProviderTests GetThumbnail_)
set_tests_properties(ThumbnailProvider.Stream ThumbnailProvider.GetThumbnail PROPERTIES TIMEOUT 120)
