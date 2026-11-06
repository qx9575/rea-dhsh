# Run dhsh_test_scene and then the direct-PAK pixel verifier as one CTest.
# All paths are passed by tests/CMakeLists.txt so the script does not depend
# on CMAKE_CURRENT_SOURCE_DIR (unset in cmake -P mode).

if(NOT DEFINED TEST_EXECUTABLE
    OR NOT DEFINED SHDATA
    OR NOT DEFINED CACHE
    OR NOT DEFINED PYTHON
    OR NOT DEFINED VERIFY_SCRIPT
    OR NOT DEFINED PPM
    OR NOT DEFINED REFERENCE
    OR NOT DEFINED DIFF)
    message(FATAL_ERROR
        "run_scene_verify.cmake requires TEST_EXECUTABLE, SHDATA, CACHE, "
        "PYTHON, VERIFY_SCRIPT, PPM, REFERENCE and DIFF")
endif()

get_filename_component(REFERENCE_DIR "${REFERENCE}" DIRECTORY)
file(MAKE_DIRECTORY "${REFERENCE_DIR}")
file(REMOVE "${PPM}" "${REFERENCE}" "${DIFF}")

message(STATUS "render: ${TEST_EXECUTABLE} --render-ying-tian-fu "
    "${SHDATA} ${CACHE} ${PPM}")
execute_process(
    COMMAND "${TEST_EXECUTABLE}" --render-ying-tian-fu
        "${SHDATA}" "${CACHE}" "${PPM}"
    RESULT_VARIABLE render_result
    OUTPUT_VARIABLE render_stdout
    ERROR_VARIABLE render_stderr)
if(NOT render_stdout STREQUAL "")
    message(STATUS "${render_stdout}")
endif()
if(NOT render_stderr STREQUAL "")
    message(STATUS "${render_stderr}")
endif()
if(NOT render_result STREQUAL "0")
    message(FATAL_ERROR
        "dhsh_test_scene failed with exit code ${render_result}")
endif()
if(NOT EXISTS "${PPM}")
    message(FATAL_ERROR "dhsh_test_scene did not write ${PPM}")
endif()

message(STATUS "verify: ${PYTHON} ${VERIFY_SCRIPT} --shdata ${SHDATA} "
    "--cache ${CACHE} --actual ${PPM} --reference ${REFERENCE} --diff ${DIFF}")
execute_process(
    COMMAND "${PYTHON}" "${VERIFY_SCRIPT}"
        --shdata "${SHDATA}"
        --cache "${CACHE}"
        --actual "${PPM}"
        --reference "${REFERENCE}"
        --diff "${DIFF}"
        --map YingTianFu
        --width 800
        --height 600
        --camera-x 0
        --camera-y 0
    RESULT_VARIABLE verify_result
    OUTPUT_VARIABLE verify_stdout
    ERROR_VARIABLE verify_stderr)
if(NOT verify_stdout STREQUAL "")
    message(STATUS "${verify_stdout}")
endif()
if(NOT verify_stderr STREQUAL "")
    message(STATUS "${verify_stderr}")
endif()
if(NOT verify_result STREQUAL "0")
    message(FATAL_ERROR
        "client_scene_verify.py reported a mismatch or error "
        "(exit ${verify_result})")
endif()
