# Cross-compiles BlastEm and the plugin for Windows x64.
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends \
    make cmake ninja-build python3 pkg-config git ca-certificates \
    gcc-mingw-w64-x86-64-win32 g++-mingw-w64-x86-64-win32 \
    gcc-mingw-w64-x86-64-posix g++-mingw-w64-x86-64-posix binutils-mingw-w64-x86-64
WORKDIR /src
