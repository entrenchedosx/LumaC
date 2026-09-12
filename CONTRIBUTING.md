# Contributing to LumaC

## Requirements

- CMake >= 3.15
- A C11 compiler (MSVC, GCC, or Clang)
- Vulkan SDK for graphics work (`find_package(Vulkan REQUIRED)`)
- X11 development headers on Linux (`libx11-dev`)

## Build

```bash
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Also verify the shared build before submitting:

```bash
cmake -S . -B build_shared -DLUMAC_BUILD_SHARED=ON
cmake --build build_shared --config Debug
ctest --test-dir build_shared -C Debug --output-on-failure
```

## Coding style

- C11, no C++ in library sources (public headers stay C++-compatible).
- Opaque handles (`lc_*`), small modules, explicit error handling.
- No Vulkan, Win32, or X11 types in `include/lumac/lumac.h`.
- Match the existing brace/indentation style (4 spaces, Allman braces).
- Comments explain Vulkan synchronization, lifetime, and queue
  decisions — not what the code obviously does.

## Testing expectation

- Every change must keep `ctest` green (Debug and shared builds).
- New public API needs headless-safe unit tests (`tests/test_*.c`).
- GPU/window integration tests must SKIP cleanly (exit 0) when the
  environment lacks a display or Vulkan runtime.
- Run with validation layers enabled and fix any messages before
  submitting Vulkan changes.

## Pull requests

- Keep scope tight: one phase-sized concern per PR (see `CHANGELOG.md`
  and the roadmap in `README.md`).
- Do not add rendering-adjacent systems (pipelines, shaders, buffers)
  unless the PR is explicitly about them.
- Update `README.md` and `CHANGELOG.md` when behavior changes.
