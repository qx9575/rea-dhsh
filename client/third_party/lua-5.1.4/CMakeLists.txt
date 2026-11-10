if(NOT DEFINED DHSH_LUA514_SOURCE_DIR)
    set(DHSH_LUA514_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
endif()

set(DHSH_LUA514_SOURCE_DIR "${DHSH_LUA514_SOURCE_DIR}" CACHE INTERNAL
    "Official Lua 5.1.4 source directory")

add_library(dhsh_lua514 STATIC
    "${DHSH_LUA514_SOURCE_DIR}/src/lapi.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lauxlib.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lbaselib.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lcode.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/ldebug.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/ldo.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/ldump.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lfunc.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lgc.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/llex.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lmathlib.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lmem.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lobject.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lopcodes.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lparser.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lstate.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lstring.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lstrlib.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/ltable.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/ltablib.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/ltm.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lundump.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lvm.c"
    "${DHSH_LUA514_SOURCE_DIR}/src/lzio.c"
)

target_include_directories(dhsh_lua514 PUBLIC
    "${DHSH_LUA514_SOURCE_DIR}/src"
)

if(MSVC)
    target_compile_definitions(dhsh_lua514 PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
