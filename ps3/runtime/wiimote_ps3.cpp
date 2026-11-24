// PS3 replacement for runtime/src/wiimote/wiimote_host.cpp: the PS3 exposes no HID access to
// homebrew, so no real Wii Remote is ever attached and the emulated Bluetooth stack sees none.

#include "wiimote_host.h"

namespace Wiimote {
namespace {
class NullHost final : public Bluetooth::RemoteHost {
public:
    bool IsAttached(size_t) const override { return false; }
    bool ConsumeWake(size_t) override { return false; }
    bool PopInputReport(size_t, std::vector<uint8_t>&) override { return false; }
    void SendOutputReport(size_t, const uint8_t*, size_t) override {}
    void OnLinkStateChanged(size_t, Bluetooth::LinkState) override {}
    void OnGuestDisconnect(size_t) override {}
    void OnGuestInquiry(bool) override {}
    const char* DeviceName(size_t) const override { return "Nintendo RVL-CNT-01"; }
};
} // namespace

void ConfigureSdlHints(bool) {}
void Initialize() {}
void Shutdown() {}

Bluetooth::RemoteHost& Host() {
    static NullHost host;
    return host;
}

SlotInfo Slot(size_t) { return {}; }
PairingInfo Pairing() { return {}; }
void StartPairing() {}
void Disconnect(size_t) {}
std::string PlatformNotice() { return "Wii Remotes are not supported on the PS3."; }
size_t WaitingRemotes() { return 0; }
} // namespace Wiimote
