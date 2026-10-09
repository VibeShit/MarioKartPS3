// aurora::gfx on RSX: textures, EFB copies, offscreen passes, viewport and scissor.
//
// The staging/pipeline/draw-command entry points of the WebGPU backend are still referenced
// by command_processor.cpp, whose draw path is redirected to rsx::draw on this backend; they
// are defined here as inert stubs.

#include "rsx_backend.hpp"

#include "gfx/clear.hpp"
#include "gfx/depth_peek.hpp"
#include "gfx/efb_ram_copy.hpp"
#include "gfx/tex_copy_conv.hpp"
#include "gfx/tex_palette_conv.hpp"
#include "gfx/texture_convert.hpp"
#include "gfx/texture_replacement.hpp"
#include "gx/pipeline.hpp"
#include "internal.hpp"

#include <rsx/rsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aurora::gfx {
namespace {
Module Log("aurora::gfx::rsx");

Viewport g_viewport{0.f, 0.f, 640.f, 480.f, 0.f, 1.f};
ClipRect g_scissor{0, 0, 640, 480};
bool g_inOffscreen = false;
rsx::Target g_offscreen;
Viewport g_savedViewport;
ClipRect g_savedScissor;

uint32_t align_up(uint32_t value, uint32_t align) { return (value + align - 1) & ~(align - 1); }

TextureHandle make_texture(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat, bool renderTarget = false) {
  width = std::max(width, 1u);
  height = std::max(height, 1u);
  auto ref = std::make_shared<TextureRef>(wgpu::Texture{}, wgpu::TextureView{}, wgpu::TextureView{},
                                          wgpu::Extent3D{width, height, 1}, wgpu::TextureFormat::RGBA8Unorm,
                                          std::max(mips, 1u), gxFormat);
  ref->rsxPitch = align_up(width * 4, 64);
  // Render targets need surface alignment; sampled-only textures just texture alignment.
  ref->rsxMemory = rsx::local_alloc(ref->rsxPitch * height, renderTarget ? 4096 : 128);
  ref->rsxOffset = rsx::local_offset(ref->rsxMemory);
  return ref;
}

// RGBA8 bytes -> A8R8G8B8 words, mip 0 only.
void upload_rgba8(TextureRef& ref, const uint8_t* data, size_t size) {
  const uint32_t width = ref.size.width;
  const uint32_t height = ref.size.height;
  if (data == nullptr || size < static_cast<size_t>(width) * height * 4) {
    std::memset(ref.rsxMemory, 0, static_cast<size_t>(ref.rsxPitch) * height);
    rsx::note_texture_upload();
    return;
  }
  auto* dstBase = static_cast<uint8_t*>(ref.rsxMemory);
  for (uint32_t y = 0; y < height; ++y) {
    const auto* src = data + static_cast<size_t>(y) * width * 4;
    auto* dst = reinterpret_cast<uint32_t*>(dstBase + static_cast<size_t>(y) * ref.rsxPitch);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = src + x * 4;
      dst[x] = (static_cast<uint32_t>(p[3]) << 24) | (static_cast<uint32_t>(p[0]) << 16) |
               (static_cast<uint32_t>(p[1]) << 8) | p[2];
    }
  }
  rsx::note_texture_upload();
}

bool is_palette_index_format(u32 fmt) { return fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2; }

uint32_t copy_dimension(float value) { return static_cast<uint32_t>(std::max(value, 0.f)); }
} // namespace

AuroraStats g_stats{};
uint32_t g_drawCallCount = 0;
uint32_t g_mergedDrawCallCount = 0;

TextureFormatInfo format_info(wgpu::TextureFormat) noexcept { return {1, 1, 4, false}; }

uint64_t calc_texture_size(wgpu::TextureFormat, uint32_t width, uint32_t height, uint32_t) noexcept {
  return static_cast<uint64_t>(width) * height * 4;
}

TextureHandle new_static_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 format, ArrayRef<uint8_t> data,
                                    bool tlut, const char* label) noexcept {
  auto handle = make_texture(width, height, mips, format);
  ConvertedTexture converted;
  if (tlut) {
    converted = convert_tlut(format, width, data);
  } else if (format != InvalidTextureFormat && !is_palette_index_format(format)) {
    // Only mip 0 is uploaded (linear RSX textures have no mip chain).
    converted = convert_texture(format, width, height, 1, data);
  }
  if (!converted.data.empty()) {
    handle->hasArbitraryMips = converted.hasArbitraryMips;
    upload_rgba8(*handle, converted.data.data(), converted.data.size());
  } else if (is_palette_index_format(format)) {
    Log.warn("{}: palette index textures are not supported", label);
    upload_rgba8(*handle, nullptr, 0);
  } else {
    upload_rgba8(*handle, data.data(), data.size());
  }
  return handle;
}

TextureHandle new_dynamic_texture_2d(uint32_t width, uint32_t height, uint32_t mips, u32 gxFormat,
                                     const char*) noexcept {
  return make_texture(width, height, mips, gxFormat);
}

TextureHandle new_render_texture(uint32_t width, uint32_t height, u32 gxFormat, const char*) noexcept {
  return make_texture(width, height, 1, gxFormat, true);
}

TextureHandle new_conv_texture(uint32_t width, uint32_t height, u32 gxFormat, const char*) noexcept {
  return make_texture(width, height, 1, gxFormat, true);
}

void write_texture(TextureRef& ref, ArrayRef<uint8_t> data) noexcept {
  // The GPU may still sample the previous contents this frame.
  rsx::wait_idle();
  ConvertedTexture converted;
  if (ref.gxFormat != InvalidTextureFormat) {
    converted = convert_texture(ref.gxFormat, ref.size.width, ref.size.height, 1, data);
  }
  if (!converted.data.empty()) {
    upload_rgba8(ref, converted.data.data(), converted.data.size());
  } else {
    upload_rgba8(ref, data.data(), data.size());
  }
}

wgpu::SamplerDescriptor TextureBind::get_descriptor() const noexcept { return {}; }

Vec2<uint32_t> get_render_target_size() noexcept {
  if (g_inOffscreen) {
    return {g_offscreen.width, g_offscreen.height};
  }
  return {rsx::efb_target().width, rsx::efb_target().height};
}

Vec2<uint32_t> get_frame_buffer_size() noexcept { return {rsx::efb_target().width, rsx::efb_target().height}; }

void set_viewport(const Viewport& viewport) noexcept {
  g_viewport = viewport;
  rsx::apply_viewport_scissor();
}

void set_scissor(const ClipRect& scissor) noexcept {
  g_scissor = scissor;
  rsx::apply_viewport_scissor();
}

uint32_t current_frame() noexcept { return rsx::frame_index(); }
bool is_offscreen() noexcept { return g_inOffscreen; }
uint32_t get_sample_count() noexcept { return 1; }

void begin_offscreen(uint32_t width, uint32_t height) {
  if (!g_inOffscreen) {
    g_savedViewport = g_viewport;
    g_savedScissor = g_scissor;
  }
  if (g_offscreen.width != width || g_offscreen.height != height) {
    // The previous offscreen surfaces may still be in flight.
    rsx::wait_idle();
    g_offscreen = {};
    g_offscreen.width = width;
    g_offscreen.height = height;
    g_offscreen.colorPitch = align_up(width * 4, 64);
    g_offscreen.depthPitch = g_offscreen.colorPitch;
    static void* s_color = nullptr;
    static void* s_depth = nullptr;
    rsx::local_free(s_color);
    rsx::local_free(s_depth);
    s_color = rsx::local_alloc(g_offscreen.colorPitch * height, 4096);
    s_depth = rsx::local_alloc(g_offscreen.depthPitch * height, 4096);
    g_offscreen.colorOffset = rsx::local_offset(s_color);
    g_offscreen.depthOffset = rsx::local_offset(s_depth);
    g_offscreen.hasDepth = true;
  }
  g_inOffscreen = true;
  rsx::bind_target(g_offscreen);
  rsxSetScissor(rsx::g_ctx, 0, 0, width, height);
  rsxSetClearColor(rsx::g_ctx, 0);
  rsxSetClearDepthStencil(rsx::g_ctx, 0xFFFFFF00);
  rsx::prepare_clear();
  rsxClearSurface(rsx::g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);
  g_viewport = {0.f, 0.f, static_cast<float>(width), static_cast<float>(height), 0.f, 1.f};
  g_scissor = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
  rsx::apply_viewport_scissor();
}

void end_offscreen() {
  if (!g_inOffscreen) {
    return;
  }
  g_inOffscreen = false;
  rsx::bind_target(rsx::efb_target());
  g_viewport = g_savedViewport;
  g_scissor = g_savedScissor;
  rsx::apply_viewport_scissor();
}

void resolve_pass(TextureHandle texture, ClipRect rect, bool clearColor, bool clearAlpha, bool clearDepth,
                  Vec4<float> clearColorValue, float clearDepthValue, GXTexFmt resolveFormat,
                  const Vec4<float>* sourceRectPixels, bool halfScale, const std::array<u32, 3>*, bool forceOpaqueAlpha,
                  float, bool, bool, bool) {
  (void)halfScale;
  const rsx::Target source = rsx::current_target();
  const auto targetWidth = static_cast<int32_t>(source.width);
  const auto targetHeight = static_cast<int32_t>(source.height);
  if (targetWidth <= 0 || targetHeight <= 0) {
    return;
  }
  Vec4<float> sourceRect = sourceRectPixels != nullptr
                               ? *sourceRectPixels
                               : Vec4<float>{static_cast<float>(rect.x), static_cast<float>(rect.y),
                                             static_cast<float>(rect.width), static_cast<float>(rect.height)};
  {
    const int32_t left = std::clamp(rect.x, 0, targetWidth - 1);
    const int32_t top = std::clamp(rect.y, 0, targetHeight - 1);
    const int32_t right = std::clamp(rect.x + rect.width, left + 1, targetWidth);
    const int32_t bottom = std::clamp(rect.y + rect.height, top + 1, targetHeight);
    rect = {left, top, right - left, bottom - top};
    const float srcW = static_cast<float>(targetWidth);
    const float srcH = static_cast<float>(targetHeight);
    const float srcLeft = std::clamp(sourceRect.x(), 0.0f, srcW);
    const float srcTop = std::clamp(sourceRect.y(), 0.0f, srcH);
    const float srcRight = std::clamp(sourceRect.x() + sourceRect.z(), srcLeft, srcW);
    const float srcBottom = std::clamp(sourceRect.y() + sourceRect.w(), srcTop, srcH);
    sourceRect = {srcLeft, srcTop, std::max(srcRight - srcLeft, 1.0f), std::max(srcBottom - srcTop, 1.0f)};
  }

  if (texture && texture->rsxMemory != nullptr) {
    // Finish rendering into the source before it is sampled.
    rsxSetWaitForIdle(rsx::g_ctx);
    rsxInvalidateTextureCache(rsx::g_ctx, GCM_INVALIDATE_TEXTURE);
    rsx::bind_target(rsx::texture_target(*texture));
    rsx::QuadDraw quad;
    quad.texOffset = source.colorOffset;
    quad.texPitch = source.colorPitch;
    quad.texWidth = source.width;
    quad.texHeight = source.height;
    quad.u0 = sourceRect.x() / static_cast<float>(source.width);
    quad.v0 = sourceRect.y() / static_cast<float>(source.height);
    quad.u1 = (sourceRect.x() + sourceRect.z()) / static_cast<float>(source.width);
    quad.v1 = (sourceRect.y() + sourceRect.w()) / static_cast<float>(source.height);
    quad.x1 = static_cast<float>(texture->size.width);
    quad.y1 = static_cast<float>(texture->size.height);
    quad.copyFormat = resolveFormat;
    quad.forceOpaqueAlpha = forceOpaqueAlpha;
    rsx::draw_quad(quad);
    rsxSetWaitForIdle(rsx::g_ctx);
    rsxInvalidateTextureCache(rsx::g_ctx, GCM_INVALIDATE_TEXTURE);
    rsx::bind_target(source);
  }

  if (clearColor || clearAlpha || clearDepth) {
    rsx::clear_rect(rect, clearColor, clearAlpha, clearDepth, clearColorValue, clearDepthValue);
  }
  rsx::apply_viewport_scissor();
}

void queue_palette_conv(tex_palette_conv::ConvRequest req) {
  // Converting EFB copies through a TLUT needs a GPU read-back path the RSX backend lacks;
  // show the raw copy instead of nothing.
  if (!req.src || !req.dst || req.src->rsxMemory == nullptr || req.dst->rsxMemory == nullptr) {
    return;
  }
  const rsx::Target previous = rsx::current_target();
  rsxSetWaitForIdle(rsx::g_ctx);
  rsxInvalidateTextureCache(rsx::g_ctx, GCM_INVALIDATE_TEXTURE);
  rsx::bind_target(rsx::texture_target(*req.dst));
  rsx::QuadDraw quad;
  quad.texOffset = req.src->rsxOffset;
  quad.texPitch = req.src->rsxPitch;
  quad.texWidth = req.src->size.width;
  quad.texHeight = req.src->size.height;
  quad.x1 = static_cast<float>(req.dst->size.width);
  quad.y1 = static_cast<float>(req.dst->size.height);
  rsx::draw_quad(quad);
  rsxSetWaitForIdle(rsx::g_ctx);
  rsxInvalidateTextureCache(rsx::g_ctx, GCM_INVALIDATE_TEXTURE);
  rsx::bind_target(previous);
  rsx::apply_viewport_scissor();
}

void clear_caches() noexcept {}
void push_debug_group(std::string) {}
void insert_debug_marker(std::string) {}

// Inert WebGPU staging surface (see file comment).
Range push_verts(const uint8_t*, size_t) { return {}; }
Range push_indices(const uint8_t*, size_t) { return {}; }
Range push_uniform(const uint8_t*, size_t) { return {}; }
Range push_storage(const uint8_t*, size_t) { return {}; }
std::pair<ByteBuffer, Range> map_uniform(size_t length) { return {ByteBuffer{length}, Range{}}; }
uint32_t align_uniform(uint32_t value) { return (value + 255u) & ~255u; }
uint64_t staging_uniform_bytes(uint64_t bytes) { return bytes; }
uint64_t staging_storage_bytes(uint64_t bytes) { return bytes; }
bool staging_has_space(const StagingSizes&) { return true; }
void split_staging_batch() {}

template <>
gx::DrawData* get_last_draw_command() {
  return nullptr;
}
template <>
void push_draw_command(gx::DrawData) {}
template <>
PipelineRef pipeline_ref(const gx::PipelineConfig&) {
  return 0;
}

template <>
PipelineRef pipeline_ref(const clear::PipelineConfig& config) {
  return (config.clearColor ? 1u : 0u) | (config.clearAlpha ? 2u : 0u) | (config.clearDepth ? 4u : 0u);
}

template <>
void push_draw_command(clear::DrawData data) {
  const auto size = get_render_target_size();
  const ClipRect rect = data.useScissor ? data.scissor
                                        : ClipRect{0, 0, static_cast<int32_t>(size.x), static_cast<int32_t>(size.y)};
  rsx::clear_rect(rect, (data.pipeline & 1u) != 0, (data.pipeline & 2u) != 0, (data.pipeline & 4u) != 0,
                  {static_cast<float>(data.color.r), static_cast<float>(data.color.g),
                   static_cast<float>(data.color.b), static_cast<float>(data.color.a)},
                  data.depth);
  rsx::apply_viewport_scissor();
}

namespace tex_copy_conv {
bool needs_conversion(GXTexFmt fmt) {
  // Conversions run in the copy fragment program; every copy target is plain A8R8G8B8.
  (void)fmt;
  return false;
}
} // namespace tex_copy_conv

namespace texture_replacement {
void register_tlut(const GXTlutObj*, const void*, GXTlutFmt, uint16_t) noexcept {}
void load_tlut(const GXTlutObj*, uint32_t) noexcept {}
std::optional<TextureHandle> find_replacement(const GXTexObj_&) noexcept { return std::nullopt; }
} // namespace texture_replacement

namespace efb_ram {
void schedule(void*, uint32_t, uint32_t, GXTexFmt, TextureHandle) noexcept {}
bool has_pending(void*) noexcept { return false; }
} // namespace efb_ram

namespace depth_peek {
void request_snapshot() noexcept {}
bool read_latest(uint16_t, uint16_t, uint32_t&) noexcept { return false; }
void poll() noexcept {}
} // namespace depth_peek
} // namespace aurora::gfx

namespace aurora::rsx {
namespace {
uint32_t clamp_u16(float value, uint32_t max) {
  return static_cast<uint32_t>(std::clamp(value, 0.f, static_cast<float>(max)));
}
} // namespace

void apply_viewport_scissor() noexcept {
  using gfx::g_scissor;
  using gfx::g_viewport;
  const auto& target = current_target();
  if (target.width == 0 || target.height == 0) {
    return;
  }
  const auto& vp = g_viewport;
  // The viewport rectangle only clips; the transform below may extend past the target.
  const uint32_t x0 = clamp_u16(std::floor(std::min(vp.left, vp.left + vp.width)), target.width);
  const uint32_t y0 = clamp_u16(std::floor(std::min(vp.top, vp.top + vp.height)), target.height);
  const uint32_t x1 = clamp_u16(std::ceil(std::max(vp.left, vp.left + vp.width)), target.width);
  const uint32_t y1 = clamp_u16(std::ceil(std::max(vp.top, vp.top + vp.height)), target.height);
  const float scale[4] = {vp.width * 0.5f, -vp.height * 0.5f, 1.f, 0.f};
  const float offset[4] = {vp.left + vp.width * 0.5f, vp.top + vp.height * 0.5f, 0.f, 0.f};
  rsxSetViewport(g_ctx, x0, y0, std::max(x1 - x0, 1u), std::max(y1 - y0, 1u), 0.f, 1.f, scale, offset);

  const int32_t sx0 = std::clamp(g_scissor.x, 0, static_cast<int32_t>(target.width));
  const int32_t sy0 = std::clamp(g_scissor.y, 0, static_cast<int32_t>(target.height));
  const int32_t sx1 = std::clamp(g_scissor.x + g_scissor.width, sx0, static_cast<int32_t>(target.width));
  const int32_t sy1 = std::clamp(g_scissor.y + g_scissor.height, sy0, static_cast<int32_t>(target.height));
  rsxSetScissor(g_ctx, sx0, sy0, sx1 - sx0, sy1 - sy0);
}

void clear_rect(const gfx::ClipRect& rect, bool color, bool alpha, bool depth, Vec4<float> clearColor,
                float clearDepth) noexcept {
  const auto& target = current_target();
  const int32_t x0 = std::clamp(rect.x, 0, static_cast<int32_t>(target.width));
  const int32_t y0 = std::clamp(rect.y, 0, static_cast<int32_t>(target.height));
  const int32_t x1 = std::clamp(rect.x + rect.width, x0, static_cast<int32_t>(target.width));
  const int32_t y1 = std::clamp(rect.y + rect.height, y0, static_cast<int32_t>(target.height));
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  rsxSetScissor(g_ctx, x0, y0, x1 - x0, y1 - y0);
  const auto to8 = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
  rsxSetClearColor(g_ctx, (to8(clearColor.w()) << 24) | (to8(clearColor.x()) << 16) | (to8(clearColor.y()) << 8) |
                              to8(clearColor.z()));
  // GX distance terms: aurora hands over reversed-Z values (1 = near); the RSX EFB uses 0 = near.
  const float depth01 = gx::UseReversedZ ? 1.f - clearDepth : clearDepth;
  const auto depth24 = static_cast<uint32_t>(std::lround(std::clamp(depth01, 0.f, 1.f) * 16777215.f));
  rsxSetClearDepthStencil(g_ctx, depth24 << 8);
  uint32_t mask = 0;
  if (color) {
    mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B;
  }
  if (alpha) {
    mask |= GCM_CLEAR_A;
  }
  if (depth && target.hasDepth) {
    mask |= GCM_CLEAR_Z | GCM_CLEAR_S;
  }
  if (mask != 0) {
    prepare_clear();
    rsxClearSurface(g_ctx, mask);
  }
}
} // namespace aurora::rsx
