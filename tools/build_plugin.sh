#!/bin/sh
# Builds the ROM, the engine DLL and the plugin in Docker. Refuses to build if third_party/ or roms/
# differ from what the repository records. build/label.txt exists only after a build that succeeded.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN_ROOT="$(cygpath -m "$ROOT")"
sh "$ROOT/tools/fetch_deps.sh"
export MSYS_NO_PATHCONV=1
LABEL="$(git -C "$WIN_ROOT" describe --tags --always --dirty 2>/dev/null || echo unknown)"

# The ROM is v1.34 with patches/0003 (D28). BUILD is what its screen shows after "MDMI".
docker build -q -t mdmi-rom-build "$WIN_ROOT/tools/docker/rom"
mkdir -p "$ROOT/build/rom"
rm -f "$ROOT/build/rom/mdmi-everdrive.bin"
docker run --rm -v "$WIN_ROOT/third_party/mdmi:/app" -w /app -e BUILD=v1.34p mdmi-rom-build make release > "$ROOT/build/rom/build.log" 2>&1 || { grep -E "error|Error" "$ROOT/build/rom/build.log"; exit 1; }
cp "$ROOT/third_party/mdmi/out/rom.bin" "$ROOT/build/rom/mdmi-everdrive.bin"

docker build -q -t mdmi-plugin-build "$WIN_ROOT/tools/docker"
docker run --rm -v "$WIN_ROOT:/src" -w /src -e LABEL="$LABEL" mdmi-plugin-build sh -c '
  set -e
  mkdir -p build/engine
  # Where the plugin patches the ROM image (D36): every change of the ROM moves it.
  python3 tools/gen_rom_map.py third_party/mdmi/out/symbol.txt third_party/mdmi/src/pcm_sample.c build/rom/rom_map.h
  rm -f build/label.txt
  make -C third_party/blastem -j16 OS=Windows CPU=x86_64 libblastem.dll > build/engine/build.log 2>&1 || { grep -E "error|undefined" build/engine/build.log; exit 1; }
  x86_64-w64-mingw32-strip -o build/engine/libblastem.dll third_party/blastem/libblastem.dll
  cmake -S . -B build/cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64.cmake -DMDMI_BUILD_LABEL="$LABEL"
  cmake --build build/cmake --target mdmi_all
  cp build/cmake/label.txt build/label.txt
'
