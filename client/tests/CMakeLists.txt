add_executable(dhsh_test_smoke
    test_smoke.cpp
)

target_link_libraries(dhsh_test_smoke PRIVATE dhsh_app)

add_test(NAME smoke COMMAND dhsh_test_smoke)
