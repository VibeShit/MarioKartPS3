// RSX device: memory, render targets, frame pacing and presentation.

#include "rsx_backend.hpp"

#include "internal.hpp"

#include <rsx/nv40.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>

#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

namespace aurora::rsx {
gcmContextData* g_ctx = nullptr;

namespace {
Module Log("aurora::rsx");

constexpr uint32_t kCommandBufferSize = 0x200000;
// Command buffer followed by two halves of streaming memory (vertices, fragment programs).
constexpr uint32_t kHostSize = 32u * 1024u * 1024u;
constexpr uint32_t kDisplayBufferCount = 2;
constexpr uint8_t kLabelFrame = 200;
constexpr uint8_t kLabelIdle = 201;
// Internal EFB resolution never exceeds this; CPU vertex processing does not scale with it, but
// fill rate and memory do.
constexpr uint32_t kMaxEfbWidth = 1280;
constexpr uint32_t kMaxEfbHeight = 720;

void* g_hostMemory = nullptr;
uint8_t* g_streamBase = nullptr;
uint32_t g_streamHalfSize = 0;
uint32_t g_streamPos = 0;

uint32_t g_frame = 1;
uint32_t g_streamGeneration = 1;
uint32_t g_idleSequence = 0;

uint32_t g_displayWidth = 0;
uint32_t g_displayHeight = 0;
uint32_t g_displayPitch = 0;
std::array<uint32_t, kDisplayBufferCount> g_displayOffsets{};
uint32_t g_displayIndex = 0;
bool g_firstFlip = true;

Target g_efb;
Target g_current;
gfx::TextureHandle g_presentTexture;
bool g_textureCacheDirty = true;

std::mutex g_freeMutex;
std::vector<std::pair<uint32_t, void*>> g_pendingFrees;

volatile uint32_t* label(uint8_t index) { return gcmGetLabelAddress(index); }

bool label_reached(uint8_t index, uint32_t value) {
  return static_cast<int32_t>(*label(index) - value) >= 0;
}

void wait_label(uint8_t index, uint32_t value) {
  while (!label_reached(index, value)) {
    usleep(30);
  }
}

uint32_t stream_half_offset(uint32_t frame) { return (frame & 1u) * g_streamHalfSize; }

void process_frees(bool all) {
  std::lock_guard lock(g_freeMutex);
  auto it = std::remove_if(g_pendingFrees.begin(), g_pendingFrees.end(), [all](const auto& entry) {
    if (all || label_reached(kLabelFrame, entry.first)) {
      rsxFree(entry.second);
      return true;
    }
    return false;
  });
  g_pendingFrees.erase(it, g_pendingFrees.end());
}

uint32_t align_up(uint32_t value, uint32_t align) { return (value + align - 1) & ~(align - 1); }

void set_default_state() {
  rsxSetColorMask(g_ctx, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
  rsxSetColorMaskMrt(g_ctx, 0);
  rsxSetShadeModel(g_ctx, GCM_SHADE_MODEL_SMOOTH);
  rsxSetFrontFace(g_ctx, GCM_FRONTFACE_CCW);
  rsxSetCullFaceEnable(g_ctx, GCM_FALSE);
  rsxSetDepthTestEnable(g_ctx, GCM_FALSE);
  rsxSetDepthWriteEnable(g_ctx, GCM_FALSE);
  rsxSetBlendEnable(g_ctx, GCM_FALSE);
  rsxSetLogicOpEnable(g_ctx, GCM_FALSE);
  rsxSetAlphaTestEnable(g_ctx, GCM_FALSE);
  rsxSetStencilTestEnable(g_ctx, GCM_FALSE);
  rsxSetDitherEnable(g_ctx, GCM_FALSE);
  rsxSetZMinMaxControl(g_ctx, GCM_TRUE, GCM_FALSE, GCM_FALSE);
}

Target make_target(uint32_t width, uint32_t height, bool depth) {
  Target target;
  target.width = width;
  target.height = height;
  target.colorPitch = align_up(width * 4, 64);
  void* color = local_alloc(target.colorPitch * height, 4096);
  target.colorOffset = local_offset(color);
  if (depth) {
    target.depthPitch = target.colorPitch;
    void* zeta = local_alloc(target.depthPitch * height, 4096);
    target.depthOffset = local_offset(zeta);
    target.hasDepth = true;
  }
  return target;
}
} // namespace

void* local_alloc(uint32_t size, uint32_t align) noexcept {
  void* ptr = rsxMemalign(std::max<uint32_t>(align, 64), std::max<uint32_t>(size, 64));
  if (ptr == nullptr) {
    // Reclaim everything the GPU no longer needs and retry once.
    wait_idle();
    process_frees(true);
    ptr = rsxMemalign(std::max<uint32_t>(align, 64), std::max<uint32_t>(size, 64));
  }
  if (ptr == nullptr) {
    Log.fatal("RSX local memory exhausted allocating {} bytes", size);
  }
  return ptr;
}

void local_free(void* ptr) noexcept {
  if (ptr == nullptr) {
    return;
  }
  std::lock_guard lock(g_freeMutex);
  g_pendingFrees.emplace_back(g_frame, ptr);
}

uint32_t local_offset(const void* ptr) noexcept {
  uint32_t offset = 0;
  rsxAddressToOffset(const_cast<void*>(ptr), &offset);
  return offset;
}

Stream stream_alloc(uint32_t size, uint32_t align) noexcept {
  uint32_t pos = align_up(g_streamPos, align);
  if (pos + size > g_streamHalfSize) {
    // Out of streaming memory for this frame: drain the GPU and start the half over.
    wait_idle();
    pos = 0;
    ++g_streamGeneration;
    if (size > g_streamHalfSize) {
      Log.fatal("RSX stream allocation of {} bytes exceeds the ring", size);
    }
  }
  g_streamPos = pos + size;
  uint8_t* ptr = g_streamBase + stream_half_offset(g_frame) + pos;
  uint32_t offset = 0;
  rsxAddressToOffset(ptr, &offset);
  return {ptr, offset};
}

void emit_method(uint32_t method, uint32_t value) noexcept {
  // Reserve space through librsx so its command buffer wrap-around still applies.
  rsxSetNopCommand(g_ctx, 2);
  g_ctx->current -= 2;
  g_ctx->current[0] = (1u << 18) | method;
  g_ctx->current[1] = value;
  g_ctx->current += 2;
}

void wait_idle() noexcept {
  ++g_idleSequence;
  rsxSetWriteBackendLabel(g_ctx, kLabelIdle, g_idleSequence);
  rsxFlushBuffer(g_ctx);
  wait_label(kLabelIdle, g_idleSequence);
}

void note_texture_upload() noexcept { g_textureCacheDirty = true; }

void flush_texture_cache() noexcept {
  if (g_textureCacheDirty) {
    rsxInvalidateTextureCache(g_ctx, GCM_INVALIDATE_TEXTURE);
    g_textureCacheDirty = false;
  }
}

bool initialize() noexcept {
  g_hostMemory = memalign(1024 * 1024, kHostSize);
  if (g_hostMemory == nullptr || rsxInit(&g_ctx, kCommandBufferSize, kHostSize, g_hostMemory) != 0) {
    Log.error("rsxInit failed");
    return false;
  }

  videoState state{};
  if (videoGetState(0, 0, &state) != 0 || state.state != 0) {
    Log.error("video output unavailable");
    return false;
  }
  videoResolution resolution{};
  videoGetResolution(state.displayMode.resolution, &resolution);
  videoConfiguration config{};
  config.resolution = state.displayMode.resolution;
  config.format = VIDEO_BUFFER_FORMAT_XRGB;
  config.pitch = resolution.width * 4;
  config.aspect = state.displayMode.aspect;
  videoConfigure(0, &config, nullptr, 0);
  videoGetState(0, 0, &state);
  g_displayWidth = resolution.width;
  g_displayHeight = resolution.height;
  g_displayPitch = resolution.width * 4;
  gcmSetFlipMode(GCM_FLIP_VSYNC);
  for (uint32_t i = 0; i < kDisplayBufferCount; ++i) {
    void* buffer = local_alloc(g_displayPitch * g_displayHeight, 4096);
    g_displayOffsets[i] = local_offset(buffer);
    gcmSetDisplayBuffer(i, g_displayOffsets[i], g_displayPitch, g_displayWidth, g_displayHeight);
  }
  gcmResetFlipStatus();

  g_streamBase = static_cast<uint8_t*>(g_hostMemory) + kCommandBufferSize;
  g_streamHalfSize = ((kHostSize - kCommandBufferSize) / 2) & ~0xFFFu;

  // The EFB keeps the display's aspect ratio; GX's logical viewport is scaled into it.
  const float displayAspect = state.displayMode.aspect == VIDEO_ASPECT_4_3
                                  ? 4.f / 3.f
                                  : static_cast<float>(g_displayWidth) / static_cast<float>(g_displayHeight);
  uint32_t efbHeight = std::min(g_displayHeight, kMaxEfbHeight);
  uint32_t efbWidth = std::min(static_cast<uint32_t>(efbHeight * displayAspect + 0.5f), kMaxEfbWidth);
  efbWidth &= ~1u;
  g_efb = make_target(efbWidth, efbHeight, true);
  Log.info("RSX initialized: display {}x{}, EFB {}x{}", g_displayWidth, g_displayHeight, efbWidth, efbHeight);

  *label(kLabelFrame) = 0;
  *label(kLabelIdle) = 0;
  set_default_state();
  initialize_programs();
  bind_target(g_efb);
  rsxSetClearColor(g_ctx, 0);
  rsxSetClearDepthStencil(g_ctx, 0xFFFFFF00);
  rsxSetScissor(g_ctx, 0, 0, g_efb.width, g_efb.height);
  prepare_clear();
  rsxClearSurface(g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A | GCM_CLEAR_Z | GCM_CLEAR_S);
  rsxFlushBuffer(g_ctx);
  return true;
}

void shutdown() noexcept {
  if (g_ctx == nullptr) {
    return;
  }
  wait_idle();
  g_presentTexture.reset();
  process_frees(true);
  gcmSetWaitFlip(g_ctx);
  rsxFinish(g_ctx, 1);
}

void bind_target(const Target& target) noexcept {
  gcmSurface surface{};
  surface.type = GCM_SURFACE_TYPE_LINEAR;
  surface.antiAlias = GCM_SURFACE_CENTER_1;
  surface.colorFormat = GCM_SURFACE_A8R8G8B8;
  surface.colorTarget = GCM_SURFACE_TARGET_0;
  surface.colorLocation[0] = GCM_LOCATION_RSX;
  surface.colorOffset[0] = target.colorOffset;
  surface.colorPitch[0] = target.colorPitch;
  for (int i = 1; i < GCM_MAX_MRT_COUNT; ++i) {
    surface.colorLocation[i] = GCM_LOCATION_RSX;
    surface.colorOffset[i] = 0;
    surface.colorPitch[i] = 64;
  }
  surface.depthFormat = GCM_SURFACE_ZETA_Z24S8;
  surface.depthLocation = GCM_LOCATION_RSX;
  // Targets without depth borrow the EFB's: copies never enable depth test or writes.
  surface.depthOffset = target.hasDepth ? target.depthOffset : g_efb.depthOffset;
  surface.depthPitch = target.hasDepth ? target.depthPitch : g_efb.depthPitch;
  surface.width = target.width;
  surface.height = target.height;
  surface.x = 0;
  surface.y = 0;
  rsxSetSurface(g_ctx, &surface);
  // rsxSetSurface selects a bottom-left window origin; GX, the EFB copies and the display all
  // address rows top-down, so switch the shader window to a top-left origin.
  emit_method(NV40TCL_SHADER_WINDOW, target.height & 0xFFFu);
  g_current = target;
}

const Target& current_target() noexcept { return g_current; }
const Target& efb_target() noexcept { return g_efb; }

Target texture_target(const gfx::TextureRef& texture) noexcept {
  Target target;
  target.colorOffset = texture.rsxOffset;
  target.colorPitch = texture.rsxPitch;
  target.width = texture.size.width;
  target.height = texture.size.height;
  return target;
}

uint32_t frame_index() noexcept { return g_frame; }
uint32_t stream_generation() noexcept { return g_streamGeneration; }
uint32_t display_width() noexcept { return g_displayWidth; }
uint32_t display_height() noexcept { return g_displayHeight; }

void set_present_texture(gfx::TextureHandle texture) noexcept { g_presentTexture = std::move(texture); }

void begin_frame() noexcept {
  // This frame reuses the streaming half of frame - 2.
  if (g_frame >= 2) {
    wait_label(kLabelFrame, g_frame - 2);
  }
  g_streamPos = 0;
  ++g_streamGeneration;
  process_frees(false);
  bind_target(g_efb);
  apply_viewport_scissor();
}

void end_frame() noexcept {
  const uint32_t back = g_displayIndex;
  Target display;
  display.colorOffset = g_displayOffsets[back];
  display.colorPitch = g_displayPitch;
  display.width = g_displayWidth;
  display.height = g_displayHeight;
  bind_target(display);
  rsxSetScissor(g_ctx, 0, 0, g_displayWidth, g_displayHeight);
  rsxSetClearColor(g_ctx, 0);
  prepare_clear();
  rsxClearSurface(g_ctx, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
  if (g_presentTexture && g_presentTexture->rsxMemory != nullptr) {
    const auto& tex = *g_presentTexture;
    float w = static_cast<float>(g_displayWidth);
    float h = static_cast<float>(g_displayHeight);
    const float aspect = present_aspect();
    if (aspect > 0.f) {
      if (w / h > aspect) {
        w = h * aspect;
      } else {
        h = w / aspect;
      }
    }
    QuadDraw quad;
    quad.texOffset = tex.rsxOffset;
    quad.texPitch = tex.rsxPitch;
    quad.texWidth = tex.size.width;
    quad.texHeight = tex.size.height;
    quad.x0 = (static_cast<float>(g_displayWidth) - w) * 0.5f;
    quad.y0 = (static_cast<float>(g_displayHeight) - h) * 0.5f;
    quad.x1 = quad.x0 + w;
    quad.y1 = quad.y0 + h;
    quad.forceOpaqueAlpha = true;
    draw_quad(quad);
  }

  rsxSetWriteBackendLabel(g_ctx, kLabelFrame, g_frame);
  if (!g_firstFlip) {
    // Keep at most one flip queued.
    while (gcmGetFlipStatus() != 0) {
      usleep(100);
    }
  }
  gcmResetFlipStatus();
  gcmSetFlip(g_ctx, back);
  rsxFlushBuffer(g_ctx);
  gcmSetWaitFlip(g_ctx);
  g_firstFlip = false;
  g_displayIndex = (g_displayIndex + 1) % kDisplayBufferCount;
  ++g_frame;
}
} // namespace aurora::rsx

namespace aurora::gfx {
TextureRef::~TextureRef() { rsx::local_free(rsxMemory); }
} // namespace aurora::gfx
