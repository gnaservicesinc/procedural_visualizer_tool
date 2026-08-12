if(NOT DEFINED PVT_ICON_SOURCE OR PVT_ICON_SOURCE STREQUAL ""
   OR NOT EXISTS "${PVT_ICON_SOURCE}")
    message(FATAL_ERROR "PVT_ICON_SOURCE must name the program icon PNG.")
endif()
if(NOT DEFINED PVT_ICON_OUTPUT OR PVT_ICON_OUTPUT STREQUAL "")
    message(FATAL_ERROR "PVT_ICON_OUTPUT must name the generated .icns file.")
endif()
if(NOT DEFINED PVT_SIPS OR NOT EXISTS "${PVT_SIPS}"
   OR NOT DEFINED PVT_ICONUTIL OR NOT EXISTS "${PVT_ICONUTIL}")
    message(FATAL_ERROR "sips and iconutil are required to build the macOS icon.")
endif()

get_filename_component(_pvt_icon_parent "${PVT_ICON_OUTPUT}" DIRECTORY)
set(_pvt_iconset "${_pvt_icon_parent}/ProceduralVisualizerTool.iconset")
file(REMOVE_RECURSE "${_pvt_iconset}")
file(MAKE_DIRECTORY "${_pvt_iconset}")

function(_pvt_icon_size logical scale pixels)
    if(scale EQUAL 1)
        set(_pvt_suffix "")
    else()
        set(_pvt_suffix "@${scale}x")
    endif()
    execute_process(
        COMMAND "${PVT_SIPS}" -z "${pixels}" "${pixels}"
                "${PVT_ICON_SOURCE}" --out
                "${_pvt_iconset}/icon_${logical}x${logical}${_pvt_suffix}.png"
        RESULT_VARIABLE _pvt_sips_result
        OUTPUT_QUIET ERROR_VARIABLE _pvt_sips_error)
    if(NOT _pvt_sips_result EQUAL 0)
        message(FATAL_ERROR "sips could not generate icon size ${pixels}: ${_pvt_sips_error}")
    endif()
endfunction()

_pvt_icon_size(16 1 16)
_pvt_icon_size(16 2 32)
_pvt_icon_size(32 1 32)
_pvt_icon_size(32 2 64)
_pvt_icon_size(128 1 128)
_pvt_icon_size(128 2 256)
_pvt_icon_size(256 1 256)
_pvt_icon_size(256 2 512)
_pvt_icon_size(512 1 512)
_pvt_icon_size(512 2 1024)

execute_process(
    COMMAND "${PVT_ICONUTIL}" -c icns "${_pvt_iconset}"
            -o "${PVT_ICON_OUTPUT}"
    RESULT_VARIABLE _pvt_iconutil_result
    ERROR_VARIABLE _pvt_iconutil_error)
if(NOT _pvt_iconutil_result EQUAL 0)
    message(FATAL_ERROR "iconutil could not generate the application icon: ${_pvt_iconutil_error}")
endif()
file(REMOVE_RECURSE "${_pvt_iconset}")
