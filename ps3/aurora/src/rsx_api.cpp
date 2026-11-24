// aurora's public API and window layer on the PS3. There is no window: the display is the
// TV output configured by rsx::initialize, input comes from the SDL3 shim (libpad), and the
// XMB "quit game" request is reported as AURORA_EXIT.

#include "rsx_backend.hpp"

#include "device.hpp"
#include "gamepad.hpp"
#include "gfx/presentation.hpp"
#include "gfx/readback/readback.hpp"
#include "gx/fifo.hpp"
#include "gx/readback.hpp"
#include "gx/frame_interpolation.hpp"
#include "gx/texture.hpp"
#include "input/sdl_input.hpp"
#include "internal.hpp"
#include "window.hpp"

#include <aurora/input.hpp>

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>

#include <SDL3/SDL.h>
#include <sysutil/sysutil.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <vector>

namespace aurora {
AuroraConfig g_config;

namespace {
Module Log("aurora");

std::recursive_mutex g_rendererGpuMutex;
std::vector<AuroraEvent> g_events;
std::atomic_bool g_exitRequested = false;
bool g_initialFrame = true;
bool g_frameActive = false;
float g_lockedAspect = 0.f;
AuroraDisplayMode g_displayMode = AURORA_DISPLAY_MODE_EXCLUSIVE;

void sysutil_callback(u64 status, u64, void*) {
  if (status == SYSUTIL_EXIT_GAME) {
    g_exitRequested = true;
  }
}

// Mirrors window.cpp's process_event for the events the PS3 produces.
void process_event(const SDL_Event& event) {
  if (input::sdl::is_routed_event(event)) {
    input::sdl::dispatch(event);
  }
  switch (event.type) {
  case SDL_EVENT_GAMEPAD_ADDED: {
    const auto instance = gamepad::add_controller(event.gdevice.which);
    if (instance != 0 && instance != static_cast<SDL_JoystickID>(-1)) {
      input::sdl::gamepad_added(instance);
    }
    g_events.push_back(AuroraEvent{.type = AURORA_CONTROLLER_ADDED, .controller = instance});
    break;
  }
  case SDL_EVENT_GAMEPAD_REMOVED:
    input::sdl::gamepad_removed(event.gdevice.which);
    gamepad::remove_controller(event.gdevice.which);
    g_events.push_back(AuroraEvent{.type = AURORA_CONTROLLER_REMOVED, .controller = event.gdevice.which});
    break;
  case SDL_EVENT_QUIT:
    g_events.push_back(AuroraEvent{.type = AURORA_EXIT});
    break;
  default:
    break;
  }
  g_events.push_back(AuroraEvent{.type = AURORA_SDL_EVENT, .sdl = event});
}
} // namespace

std::recursive_mutex& renderer_gpu_mutex() noexcept { return g_rendererGpuMutex; }
void wait_for_frame_worker() noexcept {}
std::chrono::nanoseconds wait_for_frame_worker_sealed() noexcept { return std::chrono::nanoseconds::zero(); }
bool wait_for_frame_worker_for(std::chrono::microseconds) noexcept { return true; }
} // namespace aurora

namespace aurora::rsx {
float present_aspect() noexcept { return g_lockedAspect; }
} // namespace aurora::rsx

namespace aurora::window {
SDL_Window* get_sdl_window() { return nullptr; }
AuroraWindowSize get_window_size() {
  const uint32_t width = rsx::efb_target().width;
  const uint32_t height = rsx::efb_target().height;
  return {
      .width = rsx::display_width(),
      .height = rsx::display_height(),
      .fb_width = width,
      .fb_height = height,
      .native_fb_width = width,
      .native_fb_height = height,
      .scale = 1.f,
  };
}
void set_title(const char*) {}
void set_fullscreen(bool) {}
bool get_fullscreen() { return true; }
void set_display_mode(AuroraDisplayMode mode) { g_displayMode = mode; }
AuroraDisplayMode get_display_mode() { return g_displayMode; }
void set_window_size(uint32_t, uint32_t) {}
void set_window_position(uint32_t, uint32_t) {}
void center_window() {}
void request_frame_buffer_resize() {}
void set_frame_buffer_scale(float) {}
void set_frame_buffer_aspect_fit(bool) {}
void set_force_aspect_16_9(bool) {}
void set_present_surface_fill(bool) {}
void lock_present_aspect_ratio(int width, int height) {
  g_lockedAspect = width > 0 && height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 0.f;
}
void unlock_present_aspect_ratio() { g_lockedAspect = 0.f; }
bool get_present_aspect_ratio(float& aspect) noexcept {
  aspect = g_lockedAspect;
  return g_lockedAspect > 0.f;
}
void set_background_input(bool) {}
} // namespace aurora::window

namespace aurora::gx {
namespace detail {
std::atomic_uint32_t g_frameInterpolationFps{0};
} // namespace detail
void set_frame_interpolation_fps(uint32_t) noexcept {}
void mark_frame_interpolation_replay_unsafe() noexcept {}
bool frame_interpolation_replay_safe() noexcept { return false; }
std::array<gfx::Range, MaxInterpolatedFrames> record_interpolation_draw(const FrameInterpolationDrawIdentity&,
                                                                        const Mat4x4<float>&, uint16_t,
                                                                        const InterpolatedUniformLayout&) noexcept {
  return {};
}
void extend_interpolation_draw(uint16_t) noexcept {}
GXBindGroups build_bind_groups(const ShaderInfo&) noexcept { return {}; }

// No readback streams are offered on the RSX (gfx::readback is not built): captures are refused,
// so none reach the FIFO; the handlers only keep the stream in sync if one does.
namespace readback {
GXBool capture_peeks(AuroraReadbackStream, const AuroraReadbackPeekRequest*, u32, GXAlphaReadMode) { return GX_FALSE; }
GXBool capture_copies(AuroraReadbackStream, const AuroraReadbackCopySrc*, u32) { return GX_FALSE; }
GXBool get(AuroraReadbackStream, AuroraReadbackDest* dests, u32 count) {
  for (u32 i = 0; dests != nullptr && i < count; ++i) {
    dests[i].found = GX_FALSE;
  }
  return GX_FALSE;
}
void handle_capture_peeks(ByteReader& reader) {
  reader.skip(8 + 8);
  const uint32_t count = reader.read<u32>();
  reader.skip(1 + static_cast<size_t>(count) * (8 + 1 + 2 + 2));
}
void handle_capture_copies(ByteReader& reader) {
  reader.skip(8 + 8);
  const uint32_t count = reader.read<u32>();
  reader.skip(static_cast<size_t>(count) * (8 + 8));
}
} // namespace readback
} // namespace aurora::gx

namespace aurora::gfx::readback {
StreamHandle create_stream() { return 0; }
void destroy_stream(StreamHandle) {}
} // namespace aurora::gfx::readback

namespace aurora::webgpu {
bool g_bcTexturesSupported = false;
bool g_textureComponentSwizzleSupported = false;
} // namespace aurora::webgpu

// DualShock rumble goes through the SDL gamepad; there is no device-level haptic.
namespace aurora::device {
bool rumble_available() noexcept { return false; }
void rumble(uint16_t, uint16_t, uint16_t) noexcept {}
} // namespace aurora::device

namespace aurora::imgui {
void set_cursor_managed(bool) noexcept {}
} // namespace aurora::imgui

using namespace aurora;

AuroraInfo aurora_initialize(int, char*[], const AuroraConfig* config) {
  g_config = *config;
  if (g_config.appName == nullptr) {
    g_config.appName = "Aurora";
  }
  if (g_config.userPath == nullptr) {
    g_config.userPath = SDL_GetBasePath();
  }
  if (g_config.cachePath == nullptr) {
    g_config.cachePath = g_config.userPath;
  }
  if (g_config.resourcesPath == nullptr) {
    g_config.resourcesPath = SDL_GetBasePath();
  }
  g_config.msaa = 1;

  AuroraInfo info{};
  info.backend = BACKEND_NULL;
  info.userPath = g_config.userPath;
  info.cachePath = g_config.cachePath;
  if (!SDL_Init(SDL_INIT_GAMEPAD | SDL_INIT_EVENTS) || !rsx::initialize()) {
    info.initializationStatus = AURORA_INITIALIZATION_GRAPHICS_UNAVAILABLE;
    info.initializationError = "RSX initialization failed";
    return info;
  }
  sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, nullptr);
  gx::initialize();
  info.windowSize = window::get_window_size();
  info.initializationStatus = AURORA_INITIALIZATION_SUCCESS;
  Log.info("Aurora initialized (RSX)");
  return info;
}

void aurora_shutdown() {
  gx::shutdown();
  rsx::shutdown();
  sysUtilUnregisterCallback(SYSUTIL_EVENT_SLOT0);
  SDL_Quit();
}

const AuroraEvent* aurora_update() {
  if (g_initialFrame) {
    g_initialFrame = false;
    gamepad::initialize();
  }
  g_events.clear();
  sysUtilCheckCallback();
  SDL_Event event;
  while (SDL_PollEvent(&event)) {
    process_event(event);
  }
  if (g_exitRequested.exchange(false)) {
    g_events.push_back(AuroraEvent{.type = AURORA_EXIT});
  }
  input::reconcile();
  g_events.push_back(AuroraEvent{.type = AURORA_NONE});
  gamepad::update();
  return g_events.data();
}

bool aurora_begin_frame_window() { return true; }

bool aurora_begin_frame_gx() {
  if (!g_frameActive) {
    gx::update();
    std::lock_guard lock(g_rendererGpuMutex);
    rsx::begin_frame();
    gx::fifo::begin_frame();
    gx::flush_texture_destructions();
    g_frameActive = true;
  }
  return true;
}

bool aurora_begin_frame() { return aurora_begin_frame_window() && aurora_begin_frame_gx(); }

AuroraImguiFrame* aurora_end_frame_window() { return nullptr; }
void aurora_release_imgui_frame(AuroraImguiFrame*) {}

void aurora_end_frame_gx(AuroraImguiFrame*) {
  gx::flush_texture_destructions();
  gx::fifo::drain();
  gx::fifo::end_frame();
  gx::texture::end_frame();
  std::lock_guard lock(g_rendererGpuMutex);
  if (!g_frameActive) {
    rsx::begin_frame();
  }
  // A display copy (the XFB) is what reaches the screen; without one the EFB is shown.
  auto& gxState = gx::g_gxState;
  rsx::set_present_texture(gxState.displayCopyProduced ? gxState.displayCopyTexture : gfx::TextureHandle{});
  gxState.displayCopyProduced = false;
  rsx::end_frame();
  g_frameActive = false;
}

void aurora_end_frame() { aurora_end_frame_gx(nullptr); }

// VI black (VISetBlack): frames still end, but show only black like the blanked VI output.
extern "C" void aurora_ps3_set_present_black(bool black) { rsx::set_present_black(black); }

void aurora_set_frame_worker_wait_callback(AuroraFrameWorkerWaitCallback) {}
void aurora_wait_for_frame_worker() {}
bool aurora_wait_for_frame_worker_for(uint32_t) { return true; }
void aurora_set_present_schedule(uint64_t, uint64_t) {}
void aurora_report_producer_paced(bool) {}
void aurora_request_frame_capture(uint32_t, const char*) {}
bool aurora_flush_efb_copies_to_ram() { return true; }
bool aurora_flush_efb_copy_to_ram(void*) { return false; }
void aurora_set_log_level(AuroraLogLevel level) { g_config.logLevel = level; }
void aurora_set_pause_on_focus_lost(bool value) { g_config.pauseOnFocusLost = value; }
void aurora_set_background_input(bool) {}
void aurora_set_display_mode(AuroraDisplayMode mode) { window::set_display_mode(mode); }
AuroraDisplayMode aurora_get_display_mode() { return window::get_display_mode(); }
void aurora_set_metalfx_spatial(bool) {}
bool aurora_get_metalfx_spatial() { return false; }
bool aurora_is_metalfx_spatial_supported() { return false; }
AuroraMetalFXStatus aurora_get_metalfx_status() { return AURORA_METALFX_UNSUPPORTED; }
AuroraBackend aurora_get_backend() { return BACKEND_NULL; }
const AuroraBackend* aurora_get_available_backends(size_t* count) {
  static constexpr AuroraBackend kBackends[] = {BACKEND_NULL};
  if (count != nullptr) {
    *count = 1;
  }
  return kBackends;
}

void aurora_pop_debug_group() {}
void pop_debug_group() {}
void aurora_set_skip_unready_pipelines(bool) {}
const AuroraStats* aurora_get_stats() { return &gfx::g_stats; }

namespace {
AuroraGuestWriteGenerationCallback g_guestWriteGeneration = nullptr;
AuroraGuestWriteNotifyCallback g_guestWriteNotify = nullptr;
} // namespace

namespace aurora::gfx::presentation {
uint64_t guest_write_generation(const void* hostPtr, size_t size) noexcept {
  return g_guestWriteGeneration != nullptr ? g_guestWriteGeneration(hostPtr, size) : AURORA_GUEST_WRITE_UNTRACKED;
}
void notify_guest_write(const void* hostPtr, size_t size) noexcept {
  if (g_guestWriteNotify != nullptr) {
    g_guestWriteNotify(hostPtr, size);
  }
}
} // namespace aurora::gfx::presentation

void aurora_set_guest_write_hooks(AuroraGuestWriteGenerationCallback generation, AuroraGuestWriteNotifyCallback notify) {
  g_guestWriteGeneration = generation;
  g_guestWriteNotify = notify;
}
