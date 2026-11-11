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

add_executable(dhsh_test_navigation
    test_navigation.cpp
)

target_link_libraries(dhsh_test_navigation PRIVATE dhsh_app)

add_test(NAME navigation COMMAND dhsh_test_navigation)

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

add_executable(dhsh_test_entities
    test_entities.cpp
)

target_link_libraries(dhsh_test_entities PRIVATE dhsh_scene)

add_test(NAME entities COMMAND dhsh_test_entities)

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

if(DEFINED ENV{DHSH_SHDATA} AND EXISTS "$ENV{DHSH_SHDATA}/player.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/npc.pak" AND EXISTS "$ENV{DHSH_SHDATA}/perinfo.pak"
    AND DEFINED ENV{DHSH_MAP_CACHE} AND EXISTS "$ENV{DHSH_MAP_CACHE}")
    add_test(NAME entities_real_resources
        COMMAND dhsh_test_entities --real "$ENV{DHSH_SHDATA}" "$ENV{DHSH_MAP_CACHE}"
    )
else()
    message(STATUS
        "Real entity resources not registered: set DHSH_SHDATA and DHSH_MAP_CACHE")
endif()

add_executable(dhsh_test_audio_selection
    test_audio_selection.cpp
)

target_link_libraries(dhsh_test_audio_selection PRIVATE dhsh_audio)

add_test(NAME audio_selection COMMAND dhsh_test_audio_selection)

if(DEFINED ENV{DHSH_SHDATA}
    AND EXISTS "$ENV{DHSH_SHDATA}/config.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/map.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/sound/19007.mp3")
    add_test(NAME audio_selection_real
        COMMAND dhsh_test_audio_selection --real "$ENV{DHSH_SHDATA}")
else()
    message(STATUS
        "Real audio selection test not registered: set DHSH_SHDATA to a directory containing config.pak, map.pak, and sound/19007.mp3")
endif()

add_executable(dhsh_test_scp_ui
    test_scp_ui.cpp
)

target_link_libraries(dhsh_test_scp_ui PRIVATE dhsh_app dhsh_ui)
target_compile_options(dhsh_test_scp_ui PRIVATE /utf-8)

add_test(NAME scp_ui COMMAND dhsh_test_scp_ui)

add_executable(dhsh_test_lua_host
    test_lua_host.cpp
)

target_link_libraries(dhsh_test_lua_host PRIVATE dhsh_app dhsh_ui)
target_compile_options(dhsh_test_lua_host PRIVATE /utf-8)

add_test(NAME lua_host COMMAND dhsh_test_lua_host)

add_executable(dhsh_test_startup_report
    test_startup_report.cpp
)

target_link_libraries(dhsh_test_startup_report PRIVATE dhsh_app dhsh_diagnostics)

add_test(NAME startup_report COMMAND dhsh_test_startup_report)

if(DEFINED ENV{DHSH_SHDATA} AND EXISTS "$ENV{DHSH_SHDATA}/config.pak")
    add_test(NAME lua_host_real
        COMMAND dhsh_test_lua_host --real "$ENV{DHSH_SHDATA}")
else()
    message(STATUS
        "Real Lua callback test not registered: set DHSH_SHDATA to a directory containing config.pak")
endif()

add_executable(dhsh_test_m1_acceptance
    test_m1_acceptance.cpp
)

target_link_libraries(dhsh_test_m1_acceptance PRIVATE dhsh_app dhsh_ui)
target_compile_options(dhsh_test_m1_acceptance PRIVATE /utf-8)

if(DEFINED ENV{DHSH_SHDATA} AND DEFINED ENV{DHSH_MAP_CACHE}
    AND EXISTS "$ENV{DHSH_SHDATA}/config.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/map.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/player.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/npc.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/perinfo.pak"
    AND EXISTS "$ENV{DHSH_MAP_CACHE}")
    add_test(NAME m1_acceptance
        COMMAND dhsh_test_m1_acceptance --real "$ENV{DHSH_SHDATA}" "$ENV{DHSH_MAP_CACHE}")
else()
    message(STATUS "Real M1 acceptance will SKIP: set DHSH_SHDATA and DHSH_MAP_CACHE with config/map/player/npc/perinfo resources")
    add_test(NAME m1_acceptance COMMAND dhsh_test_m1_acceptance)
endif()
set_tests_properties(m1_acceptance PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)

set(DHSH_M1_APP_VERIFY_SCRIPT
    "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/client_m1_acceptance.py")
get_filename_component(DHSH_M1_APP_VERIFY_SCRIPT
    "${DHSH_M1_APP_VERIFY_SCRIPT}" ABSOLUTE)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(DEFINED ENV{DHSH_SHDATA} AND DEFINED ENV{DHSH_MAP_CACHE}
    AND EXISTS "$ENV{DHSH_SHDATA}/800config.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/config.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/ui.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/map.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/player.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/npc.pak"
    AND EXISTS "$ENV{DHSH_SHDATA}/perinfo.pak"
    AND EXISTS "$ENV{DHSH_MAP_CACHE}"
    AND Python3_Interpreter_FOUND AND EXISTS "${DHSH_M1_APP_VERIFY_SCRIPT}")
    add_test(NAME m1_app_acceptance
        COMMAND "${Python3_EXECUTABLE}" "${DHSH_M1_APP_VERIFY_SCRIPT}"
            --client "$<TARGET_FILE:dhsh_client>"
            --shdata "$ENV{DHSH_SHDATA}" --cache "$ENV{DHSH_MAP_CACHE}"
            --log "${CMAKE_CURRENT_BINARY_DIR}/m1-app-$<CONFIG>.log"
            --capture "${CMAKE_CURRENT_BINARY_DIR}/m1-app-$<CONFIG>.ppm"
            --result "${CMAKE_CURRENT_BINARY_DIR}/m1-app-$<CONFIG>.json"
            --configuration "$<CONFIG>")
    set_tests_properties(m1_app_acceptance PROPERTIES
        TIMEOUT 180 SKIP_RETURN_CODE 77)
else()
    message(STATUS "Real App M1 acceptance not registered: set DHSH_SHDATA/DHSH_MAP_CACHE and provide all client PAK resources plus Python3")
endif()

if(Python3_Interpreter_FOUND)
    add_test(NAME package_client
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/test_package_client.py")
endif()
