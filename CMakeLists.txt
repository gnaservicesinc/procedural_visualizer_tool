cmake_minimum_required(VERSION 3.20)

project(ProceduralVisualizerTool VERSION 4.0.1 LANGUAGES CXX)

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

option(PVT_BUILD_CLI "Build the command-line renderer" ON)
option(PVT_BUILD_QT_GUI "Build the Qt 6 interactive GUI" OFF)
option(PVT_BUILD_EXAMPLES "Build the C++ library API example" OFF)

function(pvt_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endfunction()

find_package(PNG REQUIRED)
find_package(Threads REQUIRED)

add_library(ProceduralVisualizerTool
    src/composite.cpp
    src/config_io.cpp
    src/core.cpp
    src/image_io.cpp
    src/obj_mesh.cpp
    src/obj_mesh.h
    src/obj_surface.cpp
    src/obj_surface.h
    src/path_utf8.h
    src/windows_file_install.h
)
add_library(ProceduralVisualizerTool::ProceduralVisualizerTool
    ALIAS ProceduralVisualizerTool)

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
    find_package(minizip 4.2 CONFIG QUIET)
    if(NOT TARGET MINIZIP::minizip)
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
        src/project_bundle.cpp
        src/project_bundle.h)
    add_library(ProceduralVisualizerTool::Bundle ALIAS ProceduralVisualizerBundle)
    target_compile_features(ProceduralVisualizerBundle PUBLIC cxx_std_17)
    set_target_properties(ProceduralVisualizerBundle PROPERTIES
        CXX_EXTENSIONS NO
        POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(ProceduralVisualizerBundle PRIVATE
        PVT_PROGRAM_VERSION="${PROJECT_VERSION}")
    target_include_directories(ProceduralVisualizerBundle PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)
    target_link_libraries(ProceduralVisualizerBundle
        PUBLIC ProceduralVisualizerTool::ProceduralVisualizerTool
        PRIVATE MINIZIP::minizip)
    pvt_enable_warnings(ProceduralVisualizerBundle)
endif()

if(PVT_BUILD_CLI)
    add_executable(render9 app/cli_main.cpp)
    target_compile_features(render9 PRIVATE cxx_std_17)
    set_target_properties(render9 PROPERTIES CXX_EXTENSIONS NO)
    if(APPLE OR UNIX)
        file(RELATIVE_PATH pvt_cli_install_library_path
            "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_FULL_LIBDIR}")
    endif()
    if(APPLE)
        set_target_properties(render9 PROPERTIES
            INSTALL_RPATH "@executable_path/${pvt_cli_install_library_path}")
    elseif(UNIX)
        set_target_properties(render9 PROPERTIES
            INSTALL_RPATH "\$ORIGIN/${pvt_cli_install_library_path}")
    endif()
    target_link_libraries(render9 PRIVATE ProceduralVisualizerTool::Bundle)
    target_compile_definitions(render9 PRIVATE
        PVT_PROGRAM_VERSION="${PROJECT_VERSION}")
    pvt_enable_warnings(render9)
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
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Concurrent)
    qt_standard_project_setup()
    qt_add_executable(ProceduralVisualizerToolGui MACOSX_BUNDLE
        gui/main.cpp
        gui/main_window.cpp
        gui/main_window.h
        gui/preview_widget.cpp
        gui/preview_widget.h
    )
    set_target_properties(ProceduralVisualizerToolGui PROPERTIES CXX_EXTENSIONS NO)
    if(APPLE)
        set_target_properties(ProceduralVisualizerToolGui PROPERTIES
            OUTPUT_NAME "Procedural Visualizer Tool"
            MACOSX_BUNDLE_BUNDLE_NAME "Procedural Visualizer Tool"
            MACOSX_BUNDLE_GUI_IDENTIFIER "com.gnaservices.proceduralvisualizertool"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")
    else()
        set_target_properties(ProceduralVisualizerToolGui PROPERTIES
            OUTPUT_NAME "procedural-visualizer-tool")
    endif()
    target_link_libraries(ProceduralVisualizerToolGui PRIVATE
        ProceduralVisualizerTool::Bundle
        Qt6::Concurrent
        Qt6::Widgets
    )
    target_compile_definitions(ProceduralVisualizerToolGui PRIVATE
        PVT_PROGRAM_VERSION="${PROJECT_VERSION}")
    pvt_enable_warnings(ProceduralVisualizerToolGui)
endif()

if(BUILD_TESTING)
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

    add_executable(pvt_bundle_tests tests/bundle_test.cpp)
    target_compile_features(pvt_bundle_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_bundle_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_bundle_tests PRIVATE
        ProceduralVisualizerTool::Bundle
        MINIZIP::minizip)
    pvt_enable_warnings(pvt_bundle_tests)
    add_test(NAME pvt_bundle COMMAND pvt_bundle_tests)

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

    add_executable(pvt_obj_surface_tests tests/obj_surface_isolated_test.cpp)
    target_compile_features(pvt_obj_surface_tests PRIVATE cxx_std_17)
    set_target_properties(pvt_obj_surface_tests PROPERTIES CXX_EXTENSIONS NO)
    target_link_libraries(pvt_obj_surface_tests PRIVATE
        ProceduralVisualizerTool::ProceduralVisualizerTool)
    pvt_enable_warnings(pvt_obj_surface_tests)
    add_test(NAME pvt_obj_surface
        COMMAND pvt_obj_surface_tests ${CMAKE_CURRENT_SOURCE_DIR})

    if(PVT_BUILD_CLI)
        add_test(NAME pvt_cli_self_test COMMAND render9 --self-test)
        add_test(NAME pvt_cli_help COMMAND render9 --help)
        add_test(NAME pvt_cli_rejects_unknown_option
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:render9>
                -DTEST_CASE=unknown-option
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_rejects_self_test_conflict
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:render9>
                -DTEST_CASE=self-test-conflict
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_rejects_bad_png_compression
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:render9>
                -DTEST_CASE=invalid-compression
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_eof_aborts_save
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:render9>
                -DTEST_CASE=eof-save
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ExpectCliFailure.cmake)
        add_test(NAME pvt_cli_custom_obj_render
            COMMAND render9 --render
                --width 32 --height 32 --block-size 2 --frames 2
                --obj ${CMAKE_CURRENT_SOURCE_DIR}/tests/assets/obj/closed_cube.obj
                --png-compression 0
                --output-dir ${CMAKE_CURRENT_BINARY_DIR}/pvt-cli-custom-obj
                --prefix obj_ --overwrite)
        add_test(NAME pvt_cli_project_bundle
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:render9>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/TestCliBundle.cmake)
    endif()

    if(PVT_BUILD_QT_GUI)
        add_test(NAME pvt_gui_smoke
            COMMAND ProceduralVisualizerToolGui --smoke-test)
        set_tests_properties(pvt_gui_smoke PROPERTIES
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

install(TARGETS ProceduralVisualizerTool
    EXPORT ProceduralVisualizerToolTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(PVT_BUILD_CLI)
    install(TARGETS render9
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(FILES include/procedural_visualizer_tool.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md IMPLEMENTATION_STATUS.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(EXPORT ProceduralVisualizerToolTargets
    FILE ProceduralVisualizerToolTargets.cmake
    NAMESPACE ProceduralVisualizerTool::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ProceduralVisualizerTool)

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
