# Building WiiCompiled for the PlayStation 3

The PS3 port builds the same translated game code and runtime as the desktop
versions with the [PSL1GHT](https://github.com/ps3dev/PSL1GHT) SDK, and replaces
aurora's WebGPU renderer with an RSX backend. The result is a homebrew
`.pkg` (and a `.self` for `ps3load`).

> [!WARNING]
> The PS3 port is experimental. It builds and links, but it has not been run
> on hardware or in RPCS3 yet. See [Status](#status).

## Requirements

- A PS3 that runs homebrew (CFW/HEN), or RPCS3.
- Podman or Docker, and the PSL1GHT SDK image `ghcr.io/altps3/ps3dev:latest`
  (any image with `PS3DEV`/`PSL1GHT` set up works, see `PS3DEV_IMAGE`).
- The .NET 8 SDK on the host, for the translator.
- Your own PAL `RMCP01` disc, extracted exactly as in
  [building-macos.md](building-macos.md#3-step-1-extract-disc-assets):
  `Assets/main.dol`, `Assets/StaticR.rel` and `Assets/DATA/{files,sys}`.

## Build

```bash
ps3/build.sh
```

This builds the translator, translates `main.dol`/`StaticR.rel` into
`generated/`, then compiles and packages everything inside the SDK container.
Outputs land in `build/ps3/`:

- `WiiCompiled.pkg` – installable package (title ID `MKWR00001`)
- `wiicompiled.self` – for `ps3load`
- `pkg/` – the package contents

Useful environment variables:

| Variable | Meaning |
| --- | --- |
| `PS3DEV_IMAGE` | SDK container image (default `ghcr.io/altps3/ps3dev:latest`) |
| `CONTAINER` | `podman` or `docker` (default: whichever is installed) |
| `JOBS` | parallel compile jobs |
| `EXTRA_CMAKE_ARGS` | extra arguments for the CMake configure step |

On NixOS, `podman` from nixpkgs is enough; the SDK lives in the container.

`ps3/build.sh --synthetic` runs the whole pipeline on a tiny generated test
program (`ps3/tests/synthetic`) instead of the game, which needs no game data.

## Install

1. Install `WiiCompiled.pkg` from Package Manager.
2. Copy your extracted `Assets/DATA` folder (containing `files/` and `sys/`) to
   `/dev_hdd0/game/MKWR00001/USRDIR/DATA`.
3. Settings live in `/dev_hdd0/game/MKWR00001/USRDIR/UserData/Config.toml`;
   `paths.dvd_root` already points at `../DATA`.

## How the port works

- **Toolchain** (`ps3/cmake`, `ps3/CMakeLists.txt`): GCC 13 for the Cell PPU,
  newlib and libstdc++ without threads. `ps3/compat` supplies what they lack:
  `std::mutex`/`condition_variable`/`thread` on lv2 threads, a few POSIX and C99
  functions, IPv4 `getaddrinfo`, and a small `absl` subset used by aurora.
- **Runtime**: the PPU is big-endian like the Wii, so guest memory accesses and
  aurora's byte swaps become plain loads. Guest memory is page-table backed
  (MEM1 24 MiB, MEM2 64 MiB) instead of a 4 GiB reservation, paired singles use
  GCC vectors, and fibers use libco's PowerPC backend.
- **Platform**: `ps3/sdl3` implements the small SDL3 subset the runtime uses on
  top of libpad (DualShock 3/SIXAXIS) and libaudio. `ps3/platform` loads the
  system modules and starts the network stack.
- **Graphics** (`ps3/aurora/src`): aurora's GX front end (command processor,
  state, texture decoding) is unchanged. The RSX backend renders immediately:
  - vertices are decoded, transformed, lit and texgen'd on the PPU, following
    aurora's WGSL vertex stage;
  - each TEV configuration becomes an NV40 fragment program, assembled at
    runtime by the vendored PSL1GHT `cgcomp` assembler
    (`ps3/third_party/cgcomp`, MIT). Konst colors, TEV registers, fog color
    and alpha references are patched into the program's embedded constants;
  - EFB copies render into textures; the display copy is scaled to the TV
    output and flipped on vsync.

## Status

Working:

- The synthetic pipeline test translates, compiles and links into a SELF/PKG.
- Every generated fragment program assembles (checked on the host against
  thousands of random TEV setups).

Not done or approximated:

- Never run on hardware or RPCS3; expect bugs.
- TEV runs in float math, not GX's exact integer datapath.
- Indirect textures (bump mapping) are ignored.
- Textures have no mipmaps (linear RSX textures); filtering is bilinear.
- Depth EFB copies, EFB-to-RAM read-back, palette conversion of EFB copies and
  frame interpolation are not implemented.
- The ImGui settings bar and Discord presence are desktop-only; settings come
  from `Config.toml`.
- Memory is tight: the PS3 has 256 MiB of main RAM, of which the guest's MEM1 +
  MEM2 take 88 MiB and RSX streaming 32 MiB; the translated game code must fit
  in the rest.
