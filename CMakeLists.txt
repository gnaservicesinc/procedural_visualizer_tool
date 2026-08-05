cmake_minimum_required(VERSION 3.20)

project(ProceduralVisualizerTool VERSION 2.0.0 LANGUAGES CXX)

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
    target_link_libraries(render9 PRIVATE
        ProceduralVisualizerTool::ProceduralVisualizerTool)
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
        ProceduralVisualizerTool::ProceduralVisualizerTool
        Qt6::Concurrent
        Qt6::Widgets
    )
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
