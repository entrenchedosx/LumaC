# Editor Architecture (Phase 31)

Godot-like editor **foundation**, not a full editor. Headless
`EditorCore` + headless `EditorGUI` models, proven by CTest without
any window, GPU, or GUI framework.

## Layering

```
editor (led_*) — selection, commands, reflection, play, viewport math
  |
  v
engine (le_*) — objects, hierarchy, components, scenes, scripts
  |
  v
assets -> renderer -> LumaC
```

Invariants (CI-audited in `editor/CMakeLists.txt`):

- `editor → engine → assets → renderer → LumaC`. No back-edges.
- Editor sources use only public headers. Never `engine/src/internal`,
  `renderer/src`, backend or `*_internal` symbols.
- C11, no C++ in library sources. No `typeid`/`dynamic_cast`/RTTI.
- No Lua spellings outside `engine/src/script/`; the editor talks to
  scripts via `le_script_*` C API only.
- Reflection routes through validated engine APIs
  (`le_object_is_alive`, `le_object_has_component`, per-component
  getters). The editor never dereferences opaque handles.
- Commands own heap memory explicitly; no raw-pointer borrowing
  across undo boundaries (payloads are deep-copied values).
- Gameplay never runs on the edit world (see `EDITOR_PLAY_MODE.md`).

## Module map

| File | Responsibility |
| --- | --- |
| `editor_core.c` | Session create/destroy/attach/detach/tick/stats |
| `editor_selection.c` | Ordered unique handle set, liveness-filtered |
| `editor_reflect.c` | Static reflection tables + describe/read/write |
| `editor_hierarchy.c` | Roots→children snapshot (outliner model) |
| `editor_inspector.c` | Property rows for one object (inspector model) |
| `editor_commands.c` | Command apply/inverse + bounded history |
| `editor_scene.c` | New/open/save/revert + dirty tracking |
| `editor_play.c` | Play enter/tick/pause/step/exit isolation |
| `editor_viewport.c` | Orbit camera, unproject, picking, AABB, framing |
| `editor_gizmo.c` | Translate/rotate/scale drag intents (math only) |
| `editor_console.c` | Log ring + script-error mirror |
| `editor_shortcuts.c` | Shortcut table, focus policy, action dispatch |
| `editor_serialize.c` | Project sidecar text (editor state only) |

## Session model

`led_session` borrows `le_engine *` + `le_world *` (host-owned, never
destroyed by the editor). `attach()` binds, `detach()`/`destroy()`
releases editor-side state only (exiting play first so no runtime
world leaks). `tick(dt)` does a matrices-only edit refresh (scripts
never run) + stale-selection prune. Stats are plain data.

## Selection

Ordered unique `le_object` array (cap 256), ascending-slot order,
validated by `le_object_is_alive` on every read path. Stale handles
(right index, wrong generation) fail safely and never alias a
slot-reusing successor. Selection is view state: changes never mark
the scene dirty. Subtree select uses an explicit stack (100k-deep
safe); by-name select uses `le_world_find_by_name` (first match).

## Hierarchy + inspector (GUI models)

`led_hierarchy_refresh()` snapshots roots (ascending via the Phase 31
`le_world_get_roots` engine API) + iterative DFS into
session-owned rows `{handle, depth, has_children, borrowed name}`.
`led_inspect()` builds rows `{descriptor, value snapshot}` over the
reflection layer + live script exports. Both are plain data for a
future GUI; both refresh headless and are unit-tested.

## Viewport + gizmos (math only)

Orbit state `{target, yaw, pitch, distance, fov, near, far}` derives
`lr_camera` via `lr_camera_look_at` + `lr_camera_set_perspective`;
`led_viewport_ray` inverts view-proj (column-major, Vulkan NDC 0..1)
into a world ray; picking is `le_physics_raycast` (colliders
required — mesh-without-collider picking is documented
unsupported). AABBs compose `lr_mesh_get_bounds` × world matrix
(pointer renderables only; asset-backed report unavailable, never
guessed; mirrored bases expand conservatively). Gizmos emit
translate/rotate/scale intents applied through coalesced undoable
commands; `led_gizmo_lines` emits a 90-float preview soup for a
future Phase-32 overlay. No renderer submission happens here.

## Console, shortcuts, focus

Ring buffer (1024 entries, oldest drops counted), script-error
mirror via `le_script_get_last_error`, static shortcut table
(Ctrl+Z/Y/D/S, Delete, F5/Shift+F5, F10, F), focus flag suppressing
viewport shortcuts while a text field owns focus, action dispatch
(undo/redo/delete/duplicate/play/stop/step/save/focus-selection).

## Non-goals (Phase 32+)

Asset browser, shader/timeline/graph editors, physics/navmesh/
terrain editors, material graph, visual scripting, profiler,
packages, C#, MCP server, networking, prefab UI, docking, themes,
exporter, and any renderer submission path for gizmos.
