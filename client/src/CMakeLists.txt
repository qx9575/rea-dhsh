add_library(dhsh_resource
    resource/ByteReader.cpp
    resource/NzDecoder.cpp
    resource/PakArchive.cpp
    resource/PakResourceProvider.cpp
)

target_include_directories(dhsh_resource PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(dhsh_app
    app/App.cpp
)

target_include_directories(dhsh_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(dhsh_client
    main.cpp
)

target_link_libraries(dhsh_client PRIVATE dhsh_app)

set_target_properties(dhsh_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
