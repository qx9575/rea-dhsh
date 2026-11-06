add_library(dhsh_resource
    formats/MapFormats.cpp
    resource/ByteReader.cpp
    resource/NzDecoder.cpp
    resource/PakArchive.cpp
    resource/PakResourceProvider.cpp
)

target_include_directories(dhsh_resource PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

add_library(dhsh_image
    animation/AnimationClip.cpp
    formats/A16.cpp
    formats/Apg.cpp
    formats/Cti.cpp
    formats/Pif.cpp
    platform/WicImage.cpp
)

target_link_libraries(dhsh_image PUBLIC dhsh_resource PRIVATE windowscodecs ole32)

add_library(dhsh_scene
    render/SoftwareRenderer.cpp
    resource/CachedImageProvider.cpp
    scene/CoverMask.cpp
    scene/MapScene.cpp
    scene/SceneObject.cpp
)

target_include_directories(dhsh_scene PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dhsh_scene PUBLIC dhsh_image)

add_library(dhsh_app
    app/App.cpp
    platform/D3D11Presenter.cpp
    platform/Win32Window.cpp
)

target_include_directories(dhsh_app PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(dhsh_app PUBLIC dhsh_scene PRIVATE d3d11 dxgi d3dcompiler)
target_compile_definitions(dhsh_app PRIVATE
    DHSH_DEFAULT_CACHE_DIR="${CMAKE_BINARY_DIR}/map-cache"
)

add_executable(dhsh_client
    main.cpp
)

target_link_libraries(dhsh_client PRIVATE dhsh_app)

set_target_properties(dhsh_client PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
