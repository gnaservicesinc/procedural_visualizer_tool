set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "openssl")
    # The frozen worker links only the release libraries.
    set(VCPKG_BUILD_TYPE release)
    # MSVC can corrupt the stack in optimized Windows ARM64 OpenSSL builds.
    # Keep the release CRT and assembly while disabling C/C++ optimization.
    # https://github.com/openssl/openssl/issues/27030
    set(VCPKG_C_FLAGS_RELEASE "/Od")
    set(VCPKG_CXX_FLAGS_RELEASE "/Od")
endif()
