## Summary

## Checks

- [ ] Debug build passes (`cmake --build build --config Debug`)
- [ ] Release build passes
- [ ] `ctest` green (note any environment-dependent skips)
- [ ] No new validation-layer messages (Vulkan changes)
- [ ] `lumac.h` exposes no Vulkan/Win32/X11 types
- [ ] Docs updated (`README.md` / `CHANGELOG.md` if behavior changed)
- [ ] Scope stays within the PR's stated goal
