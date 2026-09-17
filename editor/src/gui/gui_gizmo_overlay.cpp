/* Phase 33 GUI gizmo overlay (isolated C++ over led_* math).
 *
 * Screen-space Translate/Rotate/Scale handles drawn over the viewport
 * image; drags map through led_viewport_ray planes into
 * led_gizmo_begin/led_gizmo_apply (ONE coalesced undoable command per
 * drag; Escape cancels with no history entry). Camera owns its input
 * while orbiting; gizmo overlays never submit scene geometry.
 */

#include "gui_internal.h"
