// Internal interface of aurora's PS3 RSX backend.
//
// The WebGPU backend records render passes and replays them on a worker; the RSX backend
// renders immediately instead: every GX draw, copy and clear is encoded straight into the
// RSX command buffer, which executes in order. Vertex transform and lighting run on the PPU
// (rsx_draw.cpp) and TEV is compiled into NV40 fragment programs (rsx_fragment.cpp).
#pragma once

#include "gfx/common.hpp"
#include "gfx/texture.hpp"
#include "gx/gx.hpp"

#include <rsx/rsx.h>

#include <cstdint>

namespace aurora::rsx {
extern gcmContextData* g_ctx;

bool initialize() noexcept;
void shutdown() noexcept;

// RSX local memory. Frees are deferred until the GPU finished the frames that may use it.
void* local_alloc(uint32_t size, uint32_t align) noexcept;
void local_free(void* ptr) noexcept;
uint32_t local_offset(const void* ptr) noexcept;

// Main memory mapped into the RSX I/O space, recycled per frame.
struct Stream {
  void* ptr = nullptr;
  uint32_t offset = 0;
};
Stream stream_alloc(uint32_t size, uint32_t align = 16) noexcept;
// Changes whenever earlier stream allocations may have been recycled.
uint32_t stream_generation() noexcept;

// A color surface (A8R8G8B8, linear) with an optional Z24S8 depth surface.
struct Target {
  uint32_t colorOffset = 0;
  uint32_t colorPitch = 0;
  uint32_t depthOffset = 0;
  uint32_t depthPitch = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool hasDepth = false;
};
void bind_target(const Target& target) noexcept;
const Target& current_target() noexcept;
const Target& efb_target() noexcept;
Target texture_target(const gfx::TextureRef& texture) noexcept;

// Frame lifecycle (driven by aurora_begin_frame / aurora_end_frame).
void begin_frame() noexcept;
void end_frame() noexcept;
uint32_t frame_index() noexcept;
void set_present_texture(gfx::TextureHandle texture) noexcept;
void wait_idle() noexcept;
// Writes one raw NV40 method.
void emit_method(uint32_t method, uint32_t value) noexcept;
void note_texture_upload() noexcept;
void flush_texture_cache() noexcept;
// Viewport/scissor state as last set by GX, re-applied after copies rebind the EFB.
void apply_viewport_scissor() noexcept;
// Display information.
uint32_t display_width() noexcept;
uint32_t display_height() noexcept;
float present_aspect() noexcept; // 0 = fill the display

// Draw a textured quad covering `dst` (target pixels) sampling `src` (normalized UV rect).
// `program` selects the conversion fragment program (see rsx_fragment.cpp).
struct QuadDraw {
  uint32_t texOffset = 0;
  uint32_t texPitch = 0;
  uint32_t texWidth = 0;
  uint32_t texHeight = 0;
  float u0 = 0.f, v0 = 0.f, u1 = 1.f, v1 = 1.f;
  float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
  bool linear = true;
  uint32_t copyFormat = GX_TF_RGBA8;
  bool forceOpaqueAlpha = false;
};
void draw_quad(const QuadDraw& quad) noexcept;
// Resets write masks that would otherwise filter a surface clear.
void prepare_clear() noexcept;
void clear_rect(const gfx::ClipRect& rect, bool color, bool alpha, bool depth, Vec4<float> clearColor,
                float clearDepth) noexcept;

// GX geometry.
void draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount, uint32_t vtxStride) noexcept;

// Fragment programs.
struct FragmentProgram;
const FragmentProgram* gx_fragment_program(const gx::ShaderConfig& config) noexcept;
const FragmentProgram* copy_fragment_program(uint32_t copyFormat, bool forceOpaqueAlpha) noexcept;
// Fills the program's constants and binds it.
void bind_fragment_program(const FragmentProgram* program, const Vec4<float>* constants) noexcept;
uint32_t fragment_constant_count(const FragmentProgram* program) noexcept;
// GX texture maps (RSX texture units) the program samples.
uint32_t fragment_texture_mask(const FragmentProgram* program) noexcept;
void bind_passthrough_vertex_program() noexcept;
void initialize_programs() noexcept;
} // namespace aurora::rsx
