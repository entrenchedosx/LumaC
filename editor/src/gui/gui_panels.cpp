/* Phase 33 GUI panels (isolated C++ over led_* commands).
 *
 * Dear ImGui panels driving EditorCore ONLY through led_* APIs:
 * menu/toolbar, hierarchy (select/reparent), inspector (reflected
 * widgets), assets (browse/drag), console, status bar. Every mutation
 * funnels GUI intent -> led_command -> engine/project API -> history.
 * No direct component-memory writes; no long-lived borrowed engine
 * pointers (IDs/handles/owned strings only).
 */

#include "gui_internal.h"
