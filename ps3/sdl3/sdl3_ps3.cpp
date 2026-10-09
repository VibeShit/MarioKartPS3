// PS3 implementation of the mini SDL3 API declared in include/SDL3/SDL_ps3.h.
//
// Gamepads come from libpad (DualShock 3 and anything the PS3 exposes as a
// standard pad), audio goes out through libaudio's 48 kHz float port fed by a
// dedicated PPU thread, and everything else is a small portable helper.

#include "SDL3/SDL_ps3.h"

#include <io/pad.h>
#include <audio/audio.h>
#include <sys/event_queue.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "ps3_threads.h"

namespace {

thread_local std::string g_error;

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
SDL_LogOutputFunction g_logOutput = nullptr;
void* g_logUserdata = nullptr;
SDL_LogPriority g_logPriority = SDL_LOG_PRIORITY_INFO;

void LogV(int category, SDL_LogPriority priority, const char* fmt, va_list args) {
    if (priority < g_logPriority)
        return;
    char buffer[1024];
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (g_logOutput != nullptr) {
        g_logOutput(g_logUserdata, category, priority, buffer);
    } else {
        std::fprintf(stderr, "[SDL] %s\n", buffer);
    }
}

// ---------------------------------------------------------------------------
// Gamepads
// ---------------------------------------------------------------------------
constexpr int kMaxPorts = MAX_PORT_NUM;

struct PadState {
    bool connected = false;
    bool hasPressure = false;
    bool hasSensors = false;
    bool hasVibrate = false;
    bool sensorsEnabled = false;
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
    Sint16 axes[SDL_GAMEPAD_AXIS_COUNT] = {};
    float accel[3] = {0.0f, SDL_STANDARD_GRAVITY, 0.0f};
    int playerIndex = -1;
    Uint64 rumbleUntil = 0;
    bool rumbling = false;
    std::string serial;
};

}  // namespace

struct SDL_Joystick {
    int port;
};
struct SDL_Gamepad {
    int port;
    SDL_Joystick joystick;
};

namespace {

PadState g_pads[kMaxPorts];
SDL_Gamepad g_gamepadHandles[kMaxPorts];
bool g_padInitialized = false;
std::deque<SDL_Event> g_events;
std::mutex g_eventMutex;

inline SDL_JoystickID PortToId(int port) { return static_cast<SDL_JoystickID>(port + 1); }
inline int IdToPort(SDL_JoystickID id) {
    const int port = static_cast<int>(id) - 1;
    return port >= 0 && port < kMaxPorts ? port : -1;
}

void PushEventLocked(const SDL_Event& event) {
    if (g_events.size() < 256)
        g_events.push_back(event);
}

Sint16 StickAxis(unsigned value) {
    // 0x00..0xFF with 0x80 as the centre.
    const int centred = static_cast<int>(value & 0xFFu) - 128;
    const int scaled = centred >= 0 ? centred * 32767 / 127 : centred * 32768 / 128;
    return static_cast<Sint16>(std::clamp(scaled, -32768, 32767));
}

Sint16 TriggerAxis(unsigned pressure) { return static_cast<Sint16>((pressure & 0xFFu) * 32767 / 255); }

void EnsurePadInit() {
    if (g_padInitialized)
        return;
    ioPadInit(kMaxPorts);
    for (int port = 0; port < kMaxPorts; ++port)
        g_gamepadHandles[port] = SDL_Gamepad{port, SDL_Joystick{port}};
    g_padInitialized = true;
}

void StopRumble(int port) {
    padActParam param{};
    ioPadSetActDirect(port, &param);
    g_pads[port].rumbling = false;
}

void PollPads() {
    EnsurePadInit();
    padInfo2 info{};
    if (ioPadGetInfo2(&info) != 0)
        return;

    const Uint64 now = SDL_GetTicks();
    for (int port = 0; port < kMaxPorts; ++port) {
        PadState& pad = g_pads[port];
        const bool connected = (info.port_status[port] & 1u) != 0;
        if (connected != pad.connected) {
            SDL_Event event{};
            event.gdevice.timestamp = SDL_GetTicksNS();
            event.gdevice.which = PortToId(port);
            if (connected) {
                pad = PadState{};
                pad.connected = true;
                const u32 caps = info.device_capability[port];
                pad.hasPressure = (caps & 2u) != 0;
                pad.hasSensors = (caps & 4u) != 0;
                pad.hasVibrate = (caps & 16u) != 0;
                if (pad.hasPressure)
                    ioPadSetPressMode(port, PAD_PRESS_MODE_ON);
                char serial[32];
                std::snprintf(serial, sizeof(serial), "ps3-port-%d", port);
                pad.serial = serial;
                event.type = SDL_EVENT_JOYSTICK_ADDED;
                PushEventLocked(event);
                event.type = SDL_EVENT_GAMEPAD_ADDED;
                PushEventLocked(event);
            } else {
                pad = PadState{};
                event.type = SDL_EVENT_GAMEPAD_REMOVED;
                PushEventLocked(event);
                event.type = SDL_EVENT_JOYSTICK_REMOVED;
                PushEventLocked(event);
                continue;
            }
        }
        if (!connected)
            continue;

        if (pad.rumbling && now >= pad.rumbleUntil)
            StopRumble(port);

        padData data{};
        if (ioPadGetData(port, &data) != 0 || data.len == 0)
            continue;  // No new report: keep the previous state.

        auto& b = pad.buttons;
        b[SDL_GAMEPAD_BUTTON_SOUTH] = data.BTN_CROSS;
        b[SDL_GAMEPAD_BUTTON_EAST] = data.BTN_CIRCLE;
        b[SDL_GAMEPAD_BUTTON_WEST] = data.BTN_SQUARE;
        b[SDL_GAMEPAD_BUTTON_NORTH] = data.BTN_TRIANGLE;
        b[SDL_GAMEPAD_BUTTON_BACK] = data.BTN_SELECT;
        b[SDL_GAMEPAD_BUTTON_START] = data.BTN_START;
        b[SDL_GAMEPAD_BUTTON_LEFT_STICK] = data.BTN_L3;
        b[SDL_GAMEPAD_BUTTON_RIGHT_STICK] = data.BTN_R3;
        b[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = data.BTN_L1;
        b[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = data.BTN_R1;
        b[SDL_GAMEPAD_BUTTON_DPAD_UP] = data.BTN_UP;
        b[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = data.BTN_DOWN;
        b[SDL_GAMEPAD_BUTTON_DPAD_LEFT] = data.BTN_LEFT;
        b[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = data.BTN_RIGHT;

        auto& a = pad.axes;
        a[SDL_GAMEPAD_AXIS_LEFTX] = StickAxis(data.ANA_L_H);
        a[SDL_GAMEPAD_AXIS_LEFTY] = StickAxis(data.ANA_L_V);
        a[SDL_GAMEPAD_AXIS_RIGHTX] = StickAxis(data.ANA_R_H);
        a[SDL_GAMEPAD_AXIS_RIGHTY] = StickAxis(data.ANA_R_V);
        if (pad.hasPressure && data.len >= 24) {
            a[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = TriggerAxis(data.PRE_L2);
            a[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = TriggerAxis(data.PRE_R2);
        } else {
            a[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = data.BTN_L2 ? 32767 : 0;
            a[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = data.BTN_R2 ? 32767 : 0;
        }
        if (pad.sensorsEnabled && data.len >= 28) {
            // 0..1023 with 512 at rest and roughly 113 counts per g.
            constexpr float kCountsPerG = 113.0f;
            pad.accel[0] = (static_cast<float>(data.SENSOR_X) - 512.0f) / kCountsPerG * SDL_STANDARD_GRAVITY;
            pad.accel[1] = -(static_cast<float>(data.SENSOR_Y) - 512.0f) / kCountsPerG * SDL_STANDARD_GRAVITY;
            pad.accel[2] = (static_cast<float>(data.SENSOR_Z) - 512.0f) / kCountsPerG * SDL_STANDARD_GRAVITY;
        }
    }
}

const char* const kButtonNames[SDL_GAMEPAD_BUTTON_COUNT] = {
    "a", "b", "x", "y", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpup", "dpdown", "dpleft", "dpright", "misc1", "paddle1", "paddle2", "paddle3",
    "paddle4", "touchpad", "misc2", "misc3", "misc4", "misc5", "misc6"};
const char* const kAxisNames[SDL_GAMEPAD_AXIS_COUNT] = {"leftx", "lefty", "rightx", "righty", "lefttrigger",
                                                        "righttrigger"};

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
constexpr int kOutputRate = 48000;

}  // namespace

struct SDL_AudioStream {
    SDL_AudioSpec spec{};
    std::mutex mutex;
    std::vector<float> queue;  // interleaved stereo at the source rate
    size_t readPos = 0;
    double phase = 0.0;
    float gain = 1.0f;
    std::atomic<bool> paused{true};
    std::atomic<bool> running{false};
    uint64_t thread = 0;
    u32 port = 0;
    sys_event_queue_t eventQueue = 0;
    sys_ipc_key_t queueKey = 0;
    audioPortConfig config{};
};

namespace {

void AudioThread(void* arg) {
    auto* stream = static_cast<SDL_AudioStream*>(arg);
    const double step = static_cast<double>(stream->spec.freq) / kOutputRate;
    while (stream->running.load(std::memory_order_acquire)) {
        sys_event_t event;
        if (sysEventQueueReceive(stream->eventQueue, &event, 20 * 1000) != 0)
            continue;
        const u64 current = *reinterpret_cast<volatile u64*>(static_cast<uintptr_t>(stream->config.readIndex));
        const u32 block = static_cast<u32>((current + 1) % stream->config.numBlocks);
        float* out = reinterpret_cast<float*>(static_cast<uintptr_t>(stream->config.audioDataStart)) +
                     block * AUDIO_BLOCK_SAMPLES * 2;

        std::lock_guard lock(stream->mutex);
        const size_t frames = (stream->queue.size() - stream->readPos) / 2;
        const float gain = stream->paused.load() ? 0.0f : stream->gain;
        for (int i = 0; i < AUDIO_BLOCK_SAMPLES; ++i) {
            const size_t index = static_cast<size_t>(stream->phase);
            if (index + 1 < frames) {
                const float t = static_cast<float>(stream->phase - static_cast<double>(index));
                const float* f0 = &stream->queue[stream->readPos + index * 2];
                const float* f1 = f0 + 2;
                out[i * 2] = (f0[0] + (f1[0] - f0[0]) * t) * gain;
                out[i * 2 + 1] = (f0[1] + (f1[1] - f0[1]) * t) * gain;
                stream->phase += step;
            } else {
                out[i * 2] = 0.0f;
                out[i * 2 + 1] = 0.0f;
            }
        }
        const size_t consumed = std::min(static_cast<size_t>(stream->phase), frames > 0 ? frames - 1 : 0);
        stream->readPos += consumed * 2;
        stream->phase -= static_cast<double>(consumed);
        if (stream->readPos > 65536) {
            stream->queue.erase(stream->queue.begin(), stream->queue.begin() + static_cast<long>(stream->readPos));
            stream->readPos = 0;
        }
    }
}

int BytesPerSample(SDL_AudioFormat format) { return (static_cast<int>(format) & 0xFF) / 8; }

}  // namespace

extern "C" {

// ---- stdinc ---------------------------------------------------------------
void* SDL_malloc(size_t size) { return std::malloc(size); }
void* SDL_calloc(size_t nmemb, size_t size) { return std::calloc(nmemb, size); }
void* SDL_realloc(void* mem, size_t size) { return std::realloc(mem, size); }
void SDL_free(void* mem) { std::free(mem); }
char* SDL_strdup(const char* str) { return str != nullptr ? strdup(str) : nullptr; }
char* SDL_strstr(const char* haystack, const char* needle) { return const_cast<char*>(std::strstr(haystack, needle)); }
size_t SDL_strlcpy(char* dst, const char* src, size_t maxlen) {
    const size_t len = std::strlen(src);
    if (maxlen > 0) {
        const size_t copy = std::min(len, maxlen - 1);
        std::memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}
int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int result = vsnprintf(text, maxlen, fmt, args);
    va_end(args);
    return result;
}
const char* SDL_GetPlatform(void) { return "PlayStation 3"; }

// ---- error ----------------------------------------------------------------
const char* SDL_GetError(void) { return g_error.c_str(); }
bool SDL_SetError(const char* fmt, ...) {
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_error = buffer;
    return false;
}
bool SDL_ClearError(void) {
    g_error.clear();
    return true;
}

// ---- init -----------------------------------------------------------------
static SDL_InitFlags g_initFlags = 0;
bool SDL_Init(SDL_InitFlags flags) { return SDL_InitSubSystem(flags); }
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (flags & (SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD | SDL_INIT_SENSOR | SDL_INIT_HAPTIC))
        EnsurePadInit();
    g_initFlags |= flags;
    return true;
}
void SDL_QuitSubSystem(SDL_InitFlags flags) { g_initFlags &= ~flags; }
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return flags == 0 ? g_initFlags : (g_initFlags & flags); }
void SDL_Quit(void) {
    if (g_padInitialized) {
        ioPadEnd();
        g_padInitialized = false;
    }
    g_initFlags = 0;
}

// ---- hints ----------------------------------------------------------------
bool SDL_SetHint(const char*, const char*) { return true; }
const char* SDL_GetHint(const char*) { return nullptr; }

// ---- log ------------------------------------------------------------------
void SDL_SetLogPriority(int, SDL_LogPriority priority) { g_logPriority = priority; }
void SDL_SetLogPriorities(SDL_LogPriority priority) { g_logPriority = priority; }
void SDL_SetLogOutputFunction(SDL_LogOutputFunction callback, void* userdata) {
    g_logOutput = callback;
    g_logUserdata = userdata;
}
#define MKW_SDL_LOG_FN(name, priority)                                                                     \
    void name(int category, const char* fmt, ...) {                                                        \
        va_list args;                                                                                      \
        va_start(args, fmt);                                                                               \
        LogV(category, priority, fmt, args);                                                               \
        va_end(args);                                                                                      \
    }
MKW_SDL_LOG_FN(SDL_LogInfo, SDL_LOG_PRIORITY_INFO)
MKW_SDL_LOG_FN(SDL_LogWarn, SDL_LOG_PRIORITY_WARN)
MKW_SDL_LOG_FN(SDL_LogError, SDL_LOG_PRIORITY_ERROR)
MKW_SDL_LOG_FN(SDL_LogDebug, SDL_LOG_PRIORITY_DEBUG)
#undef MKW_SDL_LOG_FN
void SDL_Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogV(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO, fmt, args);
    va_end(args);
}

// ---- timer ----------------------------------------------------------------
Uint64 SDL_GetPerformanceFrequency(void) { return sysGetTimebaseFrequency(); }
Uint64 SDL_GetPerformanceCounter(void) {
    Uint64 tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return tb;
}
Uint64 SDL_GetTicksNS(void) {
    static const Uint64 start = SDL_GetPerformanceCounter();
    const Uint64 elapsed = SDL_GetPerformanceCounter() - start;
    return static_cast<Uint64>((static_cast<unsigned __int128>(elapsed) * 1000000000u) /
                               SDL_GetPerformanceFrequency());
}
Uint64 SDL_GetTicks(void) { return SDL_GetTicksNS() / 1000000u; }
void SDL_Delay(Uint32 ms) { ps3compat::SleepMicros(static_cast<uint64_t>(ms) * 1000u); }
void SDL_DelayNS(Uint64 ns) { ps3compat::SleepMicros(ns / 1000u); }

// ---- guid / properties ----------------------------------------------------
void SDL_GUIDToString(SDL_GUID guid, char* pszGUID, int cbGUID) {
    static const char kHex[] = "0123456789abcdef";
    int out = 0;
    for (int i = 0; i < 16 && out + 2 < cbGUID; ++i) {
        pszGUID[out++] = kHex[guid.data[i] >> 4];
        pszGUID[out++] = kHex[guid.data[i] & 15];
    }
    if (cbGUID > 0)
        pszGUID[std::min(out, cbGUID - 1)] = '\0';
}
SDL_GUID SDL_StringToGUID(const char* pchGUID) {
    SDL_GUID guid{};
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    for (int i = 0; i < 16 && pchGUID[i * 2] != '\0' && pchGUID[i * 2 + 1] != '\0'; ++i)
        guid.data[i] = static_cast<Uint8>((nibble(pchGUID[i * 2]) << 4) | nibble(pchGUID[i * 2 + 1]));
    return guid;
}
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool default_value) {
    const int port = static_cast<int>(props) - 1;
    if (port < 0 || port >= kMaxPorts || name == nullptr)
        return default_value;
    if (std::strcmp(name, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN) == 0)
        return g_pads[port].hasVibrate;
    if (std::strcmp(name, SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN) == 0 ||
        std::strcmp(name, SDL_PROP_GAMEPAD_CAP_MONO_LED_BOOLEAN) == 0)
        return false;
    return default_value;
}

// ---- joystick -------------------------------------------------------------
SDL_JoystickID* SDL_GetJoysticks(int* count) { return SDL_GetGamepads(count); }
SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID id) {
    const int port = IdToPort(id);
    return port >= 0 && g_pads[port].connected ? &g_gamepadHandles[port].joystick : nullptr;
}
void SDL_CloseJoystick(SDL_Joystick*) {}
SDL_JoystickID SDL_GetJoystickID(SDL_Joystick* joystick) { return joystick ? PortToId(joystick->port) : 0; }
const char* SDL_GetJoystickNameForID(SDL_JoystickID) { return "PS3 Controller"; }
SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID id) {
    SDL_GUID guid{};
    guid.data[0] = 0x03;  // USB bus, Sony DualShock 3 vendor/product
    guid.data[4] = 0x4c;
    guid.data[5] = 0x05;
    guid.data[8] = 0x68;
    guid.data[9] = 0x02;
    guid.data[15] = static_cast<Uint8>(id);
    return guid;
}
void SDL_GetJoystickGUIDInfo(SDL_GUID guid, Uint16* vendor, Uint16* product, Uint16* version, Uint16* crc16) {
    if (vendor) *vendor = static_cast<Uint16>(guid.data[4] | (guid.data[5] << 8));
    if (product) *product = static_cast<Uint16>(guid.data[8] | (guid.data[9] << 8));
    if (version) *version = 0;
    if (crc16) *crc16 = 0;
}
int SDL_GetNumJoystickAxes(SDL_Joystick*) { return SDL_GAMEPAD_AXIS_COUNT; }
int SDL_GetNumJoystickButtons(SDL_Joystick*) { return SDL_GAMEPAD_BUTTON_COUNT; }
Sint16 SDL_GetJoystickAxis(SDL_Joystick* joystick, int axis) {
    if (!joystick || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    return g_pads[joystick->port].axes[axis];
}
bool SDL_GetJoystickButton(SDL_Joystick* joystick, int button) {
    if (!joystick || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    return g_pads[joystick->port].buttons[button];
}
SDL_PowerState SDL_GetJoystickPowerInfo(SDL_Joystick*, int* percent) {
    if (percent) *percent = -1;
    return SDL_POWERSTATE_UNKNOWN;
}

// ---- gamepad --------------------------------------------------------------
SDL_JoystickID* SDL_GetGamepads(int* count) {
    PollPads();
    auto* ids = static_cast<SDL_JoystickID*>(std::calloc(kMaxPorts + 1, sizeof(SDL_JoystickID)));
    int n = 0;
    for (int port = 0; port < kMaxPorts; ++port)
        if (g_pads[port].connected)
            ids[n++] = PortToId(port);
    if (count) *count = n;
    return ids;
}
bool SDL_IsGamepad(SDL_JoystickID id) { return IdToPort(id) >= 0; }
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID id) {
    const int port = IdToPort(id);
    if (port < 0 || !g_pads[port].connected) {
        SDL_SetError("Gamepad %u is not connected", static_cast<unsigned>(id));
        return nullptr;
    }
    return &g_gamepadHandles[port];
}
void SDL_CloseGamepad(SDL_Gamepad*) {}
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID id) { return SDL_OpenGamepad(id); }
SDL_Gamepad* SDL_GetGamepadFromPlayerIndex(int playerIndex) {
    for (int port = 0; port < kMaxPorts; ++port)
        if (g_pads[port].connected && g_pads[port].playerIndex == playerIndex)
            return &g_gamepadHandles[port];
    return nullptr;
}
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* gamepad) { return gamepad ? PortToId(gamepad->port) : 0; }
SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad* gamepad) { return gamepad ? &gamepad->joystick : nullptr; }
const char* SDL_GetGamepadName(SDL_Gamepad*) { return "PS3 Controller"; }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_PS3; }
Uint16 SDL_GetGamepadVendor(SDL_Gamepad*) { return 0x054c; }
Uint16 SDL_GetGamepadProduct(SDL_Gamepad*) { return 0x0268; }
const char* SDL_GetGamepadSerial(SDL_Gamepad* gamepad) {
    return gamepad ? g_pads[gamepad->port].serial.c_str() : nullptr;
}
SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID id) { return SDL_GetJoystickGUIDForID(id); }
char* SDL_GetGamepadMappingForID(SDL_JoystickID) { return SDL_strdup("ps3,PS3 Controller,platform:PS3"); }
int SDL_AddGamepadMapping(const char*) { return 0; }
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad* gamepad) {
    return gamepad ? static_cast<SDL_PropertiesID>(gamepad->port + 1) : 0;
}
int SDL_GetGamepadPlayerIndex(SDL_Gamepad* gamepad) { return gamepad ? g_pads[gamepad->port].playerIndex : -1; }
bool SDL_SetGamepadPlayerIndex(SDL_Gamepad* gamepad, int playerIndex) {
    if (!gamepad) return false;
    g_pads[gamepad->port].playerIndex = playerIndex;
    return true;
}
SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad*, int* percent) {
    if (percent) *percent = -1;
    return SDL_POWERSTATE_UNKNOWN;
}
bool SDL_GetGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button) {
    if (!gamepad || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
    return g_pads[gamepad->port].buttons[button];
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis) {
    if (!gamepad || axis < 0 || axis >= SDL_GAMEPAD_AXIS_COUNT) return 0;
    return g_pads[gamepad->port].axes[axis];
}
bool SDL_RumbleGamepad(SDL_Gamepad* gamepad, Uint16 low, Uint16 high, Uint32 durationMs) {
    if (!gamepad || !g_pads[gamepad->port].hasVibrate) return false;
    padActParam param{};
    param.small_motor = high > 0x4000 ? 1 : 0;
    param.large_motor = static_cast<u8>(low >> 8);
    ioPadSetActDirect(gamepad->port, &param);
    PadState& pad = g_pads[gamepad->port];
    pad.rumbling = low != 0 || high != 0;
    pad.rumbleUntil = SDL_GetTicks() + durationMs;
    return true;
}
bool SDL_SetGamepadLED(SDL_Gamepad*, Uint8, Uint8, Uint8) { return false; }
bool SDL_GamepadHasSensor(SDL_Gamepad* gamepad, SDL_SensorType type) {
    return gamepad && type == SDL_SENSOR_ACCEL && g_pads[gamepad->port].hasSensors;
}
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type, bool enabled) {
    if (!SDL_GamepadHasSensor(gamepad, type)) return false;
    ioPadSetSensorMode(gamepad->port, enabled ? PAD_SENSOR_MODE_ON : PAD_SENSOR_MODE_OFF);
    g_pads[gamepad->port].sensorsEnabled = enabled;
    return true;
}
bool SDL_GamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type) {
    return SDL_GamepadHasSensor(gamepad, type) && g_pads[gamepad->port].sensorsEnabled;
}
bool SDL_GetGamepadSensorData(SDL_Gamepad* gamepad, SDL_SensorType type, float* data, int numValues) {
    if (!SDL_GamepadSensorEnabled(gamepad, type) || data == nullptr) return false;
    for (int i = 0; i < numValues && i < 3; ++i)
        data[i] = g_pads[gamepad->port].accel[i];
    return true;
}
const char* SDL_GetGamepadStringForButton(SDL_GamepadButton button) {
    return button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT ? kButtonNames[button] : nullptr;
}
const char* SDL_GetGamepadStringForAxis(SDL_GamepadAxis axis) {
    return axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT ? kAxisNames[axis] : nullptr;
}
void SDL_UpdateGamepads(void) {
    std::lock_guard lock(g_eventMutex);
    PollPads();
}

// ---- keyboard / mouse -----------------------------------------------------
const bool* SDL_GetKeyboardState(int* numkeys) {
    static bool keys[SDL_SCANCODE_COUNT] = {};
    if (numkeys) *numkeys = SDL_SCANCODE_COUNT;
    return keys;
}
SDL_Window* SDL_GetKeyboardFocus(void) { return nullptr; }
const char* SDL_GetScancodeName(SDL_Scancode) { return ""; }
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y) {
    if (x) *x = 0.0f;
    if (y) *y = 0.0f;
    return 0;
}
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

// ---- events ---------------------------------------------------------------
void SDL_PumpEvents(void) {
    std::lock_guard lock(g_eventMutex);
    PollPads();
}
bool SDL_PollEvent(SDL_Event* event) {
    std::lock_guard lock(g_eventMutex);
    if (g_events.empty())
        PollPads();
    if (g_events.empty())
        return false;
    if (event) *event = g_events.front();
    g_events.pop_front();
    return true;
}
bool SDL_PushEvent(SDL_Event* event) {
    if (!event) return false;
    std::lock_guard lock(g_eventMutex);
    PushEventLocked(*event);
    return true;
}

// ---- audio ----------------------------------------------------------------
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID, const SDL_AudioSpec* spec, SDL_AudioStreamCallback,
                                           void*) {
    if (spec == nullptr || spec->channels < 1 || spec->channels > 2 || spec->freq <= 0) {
        SDL_SetError("unsupported audio spec");
        return nullptr;
    }
    static bool audioInitialized = false;
    if (!audioInitialized) {
        if (audioInit() != 0) {
            SDL_SetError("audioInit failed");
            return nullptr;
        }
        audioInitialized = true;
    }
    auto* stream = new SDL_AudioStream();
    stream->spec = *spec;
    audioPortParam params{};
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = 0;
    params.level = 1.0f;
    if (audioPortOpen(&params, &stream->port) != 0 || audioGetPortConfig(stream->port, &stream->config) != 0) {
        delete stream;
        SDL_SetError("audioPortOpen failed");
        return nullptr;
    }
    if (audioCreateNotifyEventQueue(&stream->eventQueue, &stream->queueKey) != 0 ||
        audioSetNotifyEventQueue(stream->queueKey) != 0) {
        audioPortClose(stream->port);
        delete stream;
        SDL_SetError("audio event queue setup failed");
        return nullptr;
    }
    sysEventQueueDrain(stream->eventQueue);
    audioPortStart(stream->port);
    stream->running.store(true);
    stream->thread = ps3compat::StartThread(AudioThread, stream, 64 * 1024, "sdl_audio");
    return stream;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream) {
    if (!stream) return false;
    stream->paused.store(false);
    return true;
}
bool SDL_PauseAudioStreamDevice(SDL_AudioStream* stream) {
    if (!stream) return false;
    stream->paused.store(true);
    return true;
}
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len) {
    if (!stream || !buf || len < 0) return false;
    // The runtime hands over host-native samples (it labels them S16LE because
    // its desktop hosts are little-endian); treat every S16 format as native.
    const int bytesPerSample = BytesPerSample(stream->spec.format);
    const int channels = stream->spec.channels;
    const int frames = len / (bytesPerSample * channels);
    std::lock_guard lock(stream->mutex);
    stream->queue.reserve(stream->queue.size() + static_cast<size_t>(frames) * 2);
    for (int frame = 0; frame < frames; ++frame) {
        float sample[2] = {0.0f, 0.0f};
        for (int ch = 0; ch < channels; ++ch) {
            const int index = frame * channels + ch;
            if (bytesPerSample == 2) {
                sample[ch] = static_cast<const int16_t*>(buf)[index] / 32768.0f;
            } else if (bytesPerSample == 4 && (stream->spec.format & 0x100)) {
                sample[ch] = static_cast<const float*>(buf)[index];
            } else if (bytesPerSample == 4) {
                sample[ch] = static_cast<const int32_t*>(buf)[index] / 2147483648.0f;
            } else {
                sample[ch] = (static_cast<const uint8_t*>(buf)[index] - 128) / 128.0f;
            }
        }
        if (channels == 1) sample[1] = sample[0];
        stream->queue.push_back(sample[0]);
        stream->queue.push_back(sample[1]);
    }
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream* stream) {
    if (!stream) return -1;
    std::lock_guard lock(stream->mutex);
    const size_t frames = (stream->queue.size() - stream->readPos) / 2;
    return static_cast<int>(frames * static_cast<size_t>(stream->spec.channels * BytesPerSample(stream->spec.format)));
}
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream) { return SDL_GetAudioStreamQueued(stream); }
bool SDL_ClearAudioStream(SDL_AudioStream* stream) {
    if (!stream) return false;
    std::lock_guard lock(stream->mutex);
    stream->queue.clear();
    stream->readPos = 0;
    stream->phase = 0.0;
    return true;
}
bool SDL_SetAudioStreamGain(SDL_AudioStream* stream, float gain) {
    if (!stream) return false;
    std::lock_guard lock(stream->mutex);
    stream->gain = gain;
    return true;
}
float SDL_GetAudioStreamGain(SDL_AudioStream* stream) { return stream ? stream->gain : -1.0f; }
void SDL_DestroyAudioStream(SDL_AudioStream* stream) {
    if (!stream) return;
    stream->running.store(false, std::memory_order_release);
    if (stream->thread != 0)
        ps3compat::JoinThread(stream->thread);
    audioPortStop(stream->port);
    audioRemoveNotifyEventQueue(stream->queueKey);
    audioPortClose(stream->port);
    sysEventQueueDestroy(stream->eventQueue, 0);
    delete stream;
}

}  // extern "C"

// ---- iostream / filesystem ---------------------------------------------------
struct SDL_IOStream {
    FILE* file;
};

namespace {
template <typename T>
bool ReadLE(SDL_IOStream* src, T* value) {
    uint8_t bytes[sizeof(T)];
    if (SDL_ReadIO(src, bytes, sizeof(T)) != sizeof(T)) return false;
    T result = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        result |= static_cast<T>(static_cast<T>(bytes[i]) << (8 * i));
    *value = result;
    return true;
}
template <typename T>
bool WriteLE(SDL_IOStream* dst, T value) {
    uint8_t bytes[sizeof(T)];
    for (size_t i = 0; i < sizeof(T); ++i)
        bytes[i] = static_cast<uint8_t>(static_cast<uint64_t>(value) >> (8 * i));
    return SDL_WriteIO(dst, bytes, sizeof(T)) == sizeof(T);
}
}  // namespace

extern "C" {

SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode) {
    FILE* handle = std::fopen(file, mode);
    if (!handle) {
        SDL_SetError("Couldn't open %s", file);
        return nullptr;
    }
    return new SDL_IOStream{handle};
}
bool SDL_CloseIO(SDL_IOStream* context) {
    if (!context) return false;
    const bool ok = std::fclose(context->file) == 0;
    delete context;
    return ok;
}
size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size) {
    return context ? std::fread(ptr, 1, size, context->file) : 0;
}
size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size) {
    return context ? std::fwrite(ptr, 1, size, context->file) : 0;
}
Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence) {
    if (!context) return -1;
    const int origin = whence == SDL_IO_SEEK_SET ? SEEK_SET : whence == SDL_IO_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (std::fseek(context->file, static_cast<long>(offset), origin) != 0) return -1;
    return std::ftell(context->file);
}
Sint64 SDL_TellIO(SDL_IOStream* context) { return context ? std::ftell(context->file) : -1; }
Sint64 SDL_GetIOSize(SDL_IOStream* context) {
    if (!context) return -1;
    const long pos = std::ftell(context->file);
    std::fseek(context->file, 0, SEEK_END);
    const long size = std::ftell(context->file);
    std::fseek(context->file, pos, SEEK_SET);
    return size;
}
bool SDL_FlushIO(SDL_IOStream* context) { return context && std::fflush(context->file) == 0; }

bool SDL_ReadU8(SDL_IOStream* src, Uint8* value) { return SDL_ReadIO(src, value, 1) == 1; }
bool SDL_ReadU16LE(SDL_IOStream* src, Uint16* value) { return ReadLE(src, value); }
bool SDL_ReadU32LE(SDL_IOStream* src, Uint32* value) { return ReadLE(src, value); }
bool SDL_ReadS32LE(SDL_IOStream* src, Sint32* value) {
    Uint32 raw = 0;
    if (!ReadLE(src, &raw)) return false;
    *value = static_cast<Sint32>(raw);
    return true;
}
bool SDL_WriteU8(SDL_IOStream* dst, Uint8 value) { return SDL_WriteIO(dst, &value, 1) == 1; }
bool SDL_WriteU16LE(SDL_IOStream* dst, Uint16 value) { return WriteLE(dst, value); }
bool SDL_WriteU32LE(SDL_IOStream* dst, Uint32 value) { return WriteLE(dst, value); }
bool SDL_WriteS32LE(SDL_IOStream* dst, Sint32 value) { return WriteLE(dst, static_cast<Uint32>(value)); }
void* SDL_LoadFile(const char* file, size_t* datasize) {
    SDL_IOStream* stream = SDL_IOFromFile(file, "rb");
    if (!stream) return nullptr;
    const Sint64 size = SDL_GetIOSize(stream);
    void* data = std::malloc(static_cast<size_t>(size) + 1);
    const size_t read = SDL_ReadIO(stream, data, static_cast<size_t>(size));
    SDL_CloseIO(stream);
    static_cast<char*>(data)[read] = '\0';
    if (datasize) *datasize = read;
    return data;
}
bool SDL_CreateDirectory(const char* path) {
    std::string current;
    for (const char* p = path; *p != '\0'; ++p) {
        current += *p;
        if (*p == '/' && current.size() > 1)
            mkdir(current.c_str(), 0777);
    }
    mkdir(current.c_str(), 0777);
    return true;
}
const char* SDL_GetBasePath(void) { return "/dev_hdd0/game/MKWR00001/USRDIR/"; }
char* SDL_GetPrefPath(const char*, const char*) { return SDL_strdup("/dev_hdd0/game/MKWR00001/USRDIR/"); }

}  // extern "C"
