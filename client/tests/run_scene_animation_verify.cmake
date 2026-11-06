if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED SHDATA OR NOT DEFINED CACHE
    OR NOT DEFINED PYTHON OR NOT DEFINED VERIFY_SCRIPT OR NOT DEFINED PREFIX)
    message(FATAL_ERROR "run_scene_animation_verify.cmake is missing required inputs")
endif()

file(REMOVE "${PREFIX}.meta" "${PREFIX}.before.rgba" "${PREFIX}.after.rgba")
execute_process(
    COMMAND "${TEST_EXECUTABLE}" --verify-ying-tian-fu-animation
        "${SHDATA}" "${CACHE}" "${PREFIX}"
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
    message(FATAL_ERROR "native MAN animation export failed: ${render_result}")
endif()

execute_process(
    COMMAND "${PYTHON}" "${VERIFY_SCRIPT}"
        --shdata "${SHDATA}" --prefix "${PREFIX}"
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
    message(FATAL_ERROR "Python MAN animation verification failed: ${verify_result}")
endif()
