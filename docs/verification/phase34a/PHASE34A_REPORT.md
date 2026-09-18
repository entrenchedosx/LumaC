# Phase 34A Report — Trust Closure, Real GUI Automation & Portable Asset Identity

Baseline: `16afba6` (report basis). Verified HEAD: `d79642d`
(`phase34a: real input harness`), parent `2f14fcf` (`phase34a:
portable asset identity`). Working tree on top of `d79642d` holds
the uncommitted 34A harness + headed suite + arms (13 modified
files + `editor/tests/test_editor_headed.c` untracked; full diff
stat in §9). This report verifies the TREE, not the HEAD commit
message — every claim below was re-run by the verifier.

## 1. Stock gate (must be green)

- `ctest --test-dir build -C Debug`: **94/94 passed**
  (75.08s; includes `test_editor_headed` + `test_portable_identity`).
- Stock headed: `test_editor_headed.exe --shotdir
  build/editor/shots34a`: **114 passed, 0 failed**.
- Stock portable identity: **59 passed, 0 failed**.
- Shots `01-play-click … 12-asset-drop` (12×PPM, 1280×800×3B =
  3072016B each, ~35MB) regenerated in `build/editor/shots34a/`.
- VISUALLY INSPECTED: NO (model limitation — PPMs committed for
  human inspection; programmatic census only).

## 2. Portable identity (Objective B)

- Engine IDs derive from `(project-UUID || sub-key)` via
  `le_identity_for_key`; source path is locator only
  (`engine/src/asset.c:320`, `gltf_bridge.c`, `script_asset.c`,
  `anim_api.c`).
- Sub-key vocabulary verified live: `mesh0:prim0`,
  `mat0:checkerred` (`test_portable_identity` INFO lines).
- Relocation proof: import in C, copy C→D (different absolute
  root), open D → same project UUID, same engine IDs, sidecar
  sub-keys survive; forced reimport keeps every ID (script +
  model legs, 59 checks green).
- Rename/move: UUID + engine IDs stable. Failed reimport keeps
  last-known-good (FAILED status, old handle live). Repair
  reimport stable. Identical bytes ≠ same asset (distinct UUIDs
  AND distinct engine IDs — twin.lua leg).
- Legacy location-bound scenes: NO silent rewrite (explicit
  repair only; fixture regenerated). Design doc honest decision
  `docs/PHASE34A_DESIGN.md` §2.

## 3. Real GUI automation (Objective A)

- Input path: `lc_window_inject_event` → same ring buffer as
  Win32/X11 `lc_window_push_event` → same drain
  `leg_frame_begin` → `leg_panels_frame` → `leg_frame_end`.
  Never widget-direct.
- Proven headed (inject → frames → `led_*`/`le_*` assert → shot):
  Play/Stop toolbar clicks, viewport pick A/B, translate/rotate/
  scale drags (ONE undo entry each), Ctrl+Z/Ctrl+Y, Edit-menu
  Undo/Redo clicks, Ctrl+S save + file-bytes assert + revert +
  relaunch, runtime W-key movement + release stop, script
  drag-and-drop attach, viewport composite census, live
  save→reopen identity.
- Composite determinism: census 302738 non-clear (661×458)
  stock, identical across settled frames; 0 under M-composite.
- Product bugs found + fixed by the harness (all in tree):
  capture button must overlay scene image (`SetCursorScreenPos`
  rewind); `ImGuiMod_*` alias must be submitted or real
  Ctrl+Z/Y/S dead (`gui_input.cpp`); menu-bar hover vetoed by
  `NavWindow` without OS focus (Edit assist `OpenPopup`);
  item-probe reset must precede the bar; script-drop branch
  split (was grouped with material, unreachable).

## 4. Mutation matrix (each arm breaks ONE proof; stock green)

| Arm | Armed result | Targeted FAILs (direct) | Cascade / notes |
|---|---|---|---|
| M-play | 104+3 | H1 click Play→playing, H1 runtime world, H8 world live | H8 cascade (no runtime world) |
| M-stop | 56+14 | H2 Stop→edit, H2 world destroyed | H5/H6/H4 cascade (world never returns to edit) |
| M-undo | 91+4 | H6 Ctrl+Z position, H3 Ctrl+Z rename, H-menu Undo click | H-drop `picked=0` cascade (undo arm breaks pre-drop selection/pick state, NOT the drop path; drop path proven by M-drop tap NEVER FIRED vs stock `attached=1`) |
| M-save | 109+5 | H4 save clears dirty, file holds rename, relaunch, identity | H-menu Undo cascade (rename never persisted) |
| M-pick | 87+8 | H5 A/B select+identity, H6 select/handle/drag/entry | all downstream of pick |
| M-gizmo | 108+6 | H6/H6b/H6c drag moved + one-entry | — |
| M-composite | 113+1 | H-comp scene painted (census 0 vs 302738) | — |
| M-input | 113+1 | H8 runtime object moved +X | — |
| M-drop | 113+1 | H-drop gesture attached (tap NEVER FIRED; GUI half swallows payload AND core half `led_drop_script_onto_object` refuses) | — |
| M-identity | 54+5 (`test_portable_identity_mut_identity`) | script relocation+reimport stability, model relocation+reimport stability | last-good/rename legs cascade (compare against same record ID) |

M-identity arm design (honest): a same-build key poison is
SELF-CONSISTENT (verified: armed fresh ID == independently
re-derived keyed expectation `c77e15…/1664978b…`; relocation
stays stable WITHIN the armed build — the dedup + sidecar design
absorbs it). The arm therefore asserts the cross-build invariant
directly: poisoned key in `led_identity_key_for`
(`import_impl.c`) + forced reimport off the stability-preserving
shortcuts (script fastpath gate + pre-candidate slot retire in
`reimport.c`) + VISIBLE forced ID mismatch (labeled arm behavior,
never production). Stock `test_portable_identity` stays 59/59.

## 5. Remaining matrix — honest status

- 100k-record DB + virtualization + headed search shot: NOT DONE
  (existing `test_assetdb` covers 5-record determinism/conflicts
  only). NOT VERIFIED.
- DPI 125%/150%: NOT VERIFIED — ENVIRONMENT. Single 1920×1080
  screen; `DisplayFramebufferScale` hardcoded 1.0
  (`gui_loop.cpp:57-58`); no DPI plumbing exists. Code-level gap,
  not just env.
- ASan/UBSan/TSan: NOT VERIFIED — ENVIRONMENT (MSVC build; no
  sanitizer configured; LLVM clang present but unused).
- Vulkan validation 0 ERROR/0 VUID: headed tests create with
  `enable_validation=1` and pass, but NO validation-log capture
  asserted. NOT VERIFIED (strictly).
- Windows Debug (this run): green. Release/Shared/Linux: NOT RUN.
- GUI no-regress vs Phase33V: `test_editor_gui`,
  `test_editor_gui_gpu`, `test_editor_negative`,
  `editor_demo_phase33` all pass in the 94/94.
- Asset/prefab DnD beyond script, model/material/prefab/reparent
  drops: NOT proven headed. Rotate rings axis 0..2 use center
  handle only (documented scope).

## 6. Deltas from the incoming report (verified, not trusted)

- Reported HEAD `16afba6` is the BASE, not HEAD: actual HEAD
  `d79642d` (+`2f14fcf`) already contains the identity + harness
  commits. Counts differ honestly: Debug suite is 94 (not 92 —
  +headed +portable-identity); headed stock is 114 (not 108/109).
- M-drop core half (`browser.c` refuse) was UNAPPLIED (whitespace
  match failures) — APPLIED + rebuilt in this tree.
- M-identity "matrix proof" as described (poison-only) does NOT
  bite: verified 59/59 armed after clean rebuild + FNV hand-check.
  Rebuilt as cross-build assertion (forced mismatch, labeled);
  now 54+5 armed, 59/59 stock.
- M-undo/M-save extra FAILs beyond the report's claim are cascade
  (named in the table), not new product defects.

## 7. Verdict

PHASE 34A TRUST NOT CLOSED — DO NOT ADD FEATURES

Reason: the trust-closure bar is the FULL matrix, and three
matrix cells are open — (1) DPI has NO code path
(`DisplayFramebufferScale` ≡ 1.0; 125%/150% cannot pass by
env alone); (2) 100k-record DB headed behavior unproven;
(3) sanitizers + strict validation-VUID-zero + Release/Linux
unrun. The GUI-automation + portable-identity objectives
themselves are proven (94/94, 114 headed, 59 identity, 10/10
arms bite with cascades named). Close the three cells, then
re-verdict. STOP after 34A — no 34B/35 work done or started.
