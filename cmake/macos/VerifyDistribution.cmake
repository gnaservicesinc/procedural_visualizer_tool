if(NOT DEFINED PVT_APP_BUNDLE OR PVT_APP_BUNDLE STREQUAL "")
    message(FATAL_ERROR "PVT_APP_BUNDLE must name the staged application bundle.")
endif()

set(_pvt_required_items
    "Contents/MacOS/Procedural Visualizer Tool"
    "Contents/MacOS/pvt-render"
    "Contents/Resources/ProceduralVisualizerTool.icns"
    "Contents/Frameworks/QtCore.framework/Versions/A/QtCore"
    "Contents/Frameworks/QtGui.framework/Versions/A/QtGui"
    "Contents/Frameworks/QtWidgets.framework/Versions/A/QtWidgets"
    "Contents/Frameworks/QtConcurrent.framework/Versions/A/QtConcurrent"
    "Contents/PlugIns/platforms/libqcocoa.dylib"
    "Contents/Resources/Licenses/ProceduralVisualizerTool-GPL-3.0.txt"
    "Contents/Resources/Licenses/miniaudio-LICENSE.txt"
    "Contents/Resources/Licenses/Beat-and-Tempo-Tracking-LICENSE.txt"
    "Contents/Resources/Licenses/libpng-LICENSE.txt"
)
foreach(_pvt_item IN LISTS _pvt_required_items)
    if(NOT EXISTS "${PVT_APP_BUNDLE}/${_pvt_item}")
        message(FATAL_ERROR
            "Distribution is incomplete: ${_pvt_item} is missing from ${PVT_APP_BUNDLE}.")
    endif()
endforeach()

if(NOT DEFINED PVT_PRODUCT_VERSION OR PVT_PRODUCT_VERSION STREQUAL "")
    message(FATAL_ERROR "PVT_PRODUCT_VERSION is required for CLI verification.")
endif()
execute_process(
    COMMAND "${PVT_APP_BUNDLE}/Contents/MacOS/pvt-render" --version
    RESULT_VARIABLE _pvt_cli_result
    OUTPUT_VARIABLE _pvt_cli_output
    ERROR_VARIABLE _pvt_cli_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _pvt_cli_result EQUAL 0
   OR NOT _pvt_cli_output STREQUAL
      "Procedural Visualizer Tool ${PVT_PRODUCT_VERSION}")
    message(FATAL_ERROR
        "Embedded pvt-render failed version verification: ${_pvt_cli_output}${_pvt_cli_error}")
endif()

file(GLOB_RECURSE _pvt_bundle_files LIST_DIRECTORIES FALSE
    "${PVT_APP_BUNDLE}/Contents/*")
if(NOT DEFINED PVT_DEPLOYMENT_TARGET
   OR PVT_DEPLOYMENT_TARGET STREQUAL ""
   OR NOT DEFINED PVT_VTOOL OR NOT EXISTS "${PVT_VTOOL}")
    message(FATAL_ERROR
        "Distribution deployment-target verification is not configured.")
endif()
set(_pvt_macho_count 0)
foreach(_pvt_file IN LISTS _pvt_bundle_files)
    execute_process(
        COMMAND /usr/bin/file -b "${_pvt_file}"
        RESULT_VARIABLE _pvt_file_result
        OUTPUT_VARIABLE _pvt_file_type
        ERROR_VARIABLE _pvt_file_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _pvt_file_result EQUAL 0)
        message(FATAL_ERROR
            "Could not identify distribution file ${_pvt_file}: ${_pvt_file_error}")
    endif()
    if(NOT _pvt_file_type MATCHES "Mach-O")
        continue()
    endif()

    math(EXPR _pvt_macho_count "${_pvt_macho_count} + 1")
    execute_process(
        COMMAND otool -L "${_pvt_file}"
        RESULT_VARIABLE _pvt_otool_result
        OUTPUT_VARIABLE _pvt_dependencies
        ERROR_VARIABLE _pvt_otool_error)
    if(NOT _pvt_otool_result EQUAL 0)
        message(FATAL_ERROR
            "Could not inspect distribution dependencies for ${_pvt_file}: ${_pvt_otool_error}")
    endif()
    # Universal Mach-O output repeats the inspected file's absolute path as an
    # unindented architecture header. Actual dependency entries are indented,
    # so only those entries should participate in the machine-path check.
    if(_pvt_dependencies MATCHES "[\t ](/opt/|/usr/local/|/Users/)")
        message(FATAL_ERROR
            "Distribution binary still references a build-machine library:\n"
            "${_pvt_file}\n${_pvt_dependencies}")
    endif()
    execute_process(
        COMMAND "${PVT_VTOOL}" -show-build "${_pvt_file}"
        RESULT_VARIABLE _pvt_vtool_result
        OUTPUT_VARIABLE _pvt_build_versions
        ERROR_VARIABLE _pvt_vtool_error)
    if(NOT _pvt_vtool_result EQUAL 0)
        message(FATAL_ERROR
            "Could not inspect deployment targets for ${_pvt_file}: ${_pvt_vtool_error}")
    endif()
    string(REGEX MATCHALL "minos[ \t]+[0-9]+(\\.[0-9]+)*"
        _pvt_minimum_matches "${_pvt_build_versions}")
    if(NOT _pvt_minimum_matches)
        message(FATAL_ERROR
            "Distribution binary has no inspectable macOS deployment target: ${_pvt_file}")
    endif()
    foreach(_pvt_minimum_match IN LISTS _pvt_minimum_matches)
        string(REGEX REPLACE "^minos[ \t]+" ""
            _pvt_minimum "${_pvt_minimum_match}")
        if(PVT_DEPLOYMENT_TARGET VERSION_LESS _pvt_minimum)
            message(FATAL_ERROR
                "Distribution targets macOS ${PVT_DEPLOYMENT_TARGET}, but ${_pvt_file} requires macOS ${_pvt_minimum}.")
        endif()
    endforeach()
endforeach()
if(_pvt_macho_count EQUAL 0)
    message(FATAL_ERROR "Distribution contains no Mach-O binaries.")
endif()

execute_process(
    COMMAND codesign --verify --deep --strict "${PVT_APP_BUNDLE}"
    RESULT_VARIABLE _pvt_codesign_result
    ERROR_VARIABLE _pvt_codesign_error)
if(NOT _pvt_codesign_result EQUAL 0)
    message(FATAL_ERROR "The staged application signature is invalid: ${_pvt_codesign_error}")
endif()

message(STATUS
    "Verified self-contained macOS distribution (${_pvt_macho_count} Mach-O files): ${PVT_APP_BUNDLE}")
