if(NOT DEFINED PROGRAM OR PROGRAM STREQUAL "")
    message(FATAL_ERROR "PROGRAM was not supplied")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef test_suffix)
set(test_root "${CMAKE_CURRENT_BINARY_DIR}/pvt-cli-bundle-${test_suffix}")
file(MAKE_DIRECTORY "${test_root}")

function(run_cli label)
    execute_process(
        COMMAND "${PROGRAM}" ${ARGN}
        WORKING_DIRECTORY "${test_root}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "${label} failed (${result}):\n${standard_output}${standard_error}")
    endif()
    set(last_output "${standard_output}${standard_error}" PARENT_SCOPE)
endfunction()

run_cli("first bundle save"
    --project-name CliFire
    --width 32 --height 32 --block-size 4 --frames 2
    --add-layer Sparks --blend add --layer-opacity 0.4
    --save-default)
if(NOT EXISTS "${test_root}/CliFire.zip")
    message(FATAL_ERROR "Default project save did not create CliFire.zip")
endif()

run_cli("bundle reload and clean save"
    --load CliFire.zip --save CliFire.zip)
if(NOT last_output MATCHES "No changes; verified the bundle state")
    message(FATAL_ERROR "Clean save did not report validation-only behavior:\n${last_output}")
endif()

run_cli("bundle version listing" --load CliFire.zip --list-versions)
if(NOT last_output MATCHES "2 layer\\(s\\)" OR NOT last_output MATCHES "\\* 0")
    message(FATAL_ERROR "Bundle listing did not retain the two-layer version:\n${last_output}")
endif()

run_cli("explicit legacy save"
    --project-name Original
    --width 32 --height 32 --block-size 4 --frames 2
    --save-legacy original.pvt)
if(NOT EXISTS "${test_root}/original.pvt")
    message(FATAL_ERROR "Explicit legacy export did not create original.pvt")
endif()
file(SHA256 "${test_root}/original.pvt" legacy_digest_before)

run_cli("scripted hierarchical audio routing"
    --project-audio-reactive --audio-reactive
    --save-legacy routing.pvt)
file(READ "${test_root}/routing.pvt" routing_setup)
string(ASCII 9 field_separator)
string(FIND "${routing_setup}"
    "audio_response_defaults.enabled${field_separator}1"
    project_audio_position)
string(FIND "${routing_setup}"
    "audio_reactive.override_enabled${field_separator}1"
    layer_override_position)
string(FIND "${routing_setup}"
    "audio_reactive.enabled${field_separator}1"
    layer_audio_position)
if(project_audio_position LESS 0
    OR layer_override_position LESS 0
    OR layer_audio_position LESS 0)
    message(FATAL_ERROR
        "Scripted audio switches did not persist project and layer routing")
endif()

run_cli("scripted audio inheritance"
    --audio-reactive --inherit-audio-reactive
    --save-legacy inherited-routing.pvt)
file(READ "${test_root}/inherited-routing.pvt" inherited_routing_setup)
string(FIND "${inherited_routing_setup}"
    "audio_reactive.override_enabled${field_separator}0"
    inherited_override_position)
if(inherited_override_position LESS 0)
    message(FATAL_ERROR
        "Scripted inherit switch did not disable the active-layer override")
endif()

run_cli("legacy import to bundle" --load original.pvt --save-default)
if(NOT EXISTS "${test_root}/original.zip")
    message(FATAL_ERROR "Legacy import did not default to a separate original.zip")
endif()
file(SHA256 "${test_root}/original.pvt" legacy_digest_after)
if(NOT legacy_digest_before STREQUAL legacy_digest_after)
    message(FATAL_ERROR "Normal save overwrote the imported legacy setup")
endif()

execute_process(
    COMMAND "${PROGRAM}"
        --add-layer Upper --save-legacy lossy.pvt
    WORKING_DIRECTORY "${test_root}"
    RESULT_VARIABLE lossy_result
    OUTPUT_VARIABLE lossy_output
    ERROR_VARIABLE lossy_error)
if(lossy_result EQUAL 0 OR EXISTS "${test_root}/lossy.pvt")
    message(FATAL_ERROR
        "Multi-layer legacy export was not rejected:\n${lossy_output}${lossy_error}")
endif()

execute_process(
    COMMAND "${PROGRAM}" --save-default --save-legacy both.pvt
    WORKING_DIRECTORY "${test_root}"
    RESULT_VARIABLE conflicting_save_result
    OUTPUT_VARIABLE conflicting_save_output
    ERROR_VARIABLE conflicting_save_error)
if(conflicting_save_result EQUAL 0
    OR EXISTS "${test_root}/both.pvt"
    OR EXISTS "${test_root}/Untitled Fire.zip")
    message(FATAL_ERROR
        "Conflicting normal/legacy saves were not rejected:\n"
        "${conflicting_save_output}${conflicting_save_error}")
endif()

run_cli("unpacked directory save"
    --project-name DirFire
    --width 32 --height 32 --block-size 4 --frames 2
    --save DirFire)
foreach(required_file
        metadata.txt metadata.sha256 current
        0/metadata.txt 0/render_output.txt 0/0.pvt)
    if(NOT EXISTS "${test_root}/DirFire/${required_file}")
        message(FATAL_ERROR "Unpacked bundle is missing ${required_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${test_root}")
