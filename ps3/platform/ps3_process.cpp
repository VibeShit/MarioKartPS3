// Process-level PS3 setup, linked directly into the executable.
//
// * The main thread runs the guest's main fiber host and the HLE layers, which need more than
//   PSL1GHT's default stack.
// * System PRX modules for networking (sockets, DNS, the HTTPS-backed services), pads and
//   audio are loaded and the network stack is initialized before main() runs.

#include <net/net.h>
#include <net/netctl.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>

SYS_PROCESS_PARAM(1001, 0x100000);

namespace {
__attribute__((constructor(101))) void ps3_initialize_system_modules() {
  sysModuleLoad(SYSMODULE_FS);
  sysModuleLoad(SYSMODULE_IO);
  sysModuleLoad(SYSMODULE_AUDIO);
  sysModuleLoad(SYSMODULE_GCM_SYS);
  sysModuleLoad(SYSMODULE_SYSUTIL);
  if (sysModuleLoad(SYSMODULE_NET) == 0) {
    netInitialize();
    if (sysModuleLoad(SYSMODULE_NETCTL) == 0) {
      netCtlInit();
    }
  }
}
} // namespace
