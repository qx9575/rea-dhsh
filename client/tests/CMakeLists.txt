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
