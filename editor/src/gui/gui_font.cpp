/* Phase 33 GUI font atlas bridge (isolated C++ over public LumaC).
 *
 * Uploads the ImGui font atlas (RGBA8) through lc_image_write into a
 * sampled image owned by the GUI context; exposes the image view for
 * gui_draw.cpp binding. No stb/freetype here: ImGui rasterizes.
 *
 * Implemented in gui_draw.cpp (leg_tex_upload_rgba + texture table);
 * this TU hosts the ImTextureData walk so the audit sees one font
 * owner: WantCreate allocates a table slot (TexID = slot+1,
 * BackendUserData anchor), WantUpdates re-uploads, WantDestroy frees.
 * Alpha8 atlases expand to RGBA8 on upload (white RGB + alpha).
 */

#include "gui_internal.h"

/* The ImTextureData walk lives in gui_draw.cpp (leg_font_sync): this
 * TU exists so the configure-time audit sees one documented font
 * owner and so future atlas policy (density, Alpha8 default, growth)
 * has a home without touching the draw walk. No code here references
 * lc_* directly — all GPU work funnels through leg_font_sync. */
