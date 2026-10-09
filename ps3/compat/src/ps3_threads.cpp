// lv2 backing for ps3compat (see compat/include/ps3_threads.h).
#include "ps3_threads.h"

#include <chrono>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sys/time.h>

namespace ps3compat {

void Yield() noexcept { sysThreadYield(); }

void SleepMicros(uint64_t micros) noexcept {
    if (micros == 0) {
        sysThreadYield();
        return;
    }
    while (micros > 0) {
        const uint64_t slice = micros > 1000000u ? 1000000u : micros;
        sysUsleep(static_cast<u32>(slice));
        micros -= slice;
    }
}

uint64_t CurrentThreadId() noexcept {
    sys_ppu_thread_t id = 0;
    sysThreadGetId(&id);
    return static_cast<uint64_t>(id);
}

uint64_t MonotonicMicros() noexcept {
    // The PPU timebase is monotonic and runs at sysGetTimebaseFrequency().
    static const uint64_t frequency = sysGetTimebaseFrequency();
    uint64_t tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return static_cast<uint64_t>((static_cast<unsigned __int128>(tb) * 1000000u) / frequency);
}

namespace {
struct ThreadStart {
    void (*fn)(void*);
    void* arg;
};

void ThreadEntry(void* raw) {
    ThreadStart start = *static_cast<ThreadStart*>(raw);
    delete static_cast<ThreadStart*>(raw);
    start.fn(start.arg);
    sysThreadExit(0);
}
} // namespace

uint64_t StartThread(void (*fn)(void*), void* arg, uint64_t stackSize, const char* name) noexcept {
    auto* start = new ThreadStart{fn, arg};
    sys_ppu_thread_t id = 0;
    char threadName[28] = {};
    for (int i = 0; name != nullptr && name[i] != '\0' && i < 27; ++i)
        threadName[i] = name[i];
    // Priority 1001 is the conventional default for PPU worker threads.
    const s32 rc = sysThreadCreate(&id, ThreadEntry, start, 1001, stackSize, THREAD_JOINABLE, threadName);
    if (rc != 0) {
        delete start;
        return 0;
    }
    return static_cast<uint64_t>(id);
}

void JoinThread(uint64_t id) noexcept {
    u64 retval = 0;
    sysThreadJoin(static_cast<sys_ppu_thread_t>(id), &retval);
}

void DetachThread(uint64_t id) noexcept { sysThreadDetach(static_cast<sys_ppu_thread_t>(id)); }

} // namespace ps3compat

// libstdc++ is built without nanosleep support, so std::this_thread::sleep_for
// lands here.
namespace std {
namespace this_thread {
void __sleep_for(chrono::seconds s, chrono::nanoseconds ns) {
    const uint64_t micros = static_cast<uint64_t>(s.count()) * 1000000u +
                            static_cast<uint64_t>((ns.count() + 999) / 1000);
    ps3compat::SleepMicros(micros);
}
} // namespace this_thread
} // namespace std
