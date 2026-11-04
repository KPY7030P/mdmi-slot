#!/bin/sh
# Writes the changes in third_party/ back to patches/. New files must be staged with git add first.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
git -C "$ROOT/third_party/blastem" diff HEAD > "$ROOT/patches/0001-mdmi-everdrive-bridge.patch"
git -C "$ROOT/third_party/clap-wrapper" diff HEAD > "$ROOT/patches/0002-clap-wrapper.patch"
git -C "$ROOT/third_party/mdmi" diff HEAD > "$ROOT/patches/0003-mdmi-psg-envelopes.patch"
