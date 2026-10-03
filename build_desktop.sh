#!/bin/bash
# Desktop (Linux) build of tico-snes9x with the Vulkan renderer, for testing
# shaders off-device. Needs SDL2, SDL2_mixer, libcurl and a Vulkan driver.
# Run from the repo root; the binary expects to be started from here too
# (it reads tico/fonts, tico/shaders and tico/lang relative to the cwd).
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build_desktop"
JOBS=$(nproc)

# Objects live beside the sources and the NRO build leaves aarch64 ones.
make -C "$ROOT/libretro" clean platform=unix STATIC_LINKING=1 >/dev/null
make -C "$ROOT/libretro" -j"$JOBS" platform=unix STATIC_LINKING=1 STATIC_LINKING_LINK=1 \
    TARGET=snes9x_libretro_unix.a
cmake -S "$ROOT/tico" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DSNES9X_CORE_LIB="$ROOT/libretro/snes9x_libretro_unix.a"
cmake --build "$BUILD" -j"$JOBS"
echo "Built $BUILD/tico-snes9x"
