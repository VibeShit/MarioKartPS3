# PS3 (PSL1GHT) build of WiiCompiled.
#
# Configure with the PS3 toolchain, pointing MKW_GENERATED_ROOT at the directory
# that holds the translator's generated/ tree (see ps3/README.md and
# ps3/build.sh, which drive the whole pipeline inside the ps3dev Docker image):
#
#   cmake -S ps3 -B build/ps3 -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=ps3/cmake/ps3.toolchain.cmake \
#         -DMKW_GENERATED_ROOT=<dir containing generated/>
#
# The desktop runtime and aurora sources are shared with the PC build. What is
# PS3-specific lives under ps3/: libstdc++ threading/libc compat, a minimal
# SDL3 (libpad/libaudio), an absl shim, the RSX renderer behind aurora's GX
# front end, and the guest-memory backend.
cmake_minimum_required(VERSION 3.25)
project(mkw_ps3 C CXX ASM)

if(NOT PS3)
    message(FATAL_ERROR "Configure with -DCMAKE_TOOLCHAIN_FILE=ps3/cmake/ps3.toolchain.cmake")
endif()

get_filename_component(MKW_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MKW_PS3_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(MKW_RUNTIME_DIR "${MKW_ROOT}/runtime")
set(MKW_AURORA_DIR "${MKW_ROOT}/aurora-main")

set(MKW_GENERATED_ROOT "${MKW_ROOT}" CACHE PATH
    "Directory containing the translator's generated/ output tree")
set(MKW_PS3_TITLE_ID "MKWR00001" CACHE STRING "PS3 title ID (4 letters + 5 digits)")
set(MKW_PS3_TITLE "Mario Kart Wii" CACHE STRING "Title shown on the XMB")
set(MKW_PS3_CONTENT_ID "UP0001-${MKW_PS3_TITLE_ID}_00-0000000000000000" CACHE STRING "NPDRM content ID")

if(NOT EXISTS "${MKW_GENERATED_ROOT}/generated/build_shards/shards.cmake")
    message(FATAL_ERROR
        "No translator output in ${MKW_GENERATED_ROOT}/generated. Run the translator first "
        "(ps3/build.sh does this) or set MKW_GENERATED_ROOT.")
endif()

# ---------------------------------------------------------------------------
# Third-party sources fetched at configure time
# ---------------------------------------------------------------------------
include(FetchContent)
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD) # header-only use: populate without add_subdirectory
endif()
FetchContent_Declare(mkw_fmt
    GIT_REPOSITORY https://github.com/fmtlib/fmt.git
    GIT_TAG 11.1.4
    GIT_SHALLOW TRUE)
FetchContent_Declare(mkw_xxhash
    GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
    GIT_TAG v0.8.4
    GIT_SHALLOW TRUE)
FetchContent_GetProperties(mkw_fmt)
if(NOT mkw_fmt_POPULATED)
    FetchContent_Populate(mkw_fmt)
endif()
FetchContent_GetProperties(mkw_xxhash)
if(NOT mkw_xxhash_POPULATED)
    FetchContent_Populate(mkw_xxhash)
endif()

# ---------------------------------------------------------------------------
# Flags shared by every PS3 target
# ---------------------------------------------------------------------------
add_library(mkw_ps3_flags INTERFACE)
target_include_directories(mkw_ps3_flags BEFORE INTERFACE
    "${MKW_PS3_DIR}/compat/include"
    "${MKW_PS3_DIR}/include"
    "${MKW_PS3_DIR}/sdl3/include"
    "${MKW_PS3_DIR}/aurora/include")
target_include_directories(mkw_ps3_flags SYSTEM INTERFACE
    "${mkw_fmt_SOURCE_DIR}/include"
    "${mkw_xxhash_SOURCE_DIR}"
    "${PSL1GHT}/ppu/include"
    "${PS3DEV}/portlibs/ppu/include")
target_compile_definitions(mkw_ps3_flags INTERFACE
    _GLIBCXX_USE_C99_MATH_TR1=1
    _GLIBCXX11_USE_C99_STDLIB=1
    FMT_HEADER_ONLY=1
    XXH_STATIC_LINKING_ONLY=1
    TARGET_PC
    NOMINMAX
    MKW_PS3_TITLE_ID="${MKW_PS3_TITLE_ID}")
target_compile_options(mkw_ps3_flags INTERFACE
    "$<$<COMPILE_LANGUAGE:C,CXX>:-include${MKW_PS3_DIR}/compat/include/ps3_prelude.h>"
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20>
    $<$<COMPILE_LANGUAGE:C>:-std=gnu11>
    -w)
target_link_directories(mkw_ps3_flags INTERFACE "${PSL1GHT}/ppu/lib" "${PS3DEV}/portlibs/ppu/lib")

# ---------------------------------------------------------------------------
# PS3 platform support
# ---------------------------------------------------------------------------
add_library(mkw_ps3_compat STATIC
    "${MKW_PS3_DIR}/compat/src/ps3_threads.cpp"
    "${MKW_PS3_DIR}/compat/src/posix_stubs.c"
    "${MKW_PS3_DIR}/compat/src/math_compat.c"
    "${MKW_PS3_DIR}/compat/src/netdb_compat.c"
    "${MKW_PS3_DIR}/sdl3/sdl3_ps3.cpp")
target_link_libraries(mkw_ps3_compat PUBLIC mkw_ps3_flags)
target_compile_options(mkw_ps3_compat PRIVATE -O2)

add_library(mkw_xxhash STATIC "${mkw_xxhash_SOURCE_DIR}/xxhash.c")
target_link_libraries(mkw_xxhash PRIVATE mkw_ps3_flags)
target_compile_options(mkw_xxhash PRIVATE -O3)

add_library(mkw_libco STATIC "${MKW_RUNTIME_DIR}/third_party/libco/libco.c")
target_link_libraries(mkw_libco PRIVATE mkw_ps3_flags)
target_compile_options(mkw_libco PRIVATE -O2)

add_library(mkw_pugixml STATIC "${MKW_RUNTIME_DIR}/third_party/pugixml/pugixml.cpp")
target_link_libraries(mkw_pugixml PRIVATE mkw_ps3_flags)
target_compile_options(mkw_pugixml PRIVATE -O2)

file(GLOB MKW_CRYPTOPP_SOURCES CONFIGURE_DEPENDS "${MKW_RUNTIME_DIR}/third_party/cryptopp/*.cpp")
list(FILTER MKW_CRYPTOPP_SOURCES EXCLUDE REGEX
    "/(adhoc|bench[123]|datatest|dlltest|fipsalgt|fipstest|pch|regtest[1234]|simple|test|validat[0-9]+)\\.cpp$")
list(FILTER MKW_CRYPTOPP_SOURCES EXCLUDE REGEX "(_avx|_ppc|_simd|_sse)\\.cpp$")
add_library(mkw_cryptopp STATIC ${MKW_CRYPTOPP_SOURCES})
target_link_libraries(mkw_cryptopp PRIVATE mkw_ps3_flags)
target_compile_definitions(mkw_cryptopp PUBLIC CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)
target_compile_options(mkw_cryptopp PRIVATE -O2)

# ---------------------------------------------------------------------------
# aurora: the shared GX front end plus the PS3 RSX backend
# ---------------------------------------------------------------------------
set(MKW_AURORA_SHARED_SOURCES
    lib/logging.cpp
    lib/input.cpp
    lib/dolphin/pad/pad.cpp
    lib/dolphin/si/si.cpp
    lib/dolphin/vi/vi.cpp
    lib/dolphin/gx/GXAurora.cpp
    lib/dolphin/gx/GXBump.cpp
    lib/dolphin/gx/GXCpu2Efb.cpp
    lib/dolphin/gx/GXCull.cpp
    lib/dolphin/gx/GXDispList.cpp
    lib/dolphin/gx/GXDraw.cpp
    lib/dolphin/gx/GXExtra.cpp
    lib/dolphin/gx/GXFifo.cpp
    lib/dolphin/gx/GXFrameBuffer.cpp
    lib/dolphin/gx/GXGeometry.cpp
    lib/dolphin/gx/GXGet.cpp
    lib/dolphin/gx/GXLighting.cpp
    lib/dolphin/gx/GXManage.cpp
    lib/dolphin/gx/GXPixel.cpp
    lib/dolphin/gx/GXTev.cpp
    lib/dolphin/gx/GXTexture.cpp
    lib/dolphin/gx/GXTransform.cpp
    lib/dolphin/gx/GXVert.cpp
    lib/dolphin/mtx/mtx.c
    lib/dolphin/mtx/mtxstack.c
    lib/dolphin/mtx/mtxvec.c
    lib/dolphin/mtx/mtx44.c
    lib/dolphin/mtx/vec.c
    lib/dolphin/mtx/quat.c
    lib/gfx/texture_convert.cpp
    lib/gx/command_processor.cpp
    lib/gx/fifo.cpp
    lib/gx/gx.cpp
    lib/gx/shader_info.cpp)
list(TRANSFORM MKW_AURORA_SHARED_SOURCES PREPEND "${MKW_AURORA_DIR}/")
file(GLOB MKW_AURORA_PS3_SOURCES CONFIGURE_DEPENDS "${MKW_PS3_DIR}/aurora/src/*.cpp")
file(GLOB MKW_CGCOMP_SOURCES CONFIGURE_DEPENDS "${MKW_PS3_DIR}/third_party/cgcomp/source/*.cpp")
add_library(mkw_aurora STATIC ${MKW_AURORA_SHARED_SOURCES} ${MKW_AURORA_PS3_SOURCES} ${MKW_CGCOMP_SOURCES})
target_link_libraries(mkw_aurora PUBLIC mkw_ps3_flags)
target_include_directories(mkw_aurora PUBLIC "${MKW_AURORA_DIR}/include")
target_include_directories(mkw_aurora PRIVATE
    "${MKW_AURORA_DIR}/lib"
    "${MKW_PS3_DIR}/aurora/src"
    "${MKW_PS3_DIR}/third_party/cgcomp/include")
target_compile_definitions(mkw_aurora PUBLIC AURORA AURORA_ENABLE_GX AURORA_RSX)
target_compile_options(mkw_aurora PRIVATE -O2)

# ---------------------------------------------------------------------------
# Runtime
# ---------------------------------------------------------------------------
file(GLOB_RECURSE MKW_RUNTIME_SOURCES CONFIGURE_DEPENDS "${MKW_RUNTIME_DIR}/src/*.cpp")
list(FILTER MKW_RUNTIME_SOURCES EXCLUDE REGEX
    "/(guest_flat_memory|guest_flat_memory_macos|external_audio_macos|host_cpu_baseline|settings_overlay|controller_mapping_wizard|discord_presence)\\.cpp$")
list(FILTER MKW_RUNTIME_SOURCES EXCLUDE REGEX "/product/")
file(GLOB MKW_PS3_RUNTIME_SOURCES CONFIGURE_DEPENDS "${MKW_PS3_DIR}/runtime/*.cpp")

set(MKW_PPC_SEMANTIC_SOURCES
    "${MKW_RUNTIME_DIR}/src/ppc_helpers.cpp"
    "${MKW_RUNTIME_DIR}/src/fpu_helpers.cpp"
    "${MKW_RUNTIME_DIR}/src/ppc_quantized.cpp")
# The translator's explicit float casts are PPC single-precision rounding points;
# fast-math may erase them.
set(MKW_TRANSLATED_FP_OPTIONS -fno-fast-math -ffp-contract=off)
set_source_files_properties(${MKW_PPC_SEMANTIC_SOURCES} PROPERTIES
    COMPILE_OPTIONS "${MKW_TRANSLATED_FP_OPTIONS}")

add_library(mkw_runtime_includes INTERFACE)
target_include_directories(mkw_runtime_includes INTERFACE
    "${MKW_RUNTIME_DIR}/include"
    "${MKW_RUNTIME_DIR}/src"
    "${MKW_GENERATED_ROOT}"
    "${MKW_RUNTIME_DIR}/third_party/toml11"
    "${MKW_RUNTIME_DIR}/third_party/pugixml"
    "${MKW_RUNTIME_DIR}/third_party/libco"
    "${MKW_RUNTIME_DIR}/third_party")
target_compile_definitions(mkw_runtime_includes INTERFACE
    SDL_MAIN_HANDLED CRYPTOPP_DISABLE_ASM CRYPTOPP_NO_GLOBAL_BYTE)

add_library(mkw_runtime STATIC ${MKW_RUNTIME_SOURCES} ${MKW_PS3_RUNTIME_SOURCES})
target_link_libraries(mkw_runtime PUBLIC mkw_ps3_flags mkw_runtime_includes mkw_aurora)
target_compile_options(mkw_runtime PRIVATE -O2)

# ---------------------------------------------------------------------------
# Translated game code (translator-owned shard graph)
# ---------------------------------------------------------------------------
include("${MKW_GENERATED_ROOT}/generated/build_shards/shards.cmake")

set(MKW_TRANSLATED_EXTRA_SOURCES)
foreach(candidate data_sections_init.cpp guest_symbol_table.cpp data_sections_init_blobs.S)
    if(EXISTS "${MKW_GENERATED_ROOT}/generated/${candidate}")
        list(APPEND MKW_TRANSLATED_EXTRA_SOURCES "${MKW_GENERATED_ROOT}/generated/${candidate}")
    endif()
endforeach()
if(NOT EXISTS "${MKW_GENERATED_ROOT}/generated/guest_symbol_table.cpp")
    list(APPEND MKW_TRANSLATED_EXTRA_SOURCES "${MKW_RUNTIME_DIR}/cmake/guest_symbol_table_stub.cpp")
endif()
set_source_files_properties("${MKW_GENERATED_ROOT}/generated/data_sections_init_blobs.S"
    PROPERTIES LANGUAGE ASM)

add_library(mkw_translated STATIC
    ${MKW_BASE_COMMON_SHARDS} ${MKW_BASE_PORTABLE_SENSITIVE_SHARDS} ${MKW_TRANSLATED_EXTRA_SOURCES})
target_link_libraries(mkw_translated PRIVATE mkw_ps3_flags mkw_runtime_includes mkw_aurora)
target_compile_options(mkw_translated PRIVATE -O2 ${MKW_TRANSLATED_FP_OPTIONS})

# ---------------------------------------------------------------------------
# Executable and PS3 packaging
# ---------------------------------------------------------------------------
add_executable(wiicompiled_ps3
    "${MKW_RUNTIME_DIR}/src/product/base_product.cpp"
    ${MKW_BASE_REGISTRATION_SOURCES})
set_target_properties(wiicompiled_ps3 PROPERTIES OUTPUT_NAME "wiicompiled" SUFFIX ".elf")
target_compile_options(wiicompiled_ps3 PRIVATE -O2 ${MKW_TRANSLATED_FP_OPTIONS})
target_link_libraries(wiicompiled_ps3 PRIVATE
    -Wl,--start-group
    mkw_translated mkw_runtime mkw_aurora mkw_cryptopp mkw_pugixml mkw_libco mkw_xxhash mkw_ps3_compat
    mbedtls mbedx509 mbedcrypto
    gcm_sys rsx sysutil sysmodule io audio net netctl sysfs rt lv2 m
    -Wl,--end-group)

set(MKW_PS3_PKG_DIR "${CMAKE_BINARY_DIR}/pkg")
set(MKW_PS3_USRDIR "${MKW_PS3_PKG_DIR}/USRDIR")
add_custom_command(TARGET wiicompiled_ps3 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MKW_PS3_USRDIR}/UserData"
    COMMAND "${CMAKE_STRIP}" "$<TARGET_FILE:wiicompiled_ps3>" -o "${CMAKE_BINARY_DIR}/wiicompiled.stripped.elf"
    COMMAND "${PS3DEV}/bin/sprxlinker" "${CMAKE_BINARY_DIR}/wiicompiled.stripped.elf"
    COMMAND "${PS3DEV}/bin/make_self" "${CMAKE_BINARY_DIR}/wiicompiled.stripped.elf" "${CMAKE_BINARY_DIR}/wiicompiled.self"
    COMMAND "${PS3DEV}/bin/make_self_npdrm" "${CMAKE_BINARY_DIR}/wiicompiled.stripped.elf"
            "${MKW_PS3_USRDIR}/EBOOT.BIN" "${MKW_PS3_CONTENT_ID}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${MKW_RUNTIME_DIR}/assets/wii" "${MKW_PS3_USRDIR}/wii_bootstrap"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MKW_RUNTIME_DIR}/assets/dsp/dsp_coef.bin" "${MKW_PS3_USRDIR}/dsp_coef.bin"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MKW_RUNTIME_DIR}/assets/certs/cacert.pem" "${MKW_PS3_USRDIR}/cacert.pem"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MKW_PS3_DIR}/package/Config.toml" "${MKW_PS3_USRDIR}/UserData/Config.toml"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PS3DEV}/bin/ICON0.PNG" "${MKW_PS3_PKG_DIR}/ICON0.PNG"
    COMMAND "${PS3DEV}/bin/sfo" --title "${MKW_PS3_TITLE}" --appid "${MKW_PS3_TITLE_ID}"
            -f "${PS3DEV}/bin/sfo.xml" "${MKW_PS3_PKG_DIR}/PARAM.SFO"
    COMMAND "${PS3DEV}/bin/pkg" --contentid "${MKW_PS3_CONTENT_ID}" "${MKW_PS3_PKG_DIR}/"
            "${CMAKE_BINARY_DIR}/WiiCompiled.pkg"
    VERBATIM)
