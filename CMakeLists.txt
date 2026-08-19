cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

option(PVT_ENABLE_DISTRIBUTION
    "Add a target that stages a self-contained macOS application" OFF)
option(PVT_BUILD_QT_GUI "Build the Qt 6 interactive GUI" OFF)
set(PVT_DEPLOY_QT_RUNTIME_DEFAULT OFF)
if(PVT_BUILD_QT_GUI AND (APPLE OR WIN32))
    # Those platforms do not normally provide a package-manager Qt runtime at
    # the installed executable's lookup path. A fresh GUI install should run.
    set(PVT_DEPLOY_QT_RUNTIME_DEFAULT ON)
endif()
option(PVT_DEPLOY_QT_RUNTIME
    "Deploy the Qt runtime and plugins when installing the GUI"
    ${PVT_DEPLOY_QT_RUNTIME_DEFAULT})
# Set this before project() initializes compiler flags. Qt 6.5 supports this
# baseline, and every native API used by the application is availability-
# guarded where necessary. A caller's explicit deployment target still wins.
if(PVT_ENABLE_DISTRIBUTION AND CMAKE_HOST_APPLE
   AND (NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET
        OR CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL ""))
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING
        "Minimum macOS version for generated binaries" FORCE)
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" PVT_PRODUCT_VERSION
    LIMIT_COUNT 1)
if(NOT PVT_PRODUCT_VERSION MATCHES
   "^(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)\\.(0|[1-9][0-9]*)(-[0-9A-Za-z][0-9A-Za-z.-]*)?$")
    message(FATAL_ERROR
        "VERSION must contain SemVer such as 0.9.0 or 1.0.0-RC1.")
endif()
string(REGEX REPLACE "-.*$" "" PVT_PRODUCT_VERSION_CORE
    "${PVT_PRODUCT_VERSION}")
project(ProceduralVisualizerTool VERSION "${PVT_PRODUCT_VERSION_CORE}" LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

option(PVT_BUILD_CLI "Build the command-line renderer" ON)
option(PVT_BUILD_EXAMPLES "Build the C++ library API example" OFF)
option(PVT_ENABLE_METAL "Build the optional Apple Metal rendering backend" ON)
option(PVT_ENABLE_OPENGL_SURFACE
    "Build the Qt-hosted OpenGL analytic-surface backend on Windows/Linux" ON)
set(PVT_DISTRIBUTION_CODE_SIGN_IDENTITY "-" CACHE STRING
    "macOS signing identity for distribution; '-' performs ad-hoc signing")
if(PVT_ENABLE_DISTRIBUTION AND NOT PVT_BUILD_QT_GUI)
    message(FATAL_ERROR
        "PVT_ENABLE_DISTRIBUTION requires PVT_BUILD_QT_GUI=ON.")
endif()
if(PVT_ENABLE_DISTRIBUTION AND NOT PVT_BUILD_CLI)
    message(FATAL_ERROR
        "PVT_ENABLE_DISTRIBUTION requires PVT_BUILD_CLI=ON so the named command-line renderer can be embedded in the application.")
endif()
if(PVT_DEPLOY_QT_RUNTIME AND NOT PVT_BUILD_QT_GUI)
    message(FATAL_ERROR
        "PVT_DEPLOY_QT_RUNTIME requires PVT_BUILD_QT_GUI=ON.")
endif()
set(PVT_METAL_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/metal-cpp"
    CACHE PATH "Directory containing Apple's metal-cpp headers")

function(pvt_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

set(PVT_DISTRIBUTION_LIBPNG OFF)
if(APPLE AND PVT_ENABLE_DISTRIBUTION)
    # A distribution must not inherit an arbitrary Homebrew/local libpng and
    # its deployment target. Build the already-required dependency from a
    # content-pinned upstream archive into a private static prefix instead.
    include(ExternalProject)
    find_package(ZLIB REQUIRED)
    set(PVT_DISTRIBUTION_LIBPNG_PREFIX
        "${CMAKE_CURRENT_BINARY_DIR}/_deps/pvt-libpng")
    set(PVT_DISTRIBUTION_LIBPNG_INSTALL
        "${PVT_DISTRIBUTION_LIBPNG_PREFIX}/install")
    file(MAKE_DIRECTORY "${PVT_DISTRIBUTION_LIBPNG_INSTALL}/include")
    ExternalProject_Add(pvt_distribution_libpng
        PREFIX "${PVT_DISTRIBUTION_LIBPNG_PREFIX}"
        URL
            "https://github.com/pnggroup/libpng/archive/4e3f57d50f552841550a36eabbb3fbcecacb7750.tar.gz"
        URL_HASH
            "SHA256=0327d94ddd66859be9eb904c71baa207e32869397d3d9936b7c1ba8191d6c6f3"
        CMAKE_CACHE_ARGS
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
            "-DCMAKE_BUILD_TYPE:STRING=${CMAKE_BUILD_TYPE}"
            "-DCMAKE_OSX_DEPLOYMENT_TARGET:STRING=${CMAKE_OSX_DEPLOYMENT_TARGET}"
            "-DCMAKE_OSX_ARCHITECTURES:STRING=${CMAKE_OSX_ARCHITECTURES}"
            "-DCMAKE_OSX_SYSROOT:PATH=${CMAKE_OSX_SYSROOT}"
            "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
            "-DPNG_LIBCONF_HEADER:FILEPATH=<SOURCE_DIR>/scripts/pnglibconf.h.prebuilt"
            "-DPNG_SHARED:BOOL=OFF"
            "-DPNG_STATIC:BOOL=ON"
            "-DPNG_FRAMEWORK:BOOL=OFF"
            "-DPNG_TESTS:BOOL=OFF"
            "-DPNG_TOOLS:BOOL=OFF"
            "-DPNG_HARDWARE_OPTIMIZATIONS:BOOL=OFF"
        BUILD_BYPRODUCTS
            "${PVT_DISTRIBUTION_LIBPNG_INSTALL}/lib/libpng16.a"
        INSTALL_DIR "${PVT_DISTRIBUTION_LIBPNG_INSTALL}"
        UPDATE_DISCONNECTED TRUE)
    ExternalProject_Get_Property(pvt_distribution_libpng SOURCE_DIR)
    set(PVT_DISTRIBUTION_LIBPNG_LICENSE "${SOURCE_DIR}/LICENSE")
    add_library(PNG::PNG STATIC IMPORTED GLOBAL)
    set_target_properties(PNG::PNG PROPERTIES
        IMPORTED_LOCATION
            "${PVT_DISTRIBUTION_LIBPNG_INSTALL}/lib/libpng16.a"
        INTERFACE_INCLUDE_DIRECTORIES
            "${PVT_DISTRIBUTION_LIBPNG_INSTALL}/include"
        INTERFACE_LINK_LIBRARIES ZLIB::ZLIB)
    add_dependencies(PNG::PNG pvt_distribution_libpng)
    set(PVT_DISTRIBUTION_LIBPNG ON)
else()
    find_package(PNG REQUIRED)
endif()
find_package(Threads REQUIRED)

# Product builds already require Qt for the editor. Discover it before the
# renderer target so Windows/Linux can use the same public Qt context and
# function-loading APIs for offscreen OpenGL surface mapping. A core-only
# library build remains free of Qt and uses the portable CPU implementation.
if(PVT_BUILD_QT_GUI)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent Network)
endif()

add_library(ProceduralVisualizerTool
    src/composite.cpp
    src/config_io.cpp
    src/core.cpp
    src/displacement_surface.cpp
    src/displacement_surface.h
    src/frame_renderer.cpp
    src/frame_renderer_internal.h
    src/image_io.cpp
    src/obj_mesh.cpp
    src/obj_mesh.h
    src/obj_surface.cpp
    src/obj_surface.h
    src/path_utf8.h
    src/source_image.cpp
    src/source_image.h
    src/windows_file_install.h
)

set(PVT_METAL_BACKEND_ENABLED OFF)
if(APPLE AND PVT_ENABLE_METAL)
    if(EXISTS "${PVT_METAL_CPP_DIR}/Metal/Metal.hpp"
       AND EXISTS "${PVT_METAL_CPP_DIR}/Foundation/Foundation.hpp")
        set(PVT_METAL_BACKEND_ENABLED ON)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/src/metal_kernels.metal")
        file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/metal_kernels.metal"
             PVT_METAL_KERNEL_SOURCE)
        # C++ only requires implementations to accept string literals through
        # 65,536 characters. Keep the embedded Metal source portable and avoid
        # AppleClang's overlength warning by emitting adjacent raw literals.
        set(PVT_METAL_KERNEL_SOURCE_LITERALS "")
        string(LENGTH "${PVT_METAL_KERNEL_SOURCE}"
            PVT_METAL_KERNEL_SOURCE_LENGTH)
        set(PVT_METAL_KERNEL_SOURCE_OFFSET 0)
        while(PVT_METAL_KERNEL_SOURCE_OFFSET LESS
              PVT_METAL_KERNEL_SOURCE_LENGTH)
            math(EXPR PVT_METAL_KERNEL_SOURCE_REMAINING
                "${PVT_METAL_KERNEL_SOURCE_LENGTH} - ${PVT_METAL_KERNEL_SOURCE_OFFSET}")
            if(PVT_METAL_KERNEL_SOURCE_REMAINING GREATER 60000)
                set(PVT_METAL_KERNEL_SOURCE_CHUNK_LENGTH 60000)
            else()
                set(PVT_METAL_KERNEL_SOURCE_CHUNK_LENGTH
                    ${PVT_METAL_KERNEL_SOURCE_REMAINING})
            endif()
            string(SUBSTRING "${PVT_METAL_KERNEL_SOURCE}"
                ${PVT_METAL_KERNEL_SOURCE_OFFSET}
                ${PVT_METAL_KERNEL_SOURCE_CHUNK_LENGTH}
                PVT_METAL_KERNEL_SOURCE_CHUNK)
            string(APPEND PVT_METAL_KERNEL_SOURCE_LITERALS
                "    R\"PVT_METAL(${PVT_METAL_KERNEL_SOURCE_CHUNK})PVT_METAL\",\n")
            math(EXPR PVT_METAL_KERNEL_SOURCE_OFFSET
                "${PVT_METAL_KERNEL_SOURCE_OFFSET} + ${PVT_METAL_KERNEL_SOURCE_CHUNK_LENGTH}")
        endwhile()
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/src/metal_kernels_source.h.in"
            "${CMAKE_CURRENT_BINARY_DIR}/generated/metal_kernels_source.h"
            @ONLY)
        target_sources(ProceduralVisualizerTool PRIVATE src/metal_backend.cpp)
        target_include_directories(ProceduralVisualizerTool SYSTEM PRIVATE
            "${PVT_METAL_CPP_DIR}")
        target_include_directories(ProceduralVisualizerTool PRIVATE
            "${CMAKE_CURRENT_BINARY_DIR}/generated")
        target_link_libraries(ProceduralVisualizerTool PRIVATE
            "-framework Foundation" "-framework Metal")
        target_compile_definitions(ProceduralVisualizerTool PRIVATE
            PVT_HAS_METAL_BACKEND=1)
    else()
        message(WARNING
            "PVT_ENABLE_METAL is ON, but metal-cpp was not found at "
            "${PVT_METAL_CPP_DIR}; building the CPU-only backend")
    endif()
endif()
if(NOT PVT_METAL_BACKEND_ENABLED)
    target_sources(ProceduralVisualizerTool PRIVATE src/metal_backend_stub.cpp)
endif()

set(PVT_OPENGL_SURFACE_BACKEND_ENABLED OFF)
if(PVT_ENABLE_OPENGL_SURFACE AND PVT_BUILD_QT_GUI
   AND (WIN32 OR (UNIX AND NOT APPLE)))
    set(PVT_OPENGL_SURFACE_BACKEND_ENABLED ON)
    target_sources(ProceduralVisualizerTool PRIVATE
        src/opengl_surface_backend.cpp)
    target_link_libraries(ProceduralVisualizerTool PRIVATE Qt6::Gui)
    target_compile_definitions(ProceduralVisualizerTool PRIVATE
        PVT_HAS_OPENGL_SURFACE_BACKEND=1)
else()
    target_sources(ProceduralVisualizerTool PRIVATE
        src/opengl_surface_backend_stub.cpp)
endif()
add_library(ProceduralVisualizerTool::ProceduralVisualizerTool
    ALIAS ProceduralVisualizerTool)
if(PVT_DISTRIBUTION_LIBPNG)
    # The imported target dependency supplies generated png headers as well as
    # the archive, so it must finish before compilation begins.
    add_dependencies(ProceduralVisualizerTool pvt_distribution_libpng)
endif()

target_compile_features(ProceduralVisualizerTool PUBLIC cxx_std_17)
target_compile_definitions(ProceduralVisualizerTool PRIVATE PVT_BUILDING_LIBRARY)
target_include_directories(ProceduralVisualizerTool
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(ProceduralVisualizerTool PRIVATE PNG::PNG Threads::Threads)
set_target_properties(ProceduralVisualizerTool PROPERTIES
    CXX_EXTENSIONS NO
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME ProceduralVisualizerTool
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(ProceduralVisualizerTool PUBLIC PVT_SHARED)
endif()

pvt_enable_warnings(ProceduralVisualizerTool)

# Music decoding and analysis are application/test features rather than part of
# the installed rendering ABI. The core-only installed consumer therefore does
# not inherit a decoder or a C-language requirement.
set(PVT_NEEDS_AUDIO_SUPPORT OFF)
if(PVT_BUILD_CLI OR PVT_BUILD_QT_GUI OR BUILD_TESTING)
    set(PVT_NEEDS_AUDIO_SUPPORT ON)
endif()

if(PVT_NEEDS_AUDIO_SUPPORT)
    enable_language(C)
    add_library(pvt_miniaudio STATIC
        third_party/miniaudio/miniaudio.c
        third_party/miniaudio/miniaudio.h)
    target_compile_features(pvt_miniaudio PRIVATE c_std_11)
    target_compile_definitions(pvt_miniaudio PRIVATE
        MA_NO_ENCODING
        MA_NO_ENGINE
        MA_NO_GENERATION
        MA_NO_NODE_GRAPH
        MA_NO_RESOURCE_MANAGER)
    target_include_directories(pvt_miniaudio SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio>)
    # Device I/O dynamically discovers native backends on Unix. Explicitly
    # carry its portable loader/thread requirements through this static target
    # instead of relying on whichever executable happens to consume it.
    target_link_libraries(pvt_miniaudio PRIVATE
        Threads::Threads ${CMAKE_DL_LIBS})
    set_target_properties(pvt_miniaudio PROPERTIES
        C_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(pvt_miniaudio PRIVATE /W0)
    else()
        target_compile_options(pvt_miniaudio PRIVATE -w)
    endif()

    # Beat-and-Tempo-Tracking is a causal, time-varying tracker. It supplies
    # independent local beat/tempo observations to the offline analyzer; no
    # whole-song fixed-BPM estimate is used to build the music clock.
    add_library(pvt_btt STATIC
        third_party/btt/src/BTT.c
        third_party/btt/src/DFT.c
        third_party/btt/src/Filter.c
        third_party/btt/src/STFT.c
        third_party/btt/src/Statistics.c
        third_party/btt/src/fastsin.c)
    target_compile_features(pvt_btt PRIVATE c_std_11)
    target_include_directories(pvt_btt SYSTEM PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/third_party/btt>)
    set_target_properties(pvt_btt PROPERTIES
        C_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_definitions(pvt_btt PRIVATE _USE_MATH_DEFINES)
        target_compile_options(pvt_btt PRIVATE /W0)
    else()
        target_compile_options(pvt_btt PRIVATE -w)
    endif()

    add_library(ProceduralVisualizerAudio STATIC
        src/audio_input_processing.cpp
        src/audio_input_processing.h
        src/audio_analysis.cpp
        src/audio_analysis.h
        src/live_audio_capture.cpp
        src/live_audio_capture.h
        src/audio_playback.cpp
        src/audio_playback.h)
    add_library(ProceduralVisualizerTool::Audio ALIAS ProceduralVisualizerAudio)
    target_compile_features(ProceduralVisualizerAudio PUBLIC cxx_std_17)
    target_include_directories(ProceduralVisualizerAudio PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
    target_link_libraries(ProceduralVisualizerAudio
        PUBLIC ProceduralVisualizerTool::ProceduralVisualizerTool
        PRIVATE pvt_miniaudio pvt_btt)
    set_target_properties(ProceduralVisualizerAudio PROPERTIES
        CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    pvt_enable_warnings(ProceduralVisualizerAudio)
endif()

# Project bundles are an application/test feature rather than part of the
# installed rendering library ABI. Keeping this helper non-installed prevents
# static consumers from inheriting minizip package requirements. A
# core-only configure with CLI, GUI, and tests disabled does not search for or
# fetch these dependencies.
set(PVT_NEEDS_BUNDLE_SUPPORT OFF)
if(PVT_BUILD_CLI OR PVT_BUILD_QT_GUI OR BUILD_TESTING)
    set(PVT_NEEDS_BUNDLE_SUPPORT ON)
endif()

if(PVT_NEEDS_BUNDLE_SUPPORT)
    find_package(ZLIB REQUIRED)
    set(PVT_MINIZIP_TARGET)
    find_package(minizip 4.2 CONFIG QUIET)
    if(TARGET MINIZIP::minizip)
        set(PVT_MINIZIP_TARGET MINIZIP::minizip)
    else()
        # Debian and Ubuntu package minizip-ng under its project name and
        # export a correspondingly named target. Prefer that system package
        # when it is available so distribution builds remain offline and
        # reproducible; upstream and vcpkg builds retain their existing path.
        find_package(minizip-ng 4.0 CONFIG QUIET)
        if(TARGET MINIZIP::minizip-ng)
            set(PVT_MINIZIP_TARGET MINIZIP::minizip-ng)
        endif()
    endif()
    if(NOT PVT_MINIZIP_TARGET)
        include(FetchContent)
        set(MZ_COMPAT OFF CACHE BOOL "" FORCE)
        set(MZ_ZLIB ON CACHE BOOL "" FORCE)
        set(MZ_ZLIB_FLAVOR zlib CACHE STRING "" FORCE)
        set(MZ_BZIP2 OFF CACHE BOOL "" FORCE)
        set(MZ_LZMA OFF CACHE BOOL "" FORCE)
        set(MZ_PPMD OFF CACHE BOOL "" FORCE)
        set(MZ_ZSTD OFF CACHE BOOL "" FORCE)
        set(MZ_LIBCOMP OFF CACHE BOOL "" FORCE)
        set(MZ_FETCH_LIBS OFF CACHE BOOL "" FORCE)
        set(MZ_FORCE_FETCH_LIBS OFF CACHE BOOL "" FORCE)
        set(MZ_PKCRYPT OFF CACHE BOOL "" FORCE)
        set(MZ_WZAES OFF CACHE BOOL "" FORCE)
        set(MZ_OPENSSL OFF CACHE BOOL "" FORCE)
        set(MZ_LIBBSD OFF CACHE BOOL "" FORCE)
        set(MZ_ICONV OFF CACHE BOOL "" FORCE)
        set(MZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
        set(MZ_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
        set(MZ_BUILD_FUZZ_TESTS OFF CACHE BOOL "" FORCE)
        if(DEFINED SKIP_INSTALL_ALL)
            set(_pvt_had_skip_install_all ON)
            set(_pvt_saved_skip_install_all "${SKIP_INSTALL_ALL}")
        else()
            set(_pvt_had_skip_install_all OFF)
        endif()
        set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
        set(_pvt_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
        set(BUILD_SHARED_LIBS OFF)
        FetchContent_Declare(minizip_ng
            GIT_REPOSITORY https://github.com/zlib-ng/minizip-ng.git
            GIT_TAG 7b2387161c542fa9f427352dcdef76097d0d692b)
        FetchContent_MakeAvailable(minizip_ng)
        set(PVT_MINIZIP_TARGET MINIZIP::minizip)
        set(BUILD_SHARED_LIBS "${_pvt_saved_build_shared_libs}")
        unset(_pvt_saved_build_shared_libs)
        if(_pvt_had_skip_install_all)
            set(SKIP_INSTALL_ALL "${_pvt_saved_skip_install_all}"
                CACHE BOOL "" FORCE)
        else()
            unset(SKIP_INSTALL_ALL CACHE)
        endif()
        unset(_pvt_had_skip_install_all)
        unset(_pvt_saved_skip_install_all)
    endif()

    add_library(ProceduralVisualizerBundle STATIC
        src/bundle_archive.cpp
        src/bundle_archive.h
        src/config_codec.cpp
        src/config_codec.h
        src/palette_io.cpp
        src/palette_io.h
        src/project_bundle.cpp
        src/project_bundle.h)
    add_library(ProceduralVisualizerTool::Bundle ALIAS ProceduralVisualizerBundle)
    target_compile_features(ProceduralVisualizerBundle PUBLIC cxx_std_17)
    set_target_properties(ProceduralVisualizerBundle PROPERTIES
        CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(ProceduralVisualizerBundle PRIVATE
        PVT_PROGRAM_VERSION="${PVT_PRODUCT_VERSION}")
    target_include_directories(ProceduralVisualizerBundle PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
    target_link_libraries(ProceduralVisualizerBundle
        PUBLIC ProceduralVisualizerTool::ProceduralVisualizerTool
        PRIVATE
            ${PVT_MINIZIP_TARGET}
            PNG::PNG
            ZLIB::ZLIB
            ProceduralVisualizerTool::Audio)
    if(PVT_DISTRIBUTION_LIBPNG)
        # palette_io.cpp includes png.h directly. Ensure the private staged
        # libpng install (headers and archive) exists before it is compiled.
        add_dependencies(ProceduralVisualizerBundle pvt_distribution_libpng)
    endif()
    pvt_enable_warnings(ProceduralVisualizerBundle)
endif()

if(PVT_BUILD_CLI)
    add_executable(pvt_render_cli app/cli_main.cpp)
    target_compile_features(pvt_render_cli PRIVATE cxx_std_17)
    set_target_properties(pvt_render_cli PROPERTIES
        CXX_EXTENSIONS NO
        OUTPUT_NAME "pvt-render")
    if(APPLE OR UNIX)
        file(RELATIVE_PATH pvt_cli_install_library_path
            "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    endif()
    if(APPLE)
        set_target_properties(pvt_render_cli PROPERTIES
            # Current Apple SDKs may encode the system C++ runtime as
            # @rpath/libc++.1.dylib. Keep /usr/lib in both the build and
            # installed search paths so the standalone and embedded CLI do
            # not depend on a developer-shell DYLD override.
            BUILD_RPATH "/usr/lib"
            INSTALL_RPATH
                "@executable_path/${pvt_cli_install_library_path};/usr/lib")
    elseif(UNIX)
        set_target_properties(pvt_render_cli PROPERTIES
            INSTALL_RPATH "\$ORIGIN/${pvt_cli_install_library_path}")
    endif()
    target_link_libraries(pvt_render_cli PRIVATE
        ProceduralVisualizerTool::Bundle
        ProceduralVisualizerTool::Audio)
    target_compile_definitions(pvt_render_cli PRIVATE
        PVT_PROGRAM_VERSION="${PVT_PRODUCT_VERSION}")
    if(PVT_OPENGL_SURFACE_BACKEND_ENABLED)
        target_compile_definitions(pvt_render_cli PRIVATE
            PVT_CLI_HAS_QT_OPENGL_SURFACE=1)
        target_link_libraries(pvt_render_cli PRIVATE Qt6::Gui)
    endif()
    pvt_enable_warnings(pvt_render_cli)
endif()

if(PVT_BUILD_EXAMPLES)
    add_executable(pvt_library_example examples/library_example.cpp)
    target_compile_features(pvt_library_example PRIVATE cxx_std_17)
    set_target_properties(pvt_library_example PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_library_example PRIVATE
        ProceduralVisualizerTool::ProceduralVisualizerTool)
    pvt_enable_warnings(pvt_library_example)
endif()

if(PVT_BUILD_QT_GUI)
    qt_standard_project_setup()

    set(PVT_GUI_VIDEO_SOURCES gui/video_export_stub.cpp)
    set(PVT_GUI_MIDI_SOURCES gui/live_midi_stub.cpp)
    if(APPLE)
        enable_language(OBJCXX)
        set(PVT_GUI_VIDEO_SOURCES gui/macos_video_export.mm)
        set(PVT_GUI_MIDI_SOURCES gui/live_midi_mac.mm)
        set_source_files_properties(gui/macos_video_export.mm PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
    endif()

    set(PVT_GUI_ICON_SOURCE)
    if(APPLE)
        find_program(PVT_SIPS_EXECUTABLE NAMES sips REQUIRED)
        find_program(PVT_ICONUTIL_EXECUTABLE NAMES iconutil REQUIRED)
        set(PVT_GUI_ICON_SOURCE
            "${CMAKE_CURRENT_BINARY_DIR}/generated/ProceduralVisualizerTool.icns")
        add_custom_command(
            OUTPUT "${PVT_GUI_ICON_SOURCE}"
            COMMAND "${CMAKE_COMMAND}"
                "-DPVT_ICON_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png"
                "-DPVT_ICON_OUTPUT=${PVT_GUI_ICON_SOURCE}"
                "-DPVT_SIPS=${PVT_SIPS_EXECUTABLE}"
                "-DPVT_ICONUTIL=${PVT_ICONUTIL_EXECUTABLE}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/GenerateIcns.cmake"
            DEPENDS
                "${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png"
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/GenerateIcns.cmake"
            COMMENT "Generating the macOS application icon"
            VERBATIM)
        set_source_files_properties("${PVT_GUI_ICON_SOURCE}" PROPERTIES
            GENERATED TRUE
            MACOSX_PACKAGE_LOCATION Resources)
    endif()

    qt_add_executable(ProceduralVisualizerToolGui MACOSX_BUNDLE
        gui/audio_processing_dialog.cpp
        gui/audio_processing_dialog.h
        gui/application_settings_dialog.cpp
        gui/application_settings_dialog.h
        gui/device_sleep_guard.cpp
        gui/device_sleep_guard.h
        gui/main.cpp
        gui/main_window.cpp
        gui/main_window.h
        gui/live_midi.h
        ${PVT_GUI_MIDI_SOURCES}
        gui/live_frame_controller.cpp
        gui/live_frame_controller.h
        gui/live_osc.cpp
        gui/live_osc.h
        gui/live_target_registry.cpp
        gui/live_target_registry.h
        gui/live_workspace.cpp
        gui/live_workspace.h
        gui/stage_output_window.cpp
        gui/stage_output_window.h
        gui/studio_widgets.cpp
        gui/studio_widgets.h
        gui/preview_widget.cpp
        gui/preview_widget.h
        gui/video_export.h
        gui/video_export_dialog.cpp
        gui/video_export_dialog.h
        ${PVT_GUI_VIDEO_SOURCES}
        ${PVT_GUI_ICON_SOURCE}
    )
    set_target_properties(ProceduralVisualizerToolGui PROPERTIES CXX_EXTENSIONS NO)
    if(APPLE)
        set_target_properties(ProceduralVisualizerToolGui PROPERTIES
            OUTPUT_NAME "Procedural Visualizer Tool"
            BUILD_RPATH "/usr/lib"
            MACOSX_BUNDLE_BUNDLE_NAME "Procedural Visualizer Tool"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.gnaservices.proceduralvisualizertool"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_ICON_FILE "ProceduralVisualizerTool.icns"
            MACOSX_BUNDLE_INFO_PLIST
                "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/Info.plist.in")
        if(PVT_ENABLE_DISTRIBUTION)
            # Keep the linker's bare build-tree bundle visibly separate from
            # the verified distributable. The final app occupies the obvious
            # top-level build path, matching normal macOS packaging practice.
            set_target_properties(ProceduralVisualizerToolGui PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY
                    "${CMAKE_CURRENT_BINARY_DIR}/distribution-intermediate")
        endif()
    else()
        set_target_properties(ProceduralVisualizerToolGui PROPERTIES
            OUTPUT_NAME "procedural-visualizer-tool")
        if(UNIX)
            file(RELATIVE_PATH pvt_gui_install_library_path
                "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
            set_target_properties(ProceduralVisualizerToolGui PROPERTIES
                INSTALL_RPATH "\$ORIGIN/${pvt_gui_install_library_path}")
        endif()
    endif()
    target_link_libraries(ProceduralVisualizerToolGui PRIVATE
        ProceduralVisualizerTool::Bundle
        ProceduralVisualizerTool::Audio
        PNG::PNG
        Qt6::Concurrent
        Qt6::Network
        Qt6::Widgets
    )
    if(APPLE)
        target_link_libraries(ProceduralVisualizerToolGui PRIVATE
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreMIDI"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework IOKit"
            "-framework VideoToolbox")
    endif()
    target_compile_definitions(ProceduralVisualizerToolGui PRIVATE
        PVT_PROGRAM_VERSION="${PVT_PRODUCT_VERSION}")
    pvt_enable_warnings(ProceduralVisualizerToolGui)

    if(PVT_ENABLE_DISTRIBUTION)
        if(NOT APPLE)
            message(FATAL_ERROR
                "PVT_ENABLE_DISTRIBUTION currently creates a macOS app bundle and requires APPLE.")
        endif()
        if(TARGET Qt6::macdeployqt)
            set(PVT_MACDEPLOYQT "$<TARGET_FILE:Qt6::macdeployqt>")
        else()
            find_program(PVT_MACDEPLOYQT NAMES macdeployqt
                HINTS "${Qt6_DIR}/../../../bin" REQUIRED)
        endif()
        find_program(PVT_VTOOL NAMES vtool REQUIRED)
        set(PVT_DISTRIBUTION_APP
            "${CMAKE_CURRENT_BINARY_DIR}/Procedural Visualizer Tool.app")
        add_custom_target(distribution
            COMMAND "${CMAKE_COMMAND}" -E remove_directory
                    "${PVT_DISTRIBUTION_APP}"
            # Remove only the obsolete output directory used by the previous
            # distribution layout, so an old package cannot be mistaken for
            # the current top-level result.
            COMMAND "${CMAKE_COMMAND}" -E remove_directory
                    "${CMAKE_CURRENT_BINARY_DIR}/dist"
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                    "$<TARGET_BUNDLE_DIR:ProceduralVisualizerToolGui>"
                    "${PVT_DISTRIBUTION_APP}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory
                    "${PVT_DISTRIBUTION_APP}/Contents/Resources/Licenses"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
                    "${PVT_DISTRIBUTION_APP}/Contents/Resources/Licenses/ProceduralVisualizerTool-GPL-3.0.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniaudio/LICENSE"
                    "${PVT_DISTRIBUTION_APP}/Contents/Resources/Licenses/miniaudio-LICENSE.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/btt/LICENSE"
                    "${PVT_DISTRIBUTION_APP}/Contents/Resources/Licenses/Beat-and-Tempo-Tracking-LICENSE.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${PVT_DISTRIBUTION_LIBPNG_LICENSE}"
                    "${PVT_DISTRIBUTION_APP}/Contents/Resources/Licenses/libpng-LICENSE.txt"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "$<TARGET_FILE:pvt_render_cli>"
                    "${PVT_DISTRIBUTION_APP}/Contents/MacOS/pvt-render"
            COMMAND /usr/bin/codesign --force
                    --sign "${PVT_DISTRIBUTION_CODE_SIGN_IDENTITY}"
                    "${PVT_DISTRIBUTION_APP}/Contents/MacOS/pvt-render"
            COMMAND "${PVT_MACDEPLOYQT}"
                    "${PVT_DISTRIBUTION_APP}"
                    -always-overwrite
                    "-codesign=${PVT_DISTRIBUTION_CODE_SIGN_IDENTITY}"
                    "$<$<CONFIG:Debug>:-use-debug-libs>"
            COMMAND "${CMAKE_COMMAND}"
                    "-DPVT_APP_BUNDLE=${PVT_DISTRIBUTION_APP}"
                    "-DPVT_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}"
                    "-DPVT_PRODUCT_VERSION=${PVT_PRODUCT_VERSION}"
                    "-DPVT_VTOOL=${PVT_VTOOL}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos/VerifyDistribution.cmake"
            DEPENDS ProceduralVisualizerToolGui pvt_render_cli
            COMMENT "Staging and verifying a self-contained macOS application"
            VERBATIM)
    endif()
endif()

if(BUILD_TESTING)
    add_executable(pvt_audio_analysis_tests tests/audio_analysis_test.cpp)
    target_compile_features(pvt_audio_analysis_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_audio_analysis_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_audio_analysis_tests PRIVATE
        ProceduralVisualizerTool::Audio)
    pvt_enable_warnings(pvt_audio_analysis_tests)
    add_test(NAME pvt_audio_analysis COMMAND pvt_audio_analysis_tests)

    if(APPLE)
        add_executable(pvt_video_export_tests
            tests/video_export_test.mm
            gui/macos_video_export.mm
            gui/video_export.h)
        target_compile_features(pvt_video_export_tests PRIVATE cxx_std_17)
        set_target_properties(pvt_video_export_tests PROPERTIES
            CXX_EXTENSIONS NO)
        set_source_files_properties(
            tests/video_export_test.mm gui/macos_video_export.mm PROPERTIES
            COMPILE_OPTIONS "-fobjc-arc")
        target_include_directories(pvt_video_export_tests PRIVATE gui)
        target_link_libraries(pvt_video_export_tests PRIVATE
            ProceduralVisualizerTool::ProceduralVisualizerTool
            PNG::PNG
            "-framework AVFoundation"
            "-framework AudioToolbox"
            "-framework CoreMedia"
            "-framework CoreVideo"
            "-framework Foundation"
            "-framework VideoToolbox")
        pvt_enable_warnings(pvt_video_export_tests)
        add_test(NAME pvt_video_export COMMAND pvt_video_export_tests)
    endif()

    add_executable(pvt_tests tests/test_main.cpp)
    target_compile_features(pvt_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_tests PRIVATE
        ProceduralVisualizerTool::ProceduralVisualizerTool
        PNG::PNG)
    pvt_enable_warnings(pvt_tests)
    add_test(NAME pvt_core COMMAND pvt_tests ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(pvt_project_composite_tests
        tests/project_composite_test.cpp)
    target_compile_features(pvt_project_composite_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_project_composite_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_project_composite_tests PRIVATE
        ProceduralVisualizerTool::ProceduralVisualizerTool)
    pvt_enable_warnings(pvt_project_composite_tests)
    add_test(NAME pvt_project_composite COMMAND pvt_project_composite_tests)

    add_executable(pvt_metal_backend_tests tests/metal_backend_test.cpp)
    target_compile_features(pvt_metal_backend_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_metal_backend_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_metal_backend_tests PRIVATE
        ProceduralVisualizerTool::ProceduralVisualizerTool)
    target_compile_definitions(pvt_metal_backend_tests PRIVATE
        PVT_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    pvt_enable_warnings(pvt_metal_backend_tests)
    add_test(NAME pvt_metal_backend COMMAND pvt_metal_backend_tests)

    if(PVT_OPENGL_SURFACE_BACKEND_ENABLED)
        add_executable(pvt_opengl_surface_backend_tests
            tests/opengl_surface_backend_test.cpp)
        target_compile_features(pvt_opengl_surface_backend_tests PRIVATE
            cxx_std_17)
        set_target_properties(pvt_opengl_surface_backend_tests PROPERTIES
            CXX_EXTENSIONS NO)
        target_link_libraries(pvt_opengl_surface_backend_tests PRIVATE
            ProceduralVisualizerTool::ProceduralVisualizerTool Qt6::Gui)
        pvt_enable_warnings(pvt_opengl_surface_backend_tests)
        add_test(NAME pvt_opengl_surface_backend
            COMMAND pvt_opengl_surface_backend_tests)
    endif()

    add_executable(pvt_bundle_tests tests/bundle_test.cpp)
    target_compile_features(pvt_bundle_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_bundle_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_bundle_tests PRIVATE
        ProceduralVisualizerTool::Bundle
        ${PVT_MINIZIP_TARGET})
    pvt_enable_warnings(pvt_bundle_tests)
    add_test(NAME pvt_bundle COMMAND pvt_bundle_tests)

    add_executable(pvt_palette_io_tests tests/palette_io_test.cpp)
    target_compile_features(pvt_palette_io_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_palette_io_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_palette_io_tests PRIVATE
        ProceduralVisualizerTool::Bundle)
    pvt_enable_warnings(pvt_palette_io_tests)
    add_test(NAME pvt_palette_io COMMAND pvt_palette_io_tests)

    add_executable(pvt_obj_mesh_tests
        tests/obj_mesh_isolated_test.cpp
        src/obj_mesh.cpp
        src/obj_mesh.h)
    target_compile_features(pvt_obj_mesh_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_obj_mesh_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_obj_mesh_tests PRIVATE Threads::Threads)
    pvt_enable_warnings(pvt_obj_mesh_tests)
    add_test(NAME pvt_obj_mesh
        COMMAND pvt_obj_mesh_tests ${CMAKE_CURRENT_SOURCE_DIR})

    # This test intentionally exercises the private OBJ surface mapper. Build
    # those implementation files into the isolated test instead of relying on
    # hidden symbols from the public shared library.
    add_executable(pvt_obj_surface_tests
        tests/obj_surface_isolated_test.cpp
        src/obj_mesh.cpp
        src/obj_mesh.h
        src/obj_surface.cpp
        src/obj_surface.h)
    target_compile_features(pvt_obj_surface_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_obj_surface_tests PROPERTIES CXX_EXTENSIONS NO)
    target_include_directories(pvt_obj_surface_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include)
    target_link_libraries(pvt_obj_surface_tests PRIVATE Threads::Threads)
    pvt_enable_warnings(pvt_obj_surface_tests)
    add_test(NAME pvt_obj_surface
        COMMAND pvt_obj_surface_tests ${CMAKE_CURRENT_SOURCE_DIR})

    if(PVT_BUILD_CLI)
        add_test(NAME pvt_cli_self_test COMMAND pvt_render_cli --self-test)
        add_test(NAME pvt_cli_help COMMAND pvt_render_cli --help)
        add_test(NAME pvt_cli_version COMMAND pvt_render_cli --version)
        set_tests_properties(pvt_cli_version PROPERTIES
            PASS_REGULAR_EXPRESSION
                "Procedural Visualizer Tool ${PVT_PRODUCT_VERSION}")
        add_test(NAME pvt_cli_rejects_unknown_option
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -DTEST_CASE=unknown-option
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_rejects_self_test_conflict
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -DTEST_CASE=self-test-conflict
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_rejects_bad_png_compression
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -DTEST_CASE=invalid-compression
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_rejects_bad_backend
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -DTEST_CASE=invalid-backend
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_rejects_bad_gpu_bound
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -DTEST_CASE=invalid-gpu-bound
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_eof_aborts_save
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -DTEST_CASE=eof-save
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_custom_obj_render
            COMMAND pvt_render_cli --render
                --width 32 --height 32 --block-size 2 --frames 2
                --obj ${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/obj/closed_cube.obj
                --png-compression 0
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/pvt-cli-custom-obj
                --prefix obj_ --overwrite)
        add_test(NAME pvt_cli_displacement_plane_render
            COMMAND pvt_render_cli --render
                --width 32 --height 32 --block-size 2 --frames 2
                --height-map ${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png
                --height-min -0.3 --height-max 0.45
                --height-midpoint 0.48 --height-pixels-per-node 3
                --png-compression 0
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/pvt-cli-displacement-plane
                --prefix height_ --overwrite)
        add_test(NAME pvt_cli_starting_image_render
            COMMAND pvt_render_cli --render
                --width 32 --height 32 --block-size 2 --frames 2
                --starting-image ${CMAKE_CURRENT_SOURCE_DIR}/icon/icon.png
                --image-fit contain --png-compression 0
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/pvt-cli-starting-image
                --prefix image_ --overwrite)
        add_test(NAME pvt_cli_project_bundle
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:pvt_render_cli>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCliBundle.cmake)
    endif()

    if(PVT_BUILD_QT_GUI)
        add_test(NAME pvt_gui_smoke
            COMMAND ProceduralVisualizerToolGui --smoke-test)
        if(APPLE)
            set(PVT_GUI_SMOKE_PLATFORM cocoa)
        else()
            set(PVT_GUI_SMOKE_PLATFORM offscreen)
        endif()
        set_tests_properties(pvt_gui_smoke PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=${PVT_GUI_SMOKE_PLATFORM}")
    endif()
endif()

install(TARGETS ProceduralVisualizerTool
    EXPORT ProceduralVisualizerToolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(PVT_BUILD_CLI)
    install(TARGETS pvt_render_cli
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(PVT_BUILD_QT_GUI)
    install(TARGETS ProceduralVisualizerToolGui
        BUNDLE DESTINATION .
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    if(UNIX AND NOT APPLE)
        install(FILES script/procedural-visualizer-tool.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
        install(FILES icon/procedural-visualizer-tool.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/64x64/apps)
        install(FILES icon/procedural-visualizer-tool_128.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps
            RENAME procedural-visualizer-tool.png)
        install(FILES
            script/application-x-procedural-visualizer-tool-project.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
    endif()
    if(PVT_DEPLOY_QT_RUNTIME)
        qt_generate_deploy_app_script(
            TARGET ProceduralVisualizerToolGui
            OUTPUT_SCRIPT pvt_qt_deploy_script)
        install(SCRIPT "${pvt_qt_deploy_script}")
        if(WIN32)
            if(NOT TARGET Qt6::QOffscreenIntegrationPlugin)
                message(FATAL_ERROR
                    "The Qt offscreen platform plugin is required for installed-package validation.")
            endif()
            install(FILES "$<TARGET_FILE:Qt6::QOffscreenIntegrationPlugin>"
                DESTINATION plugins/platforms)
        endif()
    endif()
endif()
install(FILES include/procedural_visualizer_tool.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md IMPLEMENTATION_STATUS.md PORTABILITY_ROADMAP.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
if(PVT_BUILD_CLI OR PVT_BUILD_QT_GUI)
    install(FILES third_party/miniaudio/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME MINIAUDIO-LICENSE)
    install(FILES third_party/btt/LICENSE
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME BEAT-AND-TEMPO-TRACKING-LICENSE)
endif()
if(PVT_METAL_BACKEND_ENABLED AND EXISTS "${PVT_METAL_CPP_DIR}/LICENSE")
    install(FILES "${PVT_METAL_CPP_DIR}/LICENSE"
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME METAL-CPP-LICENSE)
endif()
install(EXPORT ProceduralVisualizerToolTargets
    FILE ProceduralVisualizerToolTargets.cmake
    NAMESPACE ProceduralVisualizerTool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProceduralVisualizerTool)

set(PVT_CONFIG_QT_DEPENDENCY "")
if(PVT_OPENGL_SURFACE_BACKEND_ENABLED AND NOT BUILD_SHARED_LIBS)
    set(PVT_CONFIG_QT_DEPENDENCY
        "find_dependency(Qt6 6.5 COMPONENTS Gui)")
endif()
configure_package_config_file(
    cmake/ProceduralVisualizerToolConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ProceduralVisualizerToolConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProceduralVisualizerTool)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ProceduralVisualizerToolConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ProceduralVisualizerToolConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ProceduralVisualizerToolConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProceduralVisualizerTool)
