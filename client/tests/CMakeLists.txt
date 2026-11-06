add_executable(dhsh_test_smoke
    test_smoke.cpp
)

target_link_libraries(dhsh_test_smoke PRIVATE dhsh_app)

add_test(NAME smoke COMMAND dhsh_test_smoke)

add_executable(dhsh_test_viewport
    test_viewport.cpp
)

target_link_libraries(dhsh_test_viewport PRIVATE dhsh_app)

add_test(NAME viewport COMMAND dhsh_test_viewport)

add_executable(dhsh_test_presenter
    test_presenter.cpp
)

target_link_libraries(dhsh_test_presenter PRIVATE dhsh_app)

add_test(NAME presenter COMMAND dhsh_test_presenter)

add_executable(dhsh_test_pak
    test_pak.cpp
)

target_link_libraries(dhsh_test_pak PRIVATE dhsh_resource)

add_test(NAME pak COMMAND dhsh_test_pak)

add_executable(dhsh_test_resource_provider
    test_resource_provider.cpp
)

target_link_libraries(dhsh_test_resource_provider PRIVATE dhsh_resource)

add_test(NAME resource_provider COMMAND dhsh_test_resource_provider)

add_executable(dhsh_test_nz
    test_nz.cpp
)

target_link_libraries(dhsh_test_nz PRIVATE dhsh_resource)

add_test(NAME nz COMMAND dhsh_test_nz)

add_executable(dhsh_test_a16
    test_a16.cpp
)

target_link_libraries(dhsh_test_a16 PRIVATE dhsh_image)

add_test(NAME a16 COMMAND dhsh_test_a16)

add_executable(dhsh_test_animation
    test_animation.cpp
)

target_link_libraries(dhsh_test_animation PRIVATE dhsh_image)

add_test(NAME animation COMMAND dhsh_test_animation)

add_executable(dhsh_test_avatar
    test_avatar.cpp
)

target_link_libraries(dhsh_test_avatar PRIVATE dhsh_image)

add_test(NAME avatar COMMAND dhsh_test_avatar)

set(DHSH_AVATAR_VERIFY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/client_avatar_verify.py")
get_filename_component(DHSH_AVATAR_VERIFY_SCRIPT
    "${DHSH_AVATAR_VERIFY_SCRIPT}" ABSOLUTE)
if(DEFINED ENV{DHSH_SHDATA} AND EXISTS "$ENV{DHSH_SHDATA}/perinfo.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/player.pak")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS "Avatar real-resource comparison not registered: Python3 interpreter not found")
    elseif(NOT EXISTS "${DHSH_AVATAR_VERIFY_SCRIPT}")
        message(STATUS "Avatar real-resource comparison not registered: missing verifier")
    else()
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c "import PIL"
            RESULT_VARIABLE DHSH_AVATAR_PYTHON_DEPS_RESULT
            OUTPUT_QUIET ERROR_QUIET TIMEOUT 10)
        if(NOT DHSH_AVATAR_PYTHON_DEPS_RESULT STREQUAL "0")
            message(STATUS "Avatar real-resource comparison not registered: Python3 is missing Pillow")
        else()
            add_test(NAME avatar_real_resources
                COMMAND "${Python3_EXECUTABLE}" "${DHSH_AVATAR_VERIFY_SCRIPT}"
                    --native "$<TARGET_FILE:dhsh_test_avatar>"
                    --shdata "$ENV{DHSH_SHDATA}")
        endif()
    endif()
else()
    message(STATUS "Avatar real-resource comparison not registered: set DHSH_SHDATA to a directory containing perinfo.pak and player.pak")
endif()

add_executable(dhsh_test_map_formats
    test_map_formats.cpp
)

target_link_libraries(dhsh_test_map_formats PRIVATE dhsh_resource)

add_test(NAME map_formats COMMAND dhsh_test_map_formats)

add_executable(dhsh_test_scene
    test_scene.cpp
)

target_link_libraries(dhsh_test_scene PRIVATE dhsh_scene)

add_test(NAME scene COMMAND dhsh_test_scene)

add_executable(dhsh_test_scene_layers
    test_scene_layers.cpp
)

target_link_libraries(dhsh_test_scene_layers PRIVATE dhsh_scene)

add_test(NAME scene_layers COMMAND dhsh_test_scene_layers)

set(DHSH_SCENE_VERIFY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/client_scene_verify.py")
get_filename_component(DHSH_SCENE_VERIFY_SCRIPT
    "${DHSH_SCENE_VERIFY_SCRIPT}" ABSOLUTE)
set(DHSH_SCENE_ANIMATION_VERIFY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/client_scene_animation_verify.py")
get_filename_component(DHSH_SCENE_ANIMATION_VERIFY_SCRIPT
    "${DHSH_SCENE_ANIMATION_VERIFY_SCRIPT}" ABSOLUTE)

if(DEFINED ENV{DHSH_SHDATA} AND EXISTS "$ENV{DHSH_SHDATA}/config.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/map.pak"
    AND DEFINED ENV{DHSH_MAP_CACHE} AND EXISTS "$ENV{DHSH_MAP_CACHE}")
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        message(STATUS
            "YingTianFu scene tests not registered: Python3 interpreter not found")
    elseif(NOT EXISTS "${DHSH_SCENE_VERIFY_SCRIPT}"
        OR NOT EXISTS "${DHSH_SCENE_ANIMATION_VERIFY_SCRIPT}")
        message(STATUS "YingTianFu scene tests not registered: missing verifier")
    else()
        execute_process(
            COMMAND "${Python3_EXECUTABLE}" -c "import numpy, PIL"
            RESULT_VARIABLE DHSH_PYTHON_DEPS_RESULT
            OUTPUT_QUIET
            ERROR_QUIET
            TIMEOUT 10)
        if(NOT DHSH_PYTHON_DEPS_RESULT STREQUAL "0")
            message(STATUS
                "YingTianFu scene tests not registered: Python3 is missing numpy or Pillow")
        else()
            set(DHSH_SCENE_REFERENCE_DIR
                "${CMAKE_CURRENT_BINARY_DIR}/scene-reference")
            add_test(NAME scene_ying_tian_fu_static
                COMMAND "${CMAKE_COMMAND}"
                    "-DTEST_EXECUTABLE=$<TARGET_FILE:dhsh_test_scene>"
                    "-DSHDATA=$ENV{DHSH_SHDATA}"
                    "-DCACHE=$ENV{DHSH_MAP_CACHE}"
                    "-DPYTHON=${Python3_EXECUTABLE}"
                    "-DVERIFY_SCRIPT=${DHSH_SCENE_VERIFY_SCRIPT}"
                    "-DPPM=${CMAKE_CURRENT_BINARY_DIR}/ying_tian_fu_800x600.ppm"
                    "-DREFERENCE=${DHSH_SCENE_REFERENCE_DIR}/ying_tian_fu_800x600.png"
                    "-DDIFF=${DHSH_SCENE_REFERENCE_DIR}/ying_tian_fu_800x600_diff.png"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_scene_verify.cmake")
            add_test(NAME scene_ying_tian_fu_animation
                COMMAND "${CMAKE_COMMAND}"
                    "-DTEST_EXECUTABLE=$<TARGET_FILE:dhsh_test_scene>"
                    "-DSHDATA=$ENV{DHSH_SHDATA}"
                    "-DCACHE=$ENV{DHSH_MAP_CACHE}"
                    "-DPYTHON=${Python3_EXECUTABLE}"
                    "-DVERIFY_SCRIPT=${DHSH_SCENE_ANIMATION_VERIFY_SCRIPT}"
                    "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/ying_tian_fu_animation"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/run_scene_animation_verify.cmake")
            message(STATUS "YingTianFu static and animated scene tests registered")
            message(STATUS "  Python3: ${Python3_EXECUTABLE}")
            message(STATUS "  shdata:  $ENV{DHSH_SHDATA}")
            message(STATUS "  cache:   $ENV{DHSH_MAP_CACHE}")
        endif()
    endif()
else()
    message(STATUS
        "YingTianFu scene tests not registered: set DHSH_SHDATA and DHSH_MAP_CACHE")
endif()

if(DEFINED ENV{DHSH_SHDATA} AND EXISTS "$ENV{DHSH_SHDATA}/map.pak")
    add_test(NAME map_formats_ying_tian_fu
        COMMAND dhsh_test_map_formats "$ENV{DHSH_SHDATA}"
    )
else()
    message(STATUS "YingTianFu real-resource test not registered: set DHSH_SHDATA to a directory containing map.pak")
endif()
