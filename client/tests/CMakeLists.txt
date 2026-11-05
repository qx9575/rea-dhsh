add_executable(dhsh_test_smoke
    test_smoke.cpp
)

target_link_libraries(dhsh_test_smoke PRIVATE dhsh_app)

add_test(NAME smoke COMMAND dhsh_test_smoke)

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

add_executable(dhsh_test_map_formats
    test_map_formats.cpp
)

target_link_libraries(dhsh_test_map_formats PRIVATE dhsh_resource)

add_test(NAME map_formats COMMAND dhsh_test_map_formats)
