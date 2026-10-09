// PS3 build: Tracy profiling is not available; its zone macros compile away.
#pragma once
#define ZoneScoped
#define ZoneScopedN(name)
#define ZoneScopedC(color)
#define ZoneScopedNC(name, color)
#define ZoneText(text, size)
#define ZoneName(text, size)
#define ZoneValue(value)
#define FrameMark
#define FrameMarkNamed(name)
#define FrameMarkStart(name)
#define FrameMarkEnd(name)
#define TracyPlot(name, value)
#define TracyMessage(text, size)
#define TracyMessageL(text)
#define TracyAlloc(ptr, size)
#define TracyFree(ptr)
#define TracyLockable(type, varname) type varname
#define LockableBase(type) type
#define LockMark(varname)
