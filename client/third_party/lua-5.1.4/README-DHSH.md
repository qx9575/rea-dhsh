# Vendored Lua 5.1.4 provenance

- Version: official Lua 5.1.4 (the version observed in the DHSH `Game.exe` SHA-256 `7a5e2c085ac79557ca104a2917e06f1a7e5f2827e4b68753bbaf1c754a4e78dc`).
- Source archive: https://www.lua.org/ftp/lua-5.1.4.tar.gz
- Archive SHA-256: `b038e225eaf2a5b57c9bcc35cd13aa8c6c8288ef493d52970c9545074098af3a`
- Retrieved and verified: 2026-11-09.
- The archive was extracted without modifying the upstream sources. `lua.c`, `luac.c`, and `print.c` are interpreter/compiler tools and are not linked into the client.

CMake uses an explicit `DHSH_LUA_ROOT` or an exact `find_package(Lua 5.1.4 EXACT)` result when available. Otherwise it builds these vendored sources. Configuration never downloads Lua.
