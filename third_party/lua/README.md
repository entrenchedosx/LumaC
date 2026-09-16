# third_party/lua — vendored upstream Lua 5.4.8

- Version: Lua 5.4.8 (released 21 May 2025, lua.org).
- Source: https://www.lua.org/ftp/lua-5.4.8.tar.gz (32 library
  translation units + public headers, unmodified).
- Excluded: `lua.c` / `luac.c` (standalone interpreter/compiler
  frontends — Luma embeds the library only) and `doc/` (see
  lua.org for the reference manual).
- License: MIT (see `LICENSE` in this directory — upstream Lua
  license text, preserved verbatim).
- Modifications: NONE. Upstream code is built as-is; all Luma
  policy (sandboxed stdlibs, module roots, instruction budgets)
  lives in `engine/src/script/` and never patches these files.
- Build: compiled once as `luma_lua` (static) from the engine
  tree; warnings from upstream sources are left alone (no /W3
  escalation on this directory).
