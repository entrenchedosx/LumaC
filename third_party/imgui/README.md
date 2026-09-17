# imgui (vendored, docking branch)

Dear ImGui immediate-mode GUI for the Phase 33 Luma Editor desktop
application (docking layout + panels + viewport + property widgets).

- Upstream: https://github.com/ocornut/imgui
- Version: **v1.92.9b-docking** (exact tag; tag commit `9acdfbf4`)
- Archive: `v1.92.9b-docking.tar.gz` in this directory
  (Length: 2296581, SHA256:
  `90DED916BD57DB2E0E171B6B098940A47C6F5042725DCDC67FB19940CA8BFDCC`)
- Extracted: `imgui-1.92.9b-docking/` (unmodified upstream files —
  ONLY the compiled core subset is extracted: `imgui.cpp`,
  `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp` plus
  their headers `imconfig.h`, `imgui.h`, `imgui_internal.h`,
  `imstb_rectpack.h`, `imstb_textedit.h`, `imstb_truetype.h`, and
  `LICENSE.txt`. Backends / examples / docs / misc stay in the
  tarball; re-extract with `tar -xzf v1.92.9b-docking.tar.gz` if
  ever needed)
- License: MIT (see `imgui-1.92.9b-docking/LICENSE.txt`)
- Used by: `editor/src/gui/*` (sole ImGui translation units) and
  the `luma_editor` desktop executable
- Master-branch `v1.92.9b` was evaluated and REJECTED: it has no
  docking support (`ImGuiConfigFlags_DockingEnable`, `DockSpace`,
  `DockSpaceOverViewport` are docking-branch only), and Phase 33
  requires upstream docking (no custom docking engine).

Confinement (audited at configure time by `editor/CMakeLists.txt`):

- `imgui.h` / `imgui_internal.h` may appear ONLY in
  `editor/src/gui/*` (the isolated GUI layer). They never leak into
  `led_*` / engine / renderer / LumaC / assets / editor core.
- The GUI layer is C++ (ImGui is C++-only); every other Luma layer
  stays C11. No C++ types cross the GUI boundary: `editor_gui_*`
  exposes a C ABI over opaque handles.
- Rendering goes through PUBLIC LumaC only (`lc_*` + the Phase 33
  `lc_blend_attachment` / `lc_encoder_set_scissor` additions). No
  Vulkan headers, no `imgui_impl_vulkan.*`, no Win32/X11 headers in
  the GUI layer: input comes from `lc_window_read_event`, drawing
  from `ImDrawData` walked manually into LumaC buffers/pipelines.

Modifications: NONE. Upstream files are built as-is; all Luma policy
(input mapping, texture bridge, scissor clipping, viewport
compositing) lives in `editor/src/gui/` and never patches these
files.

No package-manager or network dependency: builds are reproducible
from this checkout alone (same discipline as `third_party/cgltf`,
`third_party/lua`, `third_party/stb`).
