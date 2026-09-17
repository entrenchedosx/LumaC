# Editor Authoring Workflow (Phase 33)

The required loop, end to end, as driven by the desktop app
(`luma_editor_app`) and proven headless by the suites:

```
Open project -> Browse assets -> Open/create scene ->
Create/select -> Edit properties -> Move/rotate/scale visually ->
Drag assets -> Create/use prefabs -> Undo/redo -> Save ->
Play -> Interact -> Stop -> Continue editing unchanged
```

## Panel map (`leg_panels_frame`)

| Panel | Model | Commit path |
| --- | --- | --- |
| Menu / Toolbar | session state | `led_scene_*`, play toggle, save |
| Create / Prefab | — | `led_execute(CREATE / INSTANTIATE_PREFAB / CREATE_PREFAB)` |
| Hierarchy | `led_hierarchy_*` | select, `SET_PARENT` / reparent, delete, duplicate |
| Inspector | `led_inspector_*` + `led_write_property` | per-type widgets (BOOL/INT/UINT/FLOAT/VEC3/COLOR3/QUAT/EULER_DEG/STRING/ENUM/ASSET_ID); `script.*` rows rebuild `script.<name>`; read-only guarded; `renderable.mesh_asset` UNAVAILABLE by design; add/remove component with valid defaults |
| Assets | `led_browser_*` + `led_assetdb_*` | filter/search/sort, `led_drag_begin` payloads, double-click scene/prefab open, Import/Reimport, prefab-from-selection, browser-inspect rows |
| Viewport | `led_viewport_*` + composite | camera, pick, gizmo overlay, material/script drops, unparent drops |
| Console | `led_console_*` + script-error mirror | level filter, clear, error routing |
| Status | session/play stats | dirty flag, tick counts, object census |

## Drag payloads

`LUMA_OBJECT` (hierarchy reparent/unparent) and `LUMA_ASSET`
(browser model/script/material/prefab drops) ride the native
drag-drop with payload validation at the target (wrong-type
drops rejected: script-as-material fails loudly, proven in
`test_project_editor`). Prefab instantiate via GUI goes
through `LED_CMD_INSTANTIATE_PREFAB` (undo removes the whole
instance subtree; source survives — census-diff roots).

## Play/Stop

`F5` toggles (`led_play_enter/exit`), `F10` single-steps a
paused runtime. Enter stashes selection + pauses edit; tick
steps the runtime ONLY (`le_engine_step` on the fork; edit
gets a matrices-only refresh so the outliner stays valid);
exit destroys the runtime + restores selection. Edit is
byte-identical across the cycle (canonical capture oracle in
`test_editor_play`, `test_editor_gui_gpu`, and the demo
builder). 1k Play/Stop cycles stay identical (stress in
`test_editor_gui_gpu`).

## Demo

`editor/demo/Phase33Demo/` (rebuild with
`editor_demo_phase33`, idempotent): CameraRig + Camera + Sun
+ Ground + scripted Spinner + one prefab instance;
`Scenes/Main.luma_scene` reopens in a fresh process
(portable: session-local script refs detached before save;
the prefab file carries scripted state). Open it headed:

```
luma_editor_app --project editor/demo/Phase33Demo \
  --scene editor/demo/Phase33Demo/Scenes/Main.luma_scene
```

## Deferred (tracked, not hidden)

Per-key Play injection of arbitrary GUI key events (Phase 34);
prefab overrides; animation timeline; navmesh; material-graph;
visual scripting; profiler; plugins; exporter; D3D12 (record
replacement point = `editor/src/gui/gui_draw.cpp`).
