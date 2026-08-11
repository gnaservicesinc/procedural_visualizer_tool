if(NOT DEFINED PROGRAM OR PROGRAM STREQUAL "")
    message(FATAL_ERROR "PROGRAM was not supplied")
endif()

if(TEST_CASE STREQUAL "eof-save")
    set(working_directory "${CMAKE_CURRENT_BINARY_DIR}/pvt-cli-eof-save")
    file(REMOVE_RECURSE "${working_directory}")
    file(MAKE_DIRECTORY "${working_directory}")
    set(input_path "${working_directory}/input.txt")
    file(WRITE "${input_path}" "8\n")
    execute_process(
        COMMAND "${PROGRAM}"
        WORKING_DIRECTORY "${working_directory}"
        INPUT_FILE "${input_path}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR
            "The CLI did not exit cleanly after EOF at the save prompt:\n${standard_output}${standard_error}")
    endif()
    if(EXISTS "${working_directory}/setup.pvt")
        message(FATAL_ERROR "EOF at the save prompt unexpectedly wrote setup.pvt")
    endif()
    file(REMOVE_RECURSE "${working_directory}")
    return()
elseif(TEST_CASE STREQUAL "self-test-conflict")
    set(arguments --self-test --definitely-not-an-option)
    set(expected_message "must be used by itself")
elseif(TEST_CASE STREQUAL "invalid-compression")
    set(arguments --render --png-compression 10)
    set(expected_message "Invalid option or value")
elseif(TEST_CASE STREQUAL "invalid-backend")
    set(arguments --render --backend quantum)
    set(expected_message "must be cpu, cpu\\+gpu, or gpu")
elseif(TEST_CASE STREQUAL "invalid-gpu-bound")
    set(arguments --render --gpu-in-flight 9)
    set(expected_message "Invalid option or value")
else()
    set(arguments --definitely-not-an-option)
    set(expected_message "Unknown option")
endif()

execute_process(
    COMMAND "${PROGRAM}" ${arguments}
    RESULT_VARIABLE result
    OUTPUT_VARIABLE standard_output
    ERROR_VARIABLE standard_error)

if(result EQUAL 0)
    message(FATAL_ERROR "The CLI unexpectedly accepted an unknown option")
endif()

set(combined_output "${standard_output}${standard_error}")
if(NOT combined_output MATCHES "${expected_message}")
    message(FATAL_ERROR
        "The CLI failed without the expected diagnostic '${expected_message}':\n${combined_output}")
endif()
