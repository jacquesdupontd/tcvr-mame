// license:BSD-3-Clause
// TCVR Model 2 scene recorder — the frame as geometry, not pixels.
//
// Everything here is read off MAME's own Model 2 rasteriser, which stays the
// source of truth and the comparison oracle: model2_3d_render() in
// model2_v.cpp already unpacks every attribute a per-pixel path needs, and
// model2rd.ipp is what a GPU backend has to reproduce exactly.
//
// Why these particular fields, and not a prettier set: draw_scanline_tex()
// interpolates three parameters linearly in screen space -- 1/z, u/z and v/z
// -- then recovers z = 1/ooz, u = uoz*z, v = voz*z per pixel. That is textbook
// perspective correction, so a shader that interpolates the same three values
// with w = 1 and redoes the same division is bit-faithful rather than merely
// similar. Recording x, y already projected keeps the geometry engine on the
// CPU exactly where the hardware had it.
//
// What a consumer still has to honour, straight from model2rd.ipp:
//   - there is no depth buffer. Visibility is the one-bit m_fillmap: polygons
//     arrive in painter's order and the FIRST writer of a pixel wins. On a GPU
//     that is a stencil, never a depth test.
//   - `checker` is stipple translucency, and it keys off (x ^ scanline) & 1,
//     so it belongs to the NATIVE pixel grid and must not scale with
//     resolution.
//   - the colour chain is per texel and cannot be baked into a table: the
//     filtered texel indexes m_lumaram (the tone curve) BEFORE m_colorxlat and
//     m_gamma_table, so filtering happens first and the curve second.
//   - untextured translucent polygons draw nothing at all (draw_scanline_solid
//     returns immediately when Translucent), and textured ones discard a texel
//     whose alpha is under 50%.
//
// Recording is off until something asks for it: nobody pays for a scene nobody
// reads. Plain C ABI so the XR side never includes a MAME header.
#pragma once
#include <cstdint>

extern "C" {

// One projected vertex. x/y are screen pixels; the three parameters are the
// ones MAME iterates, so they interpolate LINEARLY in screen space (w = 1).
// Untextured polygons leave uoz/voz unused: their colour is constant over the
// whole polygon, computed once before the scanline loop.
struct tcvr_m2_vertex
{
	float x, y;
	float ooz;      // 1/z   (m2_poly_vertex::pz after model2_3d_render)
	float uoz;      // u/z/8 (pu)
	float voz;      // v/z/8 (pv)
};

// Per-polygon state, one entry per primitive, mirroring m2_poly_extra_data.
struct tcvr_m2_prim
{
	uint32_t first_vertex;
	uint32_t vertex_count;                  // 3..8, drawn as a fan
	int32_t  clip_l, clip_t, clip_r, clip_b;   // viewport, already clipped, inclusive
	uint32_t textured;                      // texheader[0] bit 14
	uint32_t translucent;                   // texheader[0] bit 13
	uint32_t checker;                       // stipple, native grid only
	uint32_t colorbase, lumabase;
	uint32_t luma;                          // 0..255
	int32_t  texlod;
	uint32_t texsheet;                      // 0 = textureram0, 1 = textureram1
	uint32_t texwidth, texheight, texx, texy;
	uint32_t texwrapx, texwrapy, texmirrorx, texmirrory;
	uint32_t utex, utexminlod, utexx, utexy;   // microtexture
};

struct tcvr_m2_frame
{
	uint64_t sequence;
	int32_t  width, height;                 // 496, 384
	const tcvr_m2_vertex *vertices; uint32_t vertex_count;
	const tcvr_m2_prim   *prims;    uint32_t prim_count;
	// Colour chain, copied per frame because the game rewrites it freely.
	const uint16_t *palram;    uint32_t palram_entries;
	const uint16_t *colorxlat; uint32_t colorxlat_entries;
	const uint8_t  *lumaram;   uint32_t lumaram_entries;
	const uint8_t  *gamma;     uint32_t gamma_entries;   // 256
	// How many primitives the recorder had to drop because a cap was hit. A
	// scene is only comparable to the CPU raster when this reads zero.
	uint32_t dropped_prims, dropped_vertices;
};

// Called by the driver, on the emulation thread.
void tcvr_m2_scene_enable(int mode);   // 0 off, 1 record alongside the CPU raster
int  tcvr_m2_scene_mode(void);
void tcvr_m2_scene_begin(int width, int height);
void tcvr_m2_scene_poly(const tcvr_m2_vertex *v, int count, const tcvr_m2_prim *p);
// The driver fills in width/height and the colour-chain pointers; the
// recorder copies those tables (about 98 KB) and owns the copies.
void tcvr_m2_scene_end(const tcvr_m2_frame *frame_params);

// Called by the XR side. Newest published scene, or null; valid until the next
// call on the same thread.
const tcvr_m2_frame *tcvr_m2_acquire_scene(void);

}
