set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET "10.14")
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CMAKE_CONFIGURE_OPTIONS_freetype "-DFT_DISABLE_PNG=ON")

# Static Qt GUI pulls in libqcocoa.a which uses OpenGL functions.
# The framework link must be in the linker flags for all ports.
set(VCPKG_LINKER_FLAGS "-framework OpenGL")
