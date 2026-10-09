#include "guest_flat_memory.h"

// PS3 (PSL1GHT) guest memory. lv2 cannot reserve a 4 GiB address-space window
// or alias pages, so there is no flat guest view: every translated access goes
// through the checked page-table path (RequiresCheckedAccess() is constant
// true on this target). This module only owns the backing stores and keeps the
// cached/uncached/physical mirrors of MEM1 and MEM2 pointing at one store.

#include <malloc.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace GuestFlat {
namespace {

struct Store {
    Backing kind = Backing::Owned;
    uint32_t owned = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;
};

std::mutex g_mutex;
std::vector<Store> g_stores;
std::vector<Mapping> g_mappings;
bool g_initialized = false;

inline uint64_t RoundUp(uint64_t value, uint64_t align) {
    return (value + align - 1) & ~(align - 1);
}

uint64_t Offset(const RegionRequest& r) {
    if (r.backing == Backing::Mem1) return r.base & 0x1fffffffu;
    if (r.backing == Backing::Mem2) return (r.base & 0x1fffffffu) - 0x10000000u;
    return 0;
}

} // namespace

bool IsActive() { return false; }

void Initialize(const std::vector<RegionRequest>& regions) {
    std::lock_guard lock(g_mutex);
    if (g_initialized)
        return;

    std::vector<Store> stores;
    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        auto it = std::find_if(stores.begin(), stores.end(), [&](const Store& s) {
            return s.kind == r.backing && s.owned == owned;
        });
        const uint64_t need = RoundUp(Offset(r) + r.size, kGuestPageSize);
        if (it == stores.end())
            stores.push_back({r.backing, owned, need, nullptr});
        else
            it->size = std::max(it->size, need);
    }

    for (auto& s : stores) {
        // 64 KiB alignment keeps every 1 MiB guest page inside one store, which
        // the page-table fast path relies on.
        void* host = memalign(0x10000, static_cast<size_t>(s.size));
        if (host == nullptr) {
            for (auto& prior : stores)
                free(prior.host);
            throw std::runtime_error("unable to allocate PS3 guest backing store");
        }
        std::memset(host, 0, static_cast<size_t>(s.size));
        s.host = static_cast<uint8_t*>(host);
    }

    for (const auto& r : regions) {
        if (!r.size) continue;
        const uint32_t owned = r.backing == Backing::Owned ? r.base : 0;
        const auto& s = *std::find_if(stores.begin(), stores.end(), [&](const Store& x) {
            return x.kind == r.backing && x.owned == owned;
        });
        g_mappings.push_back({r.base, r.size, s.host + Offset(r)});
    }

    g_stores = std::move(stores);
    g_initialized = true;
}

uint8_t* HostPointer(uint32_t a) {
    for (const auto& m : g_mappings) {
        if (a >= m.base && uint64_t(a - m.base) < m.size)
            return m.host + (a - m.base);
    }
    return nullptr;
}

void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}
FaultCounters Counters() { return {}; }
void LogFaultSummary() noexcept {}
bool HandleAccessViolation(void*, bool) noexcept { return false; }

} // namespace GuestFlat
