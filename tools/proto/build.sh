#!/bin/sh
# Builds libblastem.dll and proto_host.exe in Docker, then runs the probe natively.
# Needs third_party/blastem (patches/ applied, cloned with core.autocrlf=false) and roms/.
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WIN_ROOT="$(cygpath -m "$ROOT")"
export MSYS_NO_PATHCONV=1

docker build -q -t mdmi-plugin-build "$WIN_ROOT/tools/docker"
docker run --rm -v "$WIN_ROOT:/src" -w /src mdmi-plugin-build sh -c '
  set -e
  mkdir -p build/proto
  make -C third_party/blastem -j16 OS=Windows CPU=x86_64 libblastem.dll > build/proto/dll_build.log 2>&1
  cp third_party/blastem/libblastem.dll build/proto/
  x86_64-w64-mingw32-gcc-win32 -O2 -std=gnu99 -Wall -Wextra -Wno-cast-function-type \
    -Ithird_party/blastem -static -o build/proto/proto_host.exe tools/proto/host.c
'
unset MSYS_NO_PATHCONV

cd "$ROOT/build/proto"
./proto_host.exe libblastem.dll ../../roms/mdmi-everdrive-v1.34.bin out.wav out.bmp > run.log
python -I "$WIN_ROOT/tools/proto/analyze.py" run.log out.wav out.bmp out.png
