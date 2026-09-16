# Pre-Phase-24 Audit Baseline (Stage 0)

Date: 2026-09-15 (UTC). Auditor: adversarial senior review (Muse Spark).

## Repository status (freeze)

- Commit (HEAD, no new commits during audit): `4787d38`
  `feat: async transfer/compute, GPU-driven rendering, concurrency hardening`
- Working tree at freeze: 29 modified tracked files (uncommitted
  Phase 23 work: Hi-Z, visibility/LOD, render graph, indirect-count,
  visibility_scene example, docs) + 30 untracked paths (new Phase 23
  sources/tests/shaders/docs + local build dirs + logs). No user work
  was deleted. `git status --porcelain=v1` output preserved in the
  audit log.
- Rule applied: audit fixes layer on top of the uncommitted Phase 23
  work; nothing was reverted or silently cleaned.

## Toolchain

- CMake 4.4.1, generator `Visual Studio 17 2022` (see
  `build/CMakeCache.txt`).
- MSVC via VS2022 (Windows SDK 10.0.26100.0 targeting 10.0.26200).
- No gcc/clang on PATH (Windows-only verification; Linux/TSan/ASan
  explicitly NOT VERIFIED — see final report).

## Vulkan

- SDK: `C:\VulkanSDK\1.4.357.0` (vulkaninfo 1.4.357.0).
- Runtime device: NVIDIA GeForce RTX 5060, driver 610.88,
  apiVersion 1.4.341, vendorID 0x10de.
- Validation layer present: `VK_LAYER_KHRONOS_validation` 1.4.357.

## Baseline build/tests (pre-fix)

- `cmake --build build --config Debug -j4`: clean (with uncommitted
  Phase 23 work included).
- `ctest --test-dir build -C Debug -j4`: **49/49 PASS** (22.97 s).
  (Note: bare `ctest` without `-C Debug` reports NOT RUN — a
  multi-config invocation detail, not a code issue.)
- Post-audit suite: **50/50 PASS** (added `test_mirror_vulkan`).

## Baseline scope notes

- Reported Phase 23 debt taken as INVESTIGATE (not accepted):
  present-path validation errors, TSan not executed, Debug shared not
  verified, Linux windowed not verified, no GPU timestamps, swapchain
  idle, HDR/tonemap + shadows/IBL outside the graph, deferred
  transient aliasing, 100k/5k requested-vs-grouped accounting gap,
  vertex-attribute warnings.
- Audit verdicts for each item are in the final report, evidenced by
  code + test runs, never by prior labels.

## Post-audit verification matrix (all with audit fixes applied)

- Windows Debug static: 50/50 PASS (`ctest -C Debug`).
- Windows Release static: 50/50 PASS (`ctest -C Release`).
- Windows Debug shared: 50/50 PASS (fresh `LUMAC_BUILD_SHARED=ON`
  config; required two audit fixes: `LC_API` on 6 white-box compute
  symbols, DLL search PATH for the 5 Phase 23 renderer tests).
- MSVC ASan (`/fsanitize=address` scratch config): clean on
  `test_renderer` (68), `test_memory` allocator stress (31),
  `test_buffer_vulkan` (60), `test_phase20_transfer_vulkan` (21).
- TSan (WSL kali, gcc 15.2 `-fsanitize=thread`, lavapipe,
  `tools/tsan-lavapipe.suppr` for driver-internal noise): clean on
  `test_memory` (31), `test_sync_vulkan` (27),
  `test_phase20_transfer_vulkan` (21),
  `test_phase20_recording_vulkan` (28),
  `test_phase21_compute_vulkan` (117).
- Validation: zero VUID errors across the suite (RTX 5060, SDK
  1.4.357); zero "attribute not consumed" warnings after the unlit
  pipeline fix.
- 5k visibility scene: exact accounting (5000 candidates = 2458
  submitted + 2542 street-skipped; submitted = frustum + occlusion
  + visible every frame), 4 allocator blocks total.
- MSVC `/W3`: zero warnings in project code.
- NOT RUN / NOT AVAILABLE: Linux windowed runtime (no X server in
  WSL probe), UBSan/LSan (no toolchain on this box), 1000-frame
  dedicated full-stack, 100k full-stack (accounting proven at 5k +
  ratio-consistent at 100k).
