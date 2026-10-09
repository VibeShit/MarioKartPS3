# CMake toolchain for the PSL1GHT PS3 SDK (PPU, Cell BE, big-endian, LP64).
# Expects $PS3DEV (and optionally $PSL1GHT) to point at an installed ps3toolchain,
# as provided by the ps3dev Docker images.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR ppu)

if(DEFINED ENV{PS3DEV})
    set(PS3DEV "$ENV{PS3DEV}")
else()
    set(PS3DEV "/usr/local/ps3dev")
endif()
if(DEFINED ENV{PSL1GHT})
    set(PSL1GHT "$ENV{PSL1GHT}")
else()
    set(PSL1GHT "${PS3DEV}")
endif()

set(CMAKE_C_COMPILER   "${PS3DEV}/ppu/bin/powerpc64-ps3-elf-gcc")
set(CMAKE_CXX_COMPILER "${PS3DEV}/ppu/bin/powerpc64-ps3-elf-g++")
set(CMAKE_ASM_COMPILER "${PS3DEV}/ppu/bin/powerpc64-ps3-elf-gcc")
set(CMAKE_AR           "${PS3DEV}/ppu/bin/powerpc64-ps3-elf-ar" CACHE FILEPATH "")
set(CMAKE_RANLIB       "${PS3DEV}/ppu/bin/powerpc64-ps3-elf-ranlib" CACHE FILEPATH "")
set(CMAKE_STRIP        "${PS3DEV}/ppu/bin/powerpc64-ps3-elf-strip" CACHE FILEPATH "")

set(CMAKE_FIND_ROOT_PATH "${PS3DEV}/ppu" "${PS3DEV}/portlibs/ppu" "${PSL1GHT}/ppu")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(PS3_MACHDEP "-mcpu=cell -mhard-float -fmodulo-sched -ffunction-sections -fdata-sections")
set(CMAKE_C_FLAGS_INIT   "${PS3_MACHDEP}")
set(CMAKE_CXX_FLAGS_INIT "${PS3_MACHDEP} -D_GLIBCXX11_USE_C99_STDIO")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcpu=cell -Wl,--gc-sections")

set(PS3 TRUE)
