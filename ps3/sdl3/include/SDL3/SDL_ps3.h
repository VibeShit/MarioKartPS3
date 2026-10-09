/*
 * Minimal SDL3-compatible API for the PS3 (PSL1GHT) build.
 *
 * The desktop runtime and aurora talk to input and audio through SDL3, which
 * has no PS3 port. This header declares the subset of the SDL3 API those
 * sources use, with SDL3's names, types and semantics; ps3/sdl3/sdl3_ps3.cpp
 * implements it on top of libpad (DualShock 3 / compatible pads) and libaudio.
 * Every public SDL3/<name>.h header in this directory simply includes this one.
 *
 * Only what the game needs is implemented: gamepads, a playback audio stream,
 * events, timers, logging, hints, file streams and a few utilities. Keyboard
 * and mouse report "nothing pressed".
 */
#ifndef SDL_PS3_H_
#define SDL_PS3_H_

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- stdinc ---------------------------------------------------------- */
typedef int8_t Sint8;
typedef uint8_t Uint8;
typedef int16_t Sint16;
typedef uint16_t Uint16;
typedef int32_t Sint32;
typedef uint32_t Uint32;
typedef int64_t Sint64;
typedef uint64_t Uint64;

#define SDL_PLATFORM_PS3 1
#define SDLCALL
#define SDL_DECLSPEC

void* SDL_malloc(size_t size);
void* SDL_calloc(size_t nmemb, size_t size);
void* SDL_realloc(void* mem, size_t size);
void SDL_free(void* mem);
char* SDL_strdup(const char* str);
char* SDL_strstr(const char* haystack, const char* needle);
size_t SDL_strlcpy(char* dst, const char* src, size_t maxlen);
int SDL_snprintf(char* text, size_t maxlen, const char* fmt, ...);
const char* SDL_GetPlatform(void);

/* ---- error ----------------------------------------------------------- */
const char* SDL_GetError(void);
bool SDL_SetError(const char* fmt, ...);
bool SDL_ClearError(void);

/* ---- init ------------------------------------------------------------ */
typedef Uint32 SDL_InitFlags;
#define SDL_INIT_AUDIO 0x00000010u
#define SDL_INIT_VIDEO 0x00000020u
#define SDL_INIT_JOYSTICK 0x00000200u
#define SDL_INIT_HAPTIC 0x00001000u
#define SDL_INIT_GAMEPAD 0x00002000u
#define SDL_INIT_EVENTS 0x00004000u
#define SDL_INIT_SENSOR 0x00008000u
bool SDL_Init(SDL_InitFlags flags);
bool SDL_InitSubSystem(SDL_InitFlags flags);
void SDL_QuitSubSystem(SDL_InitFlags flags);
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags);
void SDL_Quit(void);

/* ---- hints ----------------------------------------------------------- */
#define SDL_HINT_JOYSTICK_HIDAPI_WII "SDL_JOYSTICK_HIDAPI_WII"
#define SDL_HINT_JOYSTICK_HIDAPI_WII_PLAYER_LED "SDL_JOYSTICK_HIDAPI_WII_PLAYER_LED"
#define SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS"
#define SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE "SDL_JOYSTICK_HIDAPI_GAMECUBE"
bool SDL_SetHint(const char* name, const char* value);
const char* SDL_GetHint(const char* name);

/* ---- log ------------------------------------------------------------- */
typedef enum SDL_LogCategory {
  SDL_LOG_CATEGORY_APPLICATION,
  SDL_LOG_CATEGORY_ERROR,
  SDL_LOG_CATEGORY_ASSERT,
  SDL_LOG_CATEGORY_SYSTEM,
  SDL_LOG_CATEGORY_AUDIO,
  SDL_LOG_CATEGORY_VIDEO,
  SDL_LOG_CATEGORY_RENDER,
  SDL_LOG_CATEGORY_INPUT,
  SDL_LOG_CATEGORY_TEST,
  SDL_LOG_CATEGORY_GPU,
  SDL_LOG_CATEGORY_CUSTOM = 19
} SDL_LogCategory;
typedef enum SDL_LogPriority {
  SDL_LOG_PRIORITY_INVALID,
  SDL_LOG_PRIORITY_TRACE,
  SDL_LOG_PRIORITY_VERBOSE,
  SDL_LOG_PRIORITY_DEBUG,
  SDL_LOG_PRIORITY_INFO,
  SDL_LOG_PRIORITY_WARN,
  SDL_LOG_PRIORITY_ERROR,
  SDL_LOG_PRIORITY_CRITICAL,
  SDL_LOG_PRIORITY_COUNT
} SDL_LogPriority;
typedef void(SDLCALL* SDL_LogOutputFunction)(void* userdata, int category, SDL_LogPriority priority,
                                              const char* message);
void SDL_SetLogPriority(int category, SDL_LogPriority priority);
void SDL_SetLogPriorities(SDL_LogPriority priority);
void SDL_SetLogOutputFunction(SDL_LogOutputFunction callback, void* userdata);
void SDL_Log(const char* fmt, ...);
void SDL_LogInfo(int category, const char* fmt, ...);
void SDL_LogWarn(int category, const char* fmt, ...);
void SDL_LogError(int category, const char* fmt, ...);
void SDL_LogDebug(int category, const char* fmt, ...);

/* ---- timer ----------------------------------------------------------- */
Uint64 SDL_GetTicks(void);
Uint64 SDL_GetTicksNS(void);
Uint64 SDL_GetPerformanceCounter(void);
Uint64 SDL_GetPerformanceFrequency(void);
void SDL_Delay(Uint32 ms);
void SDL_DelayNS(Uint64 ns);

/* ---- guid / properties / power -------------------------------------- */
typedef struct SDL_GUID {
  Uint8 data[16];
} SDL_GUID;
void SDL_GUIDToString(SDL_GUID guid, char* pszGUID, int cbGUID);
SDL_GUID SDL_StringToGUID(const char* pchGUID);

typedef Uint32 SDL_PropertiesID;
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char* name, bool default_value);
#define SDL_PROP_GAMEPAD_CAP_MONO_LED_BOOLEAN "SDL.joystick.cap.mono_led"
#define SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN "SDL.joystick.cap.rgb_led"
#define SDL_PROP_GAMEPAD_CAP_PLAYER_LED_BOOLEAN "SDL.joystick.cap.player_led"
#define SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN "SDL.joystick.cap.rumble"
#define SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN "SDL.joystick.cap.trigger_rumble"

typedef enum SDL_PowerState {
  SDL_POWERSTATE_ERROR = -1,
  SDL_POWERSTATE_UNKNOWN,
  SDL_POWERSTATE_ON_BATTERY,
  SDL_POWERSTATE_NO_BATTERY,
  SDL_POWERSTATE_CHARGING,
  SDL_POWERSTATE_CHARGED
} SDL_PowerState;

/* ---- sensor ---------------------------------------------------------- */
typedef enum SDL_SensorType {
  SDL_SENSOR_INVALID = -1,
  SDL_SENSOR_UNKNOWN,
  SDL_SENSOR_ACCEL,
  SDL_SENSOR_GYRO,
  SDL_SENSOR_ACCEL_L,
  SDL_SENSOR_GYRO_L,
  SDL_SENSOR_ACCEL_R,
  SDL_SENSOR_GYRO_R
} SDL_SensorType;
#define SDL_STANDARD_GRAVITY 9.80665f

/* ---- joystick -------------------------------------------------------- */
typedef Uint32 SDL_JoystickID;
typedef struct SDL_Joystick SDL_Joystick;
#define SDL_JOYSTICK_AXIS_MAX 32767
#define SDL_JOYSTICK_AXIS_MIN -32768
#define SDL_HAT_CENTERED 0x00u
#define SDL_HAT_UP 0x01u
#define SDL_HAT_RIGHT 0x02u
#define SDL_HAT_DOWN 0x04u
#define SDL_HAT_LEFT 0x08u
SDL_JoystickID* SDL_GetJoysticks(int* count);
SDL_Joystick* SDL_OpenJoystick(SDL_JoystickID instance_id);
void SDL_CloseJoystick(SDL_Joystick* joystick);
SDL_JoystickID SDL_GetJoystickID(SDL_Joystick* joystick);
const char* SDL_GetJoystickNameForID(SDL_JoystickID instance_id);
SDL_GUID SDL_GetJoystickGUIDForID(SDL_JoystickID instance_id);
void SDL_GetJoystickGUIDInfo(SDL_GUID guid, Uint16* vendor, Uint16* product, Uint16* version, Uint16* crc16);
int SDL_GetNumJoystickAxes(SDL_Joystick* joystick);
int SDL_GetNumJoystickButtons(SDL_Joystick* joystick);
Sint16 SDL_GetJoystickAxis(SDL_Joystick* joystick, int axis);
bool SDL_GetJoystickButton(SDL_Joystick* joystick, int button);
SDL_PowerState SDL_GetJoystickPowerInfo(SDL_Joystick* joystick, int* percent);

/* ---- gamepad --------------------------------------------------------- */
typedef struct SDL_Gamepad SDL_Gamepad;
typedef enum SDL_GamepadType {
  SDL_GAMEPAD_TYPE_UNKNOWN = 0,
  SDL_GAMEPAD_TYPE_STANDARD,
  SDL_GAMEPAD_TYPE_XBOX360,
  SDL_GAMEPAD_TYPE_XBOXONE,
  SDL_GAMEPAD_TYPE_PS3,
  SDL_GAMEPAD_TYPE_PS4,
  SDL_GAMEPAD_TYPE_PS5,
  SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO,
  SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT,
  SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT,
  SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR,
  SDL_GAMEPAD_TYPE_GAMECUBE,
  SDL_GAMEPAD_TYPE_COUNT
} SDL_GamepadType;
typedef enum SDL_GamepadButton {
  SDL_GAMEPAD_BUTTON_INVALID = -1,
  SDL_GAMEPAD_BUTTON_SOUTH,
  SDL_GAMEPAD_BUTTON_EAST,
  SDL_GAMEPAD_BUTTON_WEST,
  SDL_GAMEPAD_BUTTON_NORTH,
  SDL_GAMEPAD_BUTTON_BACK,
  SDL_GAMEPAD_BUTTON_GUIDE,
  SDL_GAMEPAD_BUTTON_START,
  SDL_GAMEPAD_BUTTON_LEFT_STICK,
  SDL_GAMEPAD_BUTTON_RIGHT_STICK,
  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
  SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
  SDL_GAMEPAD_BUTTON_DPAD_UP,
  SDL_GAMEPAD_BUTTON_DPAD_DOWN,
  SDL_GAMEPAD_BUTTON_DPAD_LEFT,
  SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
  SDL_GAMEPAD_BUTTON_MISC1,
  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
  SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
  SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
  SDL_GAMEPAD_BUTTON_TOUCHPAD,
  SDL_GAMEPAD_BUTTON_MISC2,
  SDL_GAMEPAD_BUTTON_MISC3,
  SDL_GAMEPAD_BUTTON_MISC4,
  SDL_GAMEPAD_BUTTON_MISC5,
  SDL_GAMEPAD_BUTTON_MISC6,
  SDL_GAMEPAD_BUTTON_COUNT
} SDL_GamepadButton;
typedef enum SDL_GamepadAxis {
  SDL_GAMEPAD_AXIS_INVALID = -1,
  SDL_GAMEPAD_AXIS_LEFTX,
  SDL_GAMEPAD_AXIS_LEFTY,
  SDL_GAMEPAD_AXIS_RIGHTX,
  SDL_GAMEPAD_AXIS_RIGHTY,
  SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
  SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
  SDL_GAMEPAD_AXIS_COUNT
} SDL_GamepadAxis;
SDL_JoystickID* SDL_GetGamepads(int* count);
bool SDL_IsGamepad(SDL_JoystickID instance_id);
SDL_Gamepad* SDL_OpenGamepad(SDL_JoystickID instance_id);
void SDL_CloseGamepad(SDL_Gamepad* gamepad);
SDL_Gamepad* SDL_GetGamepadFromID(SDL_JoystickID instance_id);
SDL_Gamepad* SDL_GetGamepadFromPlayerIndex(int player_index);
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad* gamepad);
SDL_Joystick* SDL_GetGamepadJoystick(SDL_Gamepad* gamepad);
const char* SDL_GetGamepadName(SDL_Gamepad* gamepad);
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad* gamepad);
Uint16 SDL_GetGamepadVendor(SDL_Gamepad* gamepad);
Uint16 SDL_GetGamepadProduct(SDL_Gamepad* gamepad);
const char* SDL_GetGamepadSerial(SDL_Gamepad* gamepad);
SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID instance_id);
char* SDL_GetGamepadMappingForID(SDL_JoystickID instance_id);
int SDL_AddGamepadMapping(const char* mapping);
SDL_PropertiesID SDL_GetGamepadProperties(SDL_Gamepad* gamepad);
int SDL_GetGamepadPlayerIndex(SDL_Gamepad* gamepad);
bool SDL_SetGamepadPlayerIndex(SDL_Gamepad* gamepad, int player_index);
SDL_PowerState SDL_GetGamepadPowerInfo(SDL_Gamepad* gamepad, int* percent);
bool SDL_GetGamepadButton(SDL_Gamepad* gamepad, SDL_GamepadButton button);
Sint16 SDL_GetGamepadAxis(SDL_Gamepad* gamepad, SDL_GamepadAxis axis);
bool SDL_RumbleGamepad(SDL_Gamepad* gamepad, Uint16 low_frequency_rumble, Uint16 high_frequency_rumble,
                       Uint32 duration_ms);
bool SDL_SetGamepadLED(SDL_Gamepad* gamepad, Uint8 red, Uint8 green, Uint8 blue);
bool SDL_GamepadHasSensor(SDL_Gamepad* gamepad, SDL_SensorType type);
bool SDL_SetGamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type, bool enabled);
bool SDL_GamepadSensorEnabled(SDL_Gamepad* gamepad, SDL_SensorType type);
bool SDL_GetGamepadSensorData(SDL_Gamepad* gamepad, SDL_SensorType type, float* data, int num_values);
const char* SDL_GetGamepadStringForButton(SDL_GamepadButton button);
const char* SDL_GetGamepadStringForAxis(SDL_GamepadAxis axis);
void SDL_UpdateGamepads(void);

/* ---- keyboard / mouse ------------------------------------------------ */
typedef enum SDL_Scancode {
  SDL_SCANCODE_UNKNOWN = 0,
  SDL_SCANCODE_A = 4,
  SDL_SCANCODE_B = 5,
  SDL_SCANCODE_C = 6,
  SDL_SCANCODE_D = 7,
  SDL_SCANCODE_E = 8,
  SDL_SCANCODE_F = 9,
  SDL_SCANCODE_G = 10,
  SDL_SCANCODE_H = 11,
  SDL_SCANCODE_I = 12,
  SDL_SCANCODE_J = 13,
  SDL_SCANCODE_K = 14,
  SDL_SCANCODE_L = 15,
  SDL_SCANCODE_M = 16,
  SDL_SCANCODE_N = 17,
  SDL_SCANCODE_O = 18,
  SDL_SCANCODE_P = 19,
  SDL_SCANCODE_Q = 20,
  SDL_SCANCODE_R = 21,
  SDL_SCANCODE_S = 22,
  SDL_SCANCODE_T = 23,
  SDL_SCANCODE_U = 24,
  SDL_SCANCODE_V = 25,
  SDL_SCANCODE_W = 26,
  SDL_SCANCODE_X = 27,
  SDL_SCANCODE_Y = 28,
  SDL_SCANCODE_Z = 29,
  SDL_SCANCODE_1 = 30,
  SDL_SCANCODE_2 = 31,
  SDL_SCANCODE_3 = 32,
  SDL_SCANCODE_4 = 33,
  SDL_SCANCODE_5 = 34,
  SDL_SCANCODE_6 = 35,
  SDL_SCANCODE_7 = 36,
  SDL_SCANCODE_8 = 37,
  SDL_SCANCODE_9 = 38,
  SDL_SCANCODE_0 = 39,
  SDL_SCANCODE_RETURN = 40,
  SDL_SCANCODE_ESCAPE = 41,
  SDL_SCANCODE_BACKSPACE = 42,
  SDL_SCANCODE_TAB = 43,
  SDL_SCANCODE_SPACE = 44,
  SDL_SCANCODE_MINUS = 45,
  SDL_SCANCODE_EQUALS = 46,
  SDL_SCANCODE_LEFTBRACKET = 47,
  SDL_SCANCODE_RIGHTBRACKET = 48,
  SDL_SCANCODE_BACKSLASH = 49,
  SDL_SCANCODE_SEMICOLON = 51,
  SDL_SCANCODE_APOSTROPHE = 52,
  SDL_SCANCODE_GRAVE = 53,
  SDL_SCANCODE_COMMA = 54,
  SDL_SCANCODE_PERIOD = 55,
  SDL_SCANCODE_SLASH = 56,
  SDL_SCANCODE_F1 = 58,
  SDL_SCANCODE_F2 = 59,
  SDL_SCANCODE_F3 = 60,
  SDL_SCANCODE_F4 = 61,
  SDL_SCANCODE_F5 = 62,
  SDL_SCANCODE_F6 = 63,
  SDL_SCANCODE_F7 = 64,
  SDL_SCANCODE_F8 = 65,
  SDL_SCANCODE_F9 = 66,
  SDL_SCANCODE_F10 = 67,
  SDL_SCANCODE_F11 = 68,
  SDL_SCANCODE_F12 = 69,
  SDL_SCANCODE_INSERT = 73,
  SDL_SCANCODE_HOME = 74,
  SDL_SCANCODE_PAGEUP = 75,
  SDL_SCANCODE_DELETE = 76,
  SDL_SCANCODE_END = 77,
  SDL_SCANCODE_PAGEDOWN = 78,
  SDL_SCANCODE_RIGHT = 79,
  SDL_SCANCODE_LEFT = 80,
  SDL_SCANCODE_DOWN = 81,
  SDL_SCANCODE_UP = 82,
  SDL_SCANCODE_KP_ENTER = 88,
  SDL_SCANCODE_LCTRL = 224,
  SDL_SCANCODE_LSHIFT = 225,
  SDL_SCANCODE_LALT = 226,
  SDL_SCANCODE_LGUI = 227,
  SDL_SCANCODE_RCTRL = 228,
  SDL_SCANCODE_RSHIFT = 229,
  SDL_SCANCODE_RALT = 230,
  SDL_SCANCODE_RGUI = 231,
  SDL_SCANCODE_COUNT = 512
} SDL_Scancode;
typedef Uint32 SDL_Keycode;
typedef Uint16 SDL_Keymod;
typedef Uint32 SDL_WindowID;
typedef Uint32 SDL_KeyboardID;
typedef Uint32 SDL_MouseID;
typedef Uint32 SDL_MouseButtonFlags;
typedef struct SDL_Window SDL_Window;
const bool* SDL_GetKeyboardState(int* numkeys);
SDL_Window* SDL_GetKeyboardFocus(void);
const char* SDL_GetScancodeName(SDL_Scancode scancode);
SDL_MouseButtonFlags SDL_GetMouseState(float* x, float* y);
bool SDL_ShowCursor(void);
bool SDL_HideCursor(void);

/* ---- events ---------------------------------------------------------- */
typedef enum SDL_EventType {
  SDL_EVENT_FIRST = 0,
  SDL_EVENT_QUIT = 0x100,
  SDL_EVENT_KEY_DOWN = 0x300,
  SDL_EVENT_KEY_UP,
  SDL_EVENT_MOUSE_MOTION = 0x400,
  SDL_EVENT_MOUSE_BUTTON_DOWN,
  SDL_EVENT_MOUSE_BUTTON_UP,
  SDL_EVENT_MOUSE_WHEEL,
  SDL_EVENT_JOYSTICK_AXIS_MOTION = 0x600,
  SDL_EVENT_JOYSTICK_BALL_MOTION,
  SDL_EVENT_JOYSTICK_HAT_MOTION,
  SDL_EVENT_JOYSTICK_BUTTON_DOWN,
  SDL_EVENT_JOYSTICK_BUTTON_UP,
  SDL_EVENT_JOYSTICK_ADDED,
  SDL_EVENT_JOYSTICK_REMOVED,
  SDL_EVENT_GAMEPAD_AXIS_MOTION = 0x650,
  SDL_EVENT_GAMEPAD_BUTTON_DOWN,
  SDL_EVENT_GAMEPAD_BUTTON_UP,
  SDL_EVENT_GAMEPAD_ADDED,
  SDL_EVENT_GAMEPAD_REMOVED,
  SDL_EVENT_GAMEPAD_REMAPPED,
  SDL_EVENT_LAST = 0xFFFF
} SDL_EventType;

typedef struct SDL_CommonEvent {
  Uint32 type;
  Uint32 reserved;
  Uint64 timestamp;
} SDL_CommonEvent;
typedef struct SDL_KeyboardEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_WindowID windowID;
  SDL_KeyboardID which;
  SDL_Scancode scancode;
  SDL_Keycode key;
  SDL_Keymod mod;
  Uint16 raw;
  bool down;
  bool repeat;
} SDL_KeyboardEvent;
typedef struct SDL_MouseMotionEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_WindowID windowID;
  SDL_MouseID which;
  SDL_MouseButtonFlags state;
  float x, y, xrel, yrel;
} SDL_MouseMotionEvent;
typedef struct SDL_MouseButtonEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_WindowID windowID;
  SDL_MouseID which;
  Uint8 button;
  bool down;
  Uint8 clicks;
  Uint8 padding;
  float x, y;
} SDL_MouseButtonEvent;
typedef struct SDL_MouseWheelEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_WindowID windowID;
  SDL_MouseID which;
  float x, y;
  Uint32 direction;
  float mouse_x, mouse_y;
} SDL_MouseWheelEvent;
typedef struct SDL_JoyAxisEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
  Uint8 axis;
  Uint8 padding1, padding2, padding3;
  Sint16 value;
  Uint16 padding4;
} SDL_JoyAxisEvent;
typedef struct SDL_JoyHatEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
  Uint8 hat;
  Uint8 value;
  Uint8 padding1, padding2;
} SDL_JoyHatEvent;
typedef struct SDL_JoyButtonEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
  Uint8 button;
  bool down;
  Uint8 padding1, padding2;
} SDL_JoyButtonEvent;
typedef struct SDL_JoyDeviceEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
} SDL_JoyDeviceEvent;
typedef struct SDL_GamepadDeviceEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
} SDL_GamepadDeviceEvent;
typedef struct SDL_GamepadButtonEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
  Uint8 button;
  bool down;
  Uint8 padding1, padding2;
} SDL_GamepadButtonEvent;
typedef struct SDL_GamepadAxisEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
  SDL_JoystickID which;
  Uint8 axis;
  Uint8 padding1, padding2, padding3;
  Sint16 value;
  Uint16 padding4;
} SDL_GamepadAxisEvent;
typedef struct SDL_QuitEvent {
  SDL_EventType type;
  Uint32 reserved;
  Uint64 timestamp;
} SDL_QuitEvent;

typedef union SDL_Event {
  Uint32 type;
  SDL_CommonEvent common;
  SDL_KeyboardEvent key;
  SDL_MouseMotionEvent motion;
  SDL_MouseButtonEvent button;
  SDL_MouseWheelEvent wheel;
  SDL_JoyDeviceEvent jdevice;
  SDL_JoyAxisEvent jaxis;
  SDL_JoyHatEvent jhat;
  SDL_JoyButtonEvent jbutton;
  SDL_GamepadDeviceEvent gdevice;
  SDL_GamepadAxisEvent gaxis;
  SDL_GamepadButtonEvent gbutton;
  SDL_QuitEvent quit;
  Uint8 padding[128];
} SDL_Event;

void SDL_PumpEvents(void);
bool SDL_PollEvent(SDL_Event* event);
bool SDL_PushEvent(SDL_Event* event);

/* ---- audio ----------------------------------------------------------- */
typedef enum SDL_AudioFormat {
  SDL_AUDIO_UNKNOWN = 0x0000u,
  SDL_AUDIO_U8 = 0x0008u,
  SDL_AUDIO_S8 = 0x8008u,
  SDL_AUDIO_S16LE = 0x8010u,
  SDL_AUDIO_S16BE = 0x9010u,
  SDL_AUDIO_S32LE = 0x8020u,
  SDL_AUDIO_S32BE = 0x9020u,
  SDL_AUDIO_F32LE = 0x8120u,
  SDL_AUDIO_F32BE = 0x9120u,
  SDL_AUDIO_S16 = SDL_AUDIO_S16BE,
  SDL_AUDIO_F32 = SDL_AUDIO_F32BE
} SDL_AudioFormat;
typedef Uint32 SDL_AudioDeviceID;
#define SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK ((SDL_AudioDeviceID)0xFFFFFFFFu)
typedef struct SDL_AudioSpec {
  SDL_AudioFormat format;
  int channels;
  int freq;
} SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
typedef void(SDLCALL* SDL_AudioStreamCallback)(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                                int total_amount);
SDL_AudioStream* SDL_OpenAudioDeviceStream(SDL_AudioDeviceID devid, const SDL_AudioSpec* spec,
                                           SDL_AudioStreamCallback callback, void* userdata);
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream* stream);
bool SDL_PauseAudioStreamDevice(SDL_AudioStream* stream);
bool SDL_PutAudioStreamData(SDL_AudioStream* stream, const void* buf, int len);
int SDL_GetAudioStreamQueued(SDL_AudioStream* stream);
int SDL_GetAudioStreamAvailable(SDL_AudioStream* stream);
bool SDL_ClearAudioStream(SDL_AudioStream* stream);
bool SDL_SetAudioStreamGain(SDL_AudioStream* stream, float gain);
float SDL_GetAudioStreamGain(SDL_AudioStream* stream);
void SDL_DestroyAudioStream(SDL_AudioStream* stream);

/* ---- iostream / filesystem ------------------------------------------ */
typedef struct SDL_IOStream SDL_IOStream;
typedef enum SDL_IOWhence { SDL_IO_SEEK_SET, SDL_IO_SEEK_CUR, SDL_IO_SEEK_END } SDL_IOWhence;
SDL_IOStream* SDL_IOFromFile(const char* file, const char* mode);
bool SDL_CloseIO(SDL_IOStream* context);
size_t SDL_ReadIO(SDL_IOStream* context, void* ptr, size_t size);
size_t SDL_WriteIO(SDL_IOStream* context, const void* ptr, size_t size);
Sint64 SDL_SeekIO(SDL_IOStream* context, Sint64 offset, SDL_IOWhence whence);
Sint64 SDL_TellIO(SDL_IOStream* context);
Sint64 SDL_GetIOSize(SDL_IOStream* context);
bool SDL_FlushIO(SDL_IOStream* context);
bool SDL_ReadU8(SDL_IOStream* src, Uint8* value);
bool SDL_ReadU16LE(SDL_IOStream* src, Uint16* value);
bool SDL_ReadU32LE(SDL_IOStream* src, Uint32* value);
bool SDL_ReadS32LE(SDL_IOStream* src, Sint32* value);
bool SDL_WriteU8(SDL_IOStream* dst, Uint8 value);
bool SDL_WriteU16LE(SDL_IOStream* dst, Uint16 value);
bool SDL_WriteU32LE(SDL_IOStream* dst, Uint32 value);
bool SDL_WriteS32LE(SDL_IOStream* dst, Sint32 value);
void* SDL_LoadFile(const char* file, size_t* datasize);
bool SDL_CreateDirectory(const char* path);
const char* SDL_GetBasePath(void);
char* SDL_GetPrefPath(const char* org, const char* app);

#ifdef __cplusplus
}
#endif

#endif /* SDL_PS3_H_ */
