#!/usr/bin/env bash
# Build WiiCompiled for the PlayStation 3.
#
#   1. builds the translator (.NET 8) and translates your own main.dol/StaticR.rel
#      into C++ under generated/ (exactly like the desktop build), then
#   2. compiles runtime + translated code with the PSL1GHT toolchain inside the
#      ps3dev container and packages a PS3 .pkg.
#
# Usage:
#   ps3/build.sh                      # expects Assets/main.dol and Assets/StaticR.rel
#   ps3/build.sh --synthetic          # pipeline self-test with ps3/tests/synthetic (no game data)
#
# Environment:
#   PS3DEV_IMAGE   container image with the PS3 SDK (default ghcr.io/altps3/ps3dev:latest)
#   CONTAINER      container engine: podman or docker (default: whichever is installed)
#   JOBS           parallel compile jobs (default: nproc)
#   EXTRA_CMAKE_ARGS  extra arguments for the PS3 CMake configure step
set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
workspace=$(cd "$script_dir/.." && pwd)
image=${PS3DEV_IMAGE:-ghcr.io/altps3/ps3dev:latest}
jobs=${JOBS:-$(nproc 2>/dev/null || echo 4)}
synthetic=0
skip_translate=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --synthetic) synthetic=1; shift ;;
        --skip-translate) skip_translate=1; shift ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "build.sh: unknown argument $1" >&2; exit 1 ;;
    esac
done

if [[ -z "${CONTAINER:-}" ]]; then
    if command -v podman >/dev/null 2>&1; then CONTAINER=podman
    elif command -v docker >/dev/null 2>&1; then CONTAINER=docker
    else echo "build.sh: need podman or docker" >&2; exit 1; fi
fi

if (( synthetic )); then
    project=$workspace/ps3/tests/synthetic/recomp.yml
    generated_root=$workspace/build/ps3-synthetic
    build_dir=$workspace/build/ps3-synthetic/native
    entry=0x800042E0
else
    project=$workspace/projects/mkwii/recomp.yml
    generated_root=$workspace
    build_dir=$workspace/build/ps3
    entry=0x800060A4
    for f in "$workspace/Assets/main.dol" "$workspace/Assets/StaticR.rel"; do
        [[ -f "$f" ]] || { echo "build.sh: missing $f (dump it from your own PAL RMCP01 disc)" >&2; exit 1; }
    done
fi
generated=$generated_root/generated

if (( ! skip_translate )); then
    command -v dotnet >/dev/null 2>&1 || { echo "build.sh: .NET 8 SDK (dotnet) is required" >&2; exit 1; }
    echo "== building translator"
    dotnet build "$workspace/translator/src/Translator.Cli/Translator.Cli.csproj" -c Release -v quiet -nologo
    translator=(dotnet "$workspace/translator/src/Translator.Cli/bin/Release/net8.0/Translator.Cli.dll")
    mkdir -p "$generated"
    echo "== translating"
    "${translator[@]}" translate-recursive "$entry" --project "$project" \
        --outdir "$generated/functions" --output-metadata "$generated/base_translation_output.json"
    if (( ! synthetic )); then
        "${translator[@]}" emit-base-manifest --project "$project" --out "$workspace/build/base" \
            --functions-dir "$generated/functions" \
            --translation-output-metadata "$generated/base_translation_output.json" --region P
    fi
    "${translator[@]}" generate-data-init --project "$project"
    "${translator[@]}" emit-build-shards --project "$project" \
        --base-metadata "$generated/base_translation_output.json" \
        --base-functions-dir "$generated/functions" \
        --native-source-dir "$workspace/runtime/src" --out "$generated/build_shards"
fi

echo "== compiling for PS3 in $image"
# The translator writes absolute paths (incbin, shard lists), so the workspace is
# mounted at the same path inside the container.
"$CONTAINER" run --rm -v "$workspace:$workspace" -w "$workspace" "$image" bash -lc "
    set -e
    export PS3DEV=\${PS3DEV:-/usr/local/ps3dev} PSL1GHT=\${PSL1GHT:-\${PS3DEV}}
    cmake -S ps3 -B '$build_dir' -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE='$workspace/ps3/cmake/ps3.toolchain.cmake' \
        -DMKW_GENERATED_ROOT='$generated_root' ${EXTRA_CMAKE_ARGS:-}
    cmake --build '$build_dir' -j $jobs
"
echo "== done: $build_dir/WiiCompiled.pkg (and wiicompiled.self for ps3load)"
