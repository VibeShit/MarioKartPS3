// Guest functions the runtime calls directly. The synthetic pipeline test does not translate
// them, so give them weak definitions that abort if ever reached; the real game's translated
// code overrides them.
#include <cstdio>
#include <cstdlib>

struct CpuContext;

#define MKW_PS3_GUEST_STUB(name)                                                                   \
    extern "C" __attribute__((weak)) void name(CpuContext*) {                                      \
        std::fprintf(stderr, "synthetic build: guest function %s not translated\n", #name);        \
        std::abort();                                                                              \
    }

MKW_PS3_GUEST_STUB(func_8012B830)
MKW_PS3_GUEST_STUB(func_801A0620)
MKW_PS3_GUEST_STUB(func_801A1ED8)
MKW_PS3_GUEST_STUB(func_801A961C)
MKW_PS3_GUEST_STUB(func_801AADE0)
MKW_PS3_GUEST_STUB(func_801D8D30)
MKW_PS3_GUEST_STUB(func_801D9E94)
MKW_PS3_GUEST_STUB(func_8055531C)
