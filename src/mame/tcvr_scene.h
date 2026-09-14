// license:BSD-3-Clause
// TCVR scene recorder — the System 22 frame as geometry, not pixels.
//
// The driver already walks every polygon and sprite of a frame in painter's
// order to feed its software rasteriser. These hooks record that same walk:
// vertices in eye space (camera zoom applied, perspective divide NOT applied)
// with texture coordinates and brightness, plus every per-primitive parameter
// the per-pixel path uses. A GPU can then rasterise the frame at any
// resolution and for any eye, without one extra tick of emulation.
//
// Plain C ABI so the XR side never includes MAME headers.
#pragma once
#include <cstdint>

extern "C" {

struct tcvr_scene_vertex
{
	float x, y, z;      // eye space, zoom applied; z is depth (direct quads: z holds 1/z)
	float u, v;         // texels, 12-bit domain (sprites: cell pixels 0..w)
	float bri;          // 0..255
};

struct tcvr_scene_prim
{
	uint32_t kind;          // 0 = polygon, 1 = sprite
	uint32_t first_vertex;
	uint32_t vertex_count;  // 3..6 (polygon fan), 4 (sprite)
	uint32_t direct;        // polygon with screen-relative x,y and 1/z in z
	float zoom;             // camera zoom used to scale x,y (stereo needs it)
	int32_t cx, cy;         // viewport origin, screen pixels
	int32_t clip_l, clip_t, clip_r, clip_b;   // inclusive
	uint32_t pens_offset;   // into the pen table (cmode adjustment applied)
	uint32_t bn;            // texture bank (already * 0x1000)
	uint32_t penmask, penshift;
	uint32_t texture_enabled, shade_enabled, prioverchar;
	uint32_t fog_mode;      // 0 none, 1 direct factor, 2 per-depth (czram bank)
	int32_t fogfactor;      // 0..255 (mode 1: 0xff - factor as the scanline uses)
	int32_t cz_sdelta, cz_bank;
	float fog_r, fog_g, fog_b;
	uint32_t fade_enabled;  float fade_r, fade_g, fade_b;  int32_t fadefactor;   // 0..255 blend of fade colour
	uint32_t pfade_enabled; float poly_r, poly_g, poly_b;                          // scale /256
	uint32_t alpha_enabled; int32_t alpha; uint32_t alpha_pen;                     // alpha = 0xff - extra.alpha
	// sprite
	uint32_t sprite_code, flipx, flipy;
};

struct tcvr_scene_frame
{
	uint64_t sequence;
	int32_t width, height;                      // 640, 480
	const tcvr_scene_vertex *vertices; uint32_t vertex_count;
	const tcvr_scene_prim *prims;      uint32_t prim_count;
	const uint32_t *pens;              uint32_t pen_count;      // rgb 0x00RRGGBB
	const uint8_t *czram;              uint32_t cz_banks, cz_entries;
	const uint16_t *text;              uint32_t text_stride;    // mix bitmap indices, 640x480
	uint32_t text_palbase;
	// text mix parameters
	uint32_t mix_alpha_check12, mix_alpha_check13, mix_alpha_mask, mix_alpha_factor;
	uint32_t mix_fade_enabled, mix_fade_factor; float mix_fade_r, mix_fade_g, mix_fade_b;
	uint32_t mix_spot_enabled, mix_spot_factor, mix_spot_palbase;
	const uint16_t *spotram;      // 0x400 entries, valid when mix_spot_enabled
	// Priority bitmap right after the text layer: bit 2 set on every opaque text pixel.
	// The polygons only ever touch bits 0-1, so this bit is safe to read while they draw.
	const uint8_t *pri; uint32_t pri_stride;
	const uint8_t *gamma_r, *gamma_g, *gamma_b;   // 256 each
	uint32_t bg_color;                            // 0x00RRGGBB
};

struct tcvr_scene_assets
{
	const uint8_t *tiledata;   uint32_t tiledata_bytes;   // 16x16x8 tiles, 256 bytes each
	const uint16_t *tilemap;   uint32_t tilemap_entries;  // 256 x 4096
	const uint8_t *tileattr;   uint32_t tileattr_entries;
	const uint8_t *ayx;        uint32_t ayx_entries;      // 16*16*16
	const uint8_t *sprites;    uint32_t sprite_count;     // 32x32 bytes each, contiguous
	uint32_t sprite_width, sprite_height;
	uint32_t pen_count;
};

// Called by the driver (main emulation thread).
// Recording is off until a consumer asks for it: nobody pays for a scene nobody reads.
void tcvr_mame_scene_enable(int enabled);
int tcvr_scene_mode(void);   // 0 off, 1 record, 2 record and skip the CPU rasteriser
void tcvr_scene_begin(void);
void tcvr_scene_poly(const tcvr_scene_vertex *v, int count, const tcvr_scene_prim &p);
void tcvr_scene_sprite(const tcvr_scene_vertex *v, const tcvr_scene_prim &p);
void tcvr_scene_end(const tcvr_scene_frame &frame_params);   // copies pens/czram/text, publishes

// Called by the XR side.
const tcvr_scene_frame *tcvr_mame_acquire_scene(void);      // newest published; valid until next call
int tcvr_mame_scene_assets(tcvr_scene_assets *out);         // static data, 1 on success

}
