/* Phase 33 GUI inspector widgets (isolated C++ over led_*).
 *
 * Typed widgets per LED_DATA_* reflection type: bool/int/uint/float
 * (range-clamped), vec3, euler-degrees (quaternion-stable), string,
 * enum combo, read-only asset-ID/color/unavailable rows. Writes route
 * through led_write_property / coalesced led_execute commands.
 */

#include "gui_internal.h"
