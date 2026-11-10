cmake_minimum_required(VERSION 3.26)

project(dhsh_client LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(DHSH_LUA_ROOT "" CACHE PATH "Installed or source-tree Lua 5.1.4 root")
set(DHSH_LUA514_SOURCE_DIR "")

if(DHSH_LUA_ROOT)
    if(EXISTS "${DHSH_LUA_ROOT}/src/lapi.c")
        set(DHSH_LUA514_HEADER "${DHSH_LUA_ROOT}/src/lua.h")
        set(DHSH_LUA514_SOURCE_DIR "${DHSH_LUA_ROOT}")
    else()
        find_path(DHSH_LUA514_INCLUDE_DIR lua.h
            PATHS "${DHSH_LUA_ROOT}/include" "${DHSH_LUA_ROOT}"
            NO_DEFAULT_PATH REQUIRED)
        find_library(DHSH_LUA514_LIBRARY NAMES lua5.1.4 lua51 lua
            PATHS "${DHSH_LUA_ROOT}/lib" "${DHSH_LUA_ROOT}"
            NO_DEFAULT_PATH REQUIRED)
        set(DHSH_LUA514_HEADER "${DHSH_LUA514_INCLUDE_DIR}/lua.h")
    endif()
    file(STRINGS "${DHSH_LUA514_HEADER}" DHSH_LUA514_RELEASE
        REGEX "^#define[ \t]+LUA_RELEASE[ \t]+\"Lua 5\\.1\\.4\"")
    if(NOT DHSH_LUA514_RELEASE)
        message(FATAL_ERROR "DHSH_LUA_ROOT must identify official Lua 5.1.4")
    endif()
    if(NOT DHSH_LUA514_SOURCE_DIR)
        add_library(dhsh_lua514 UNKNOWN IMPORTED)
        set_target_properties(dhsh_lua514 PROPERTIES
            IMPORTED_LOCATION "${DHSH_LUA514_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${DHSH_LUA514_INCLUDE_DIR}")
    endif()
    message(STATUS "Using explicit Lua 5.1.4: ${DHSH_LUA_ROOT}")
else()
    find_package(Lua 5.1.4 EXACT QUIET)
    if(LUA_FOUND AND LUA_VERSION_STRING STREQUAL "5.1.4")
        add_library(dhsh_lua514 INTERFACE)
        target_include_directories(dhsh_lua514 INTERFACE ${LUA_INCLUDE_DIR})
        target_link_libraries(dhsh_lua514 INTERFACE ${LUA_LIBRARIES})
        message(STATUS "Using package Lua ${LUA_VERSION_STRING}")
    else()
        set(DHSH_LUA514_SOURCE_DIR
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lua-5.1.4")
        message(STATUS "Using vendored Lua 5.1.4 (no download)")
    endif()
endif()

if(DHSH_LUA514_SOURCE_DIR)
    add_subdirectory(third_party/lua-5.1.4)
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
