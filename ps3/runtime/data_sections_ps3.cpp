// PS3 replacement for the generated data_sections_init.cpp: the guest DOL/REL images ship as
// files in USRDIR/data_sections (see CMakeLists.txt) and are read straight into guest RAM.

#include "memory.h"
#include "runtime_config.h"

#include <cstdio>
#include <stdexcept>
#include <string>

namespace {
bool g_dataInitialized = false;
}

extern "C" void InitializeDataSections() {
    if (g_dataInitialized) return;
    g_dataInitialized = true;

    const auto dir = RuntimeConfigFile::ExecutableDirectory().value() / "data_sections";
    FILE* manifest = std::fopen((dir / "manifest.txt").c_str(), "r");
    if (!manifest) throw std::runtime_error("missing data_sections/manifest.txt");
    unsigned long address = 0;
    unsigned long size = 0;
    char name[64];
    while (std::fscanf(manifest, "%lx %lu %63s", &address, &size, name) == 3) {
        if (!Memory::Contains(address, size)) continue;
        FILE* blob = std::fopen((dir / name).c_str(), "rb");
        const bool ok = blob && std::fread(Memory::GetPointer(address, size), 1, size, blob) == size;
        if (blob) std::fclose(blob);
        if (!ok) throw std::runtime_error("failed to load data_sections/" + std::string(name));
    }
    std::fclose(manifest);
}

extern "C" bool IsDataSectionsInitialized() {
    return g_dataInitialized;
}
