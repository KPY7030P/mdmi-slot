#!/bin/sh
# Runs the plugin through the headless host across rates, block sizes, modes and instance counts.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WIN_ROOT="$(cygpath -m "$ROOT")"
OUT="$ROOT/build/test"
mkdir -p "$OUT"
cp "$ROOT/build/cmake/out/CLAP/MDMI Mega Drive.clap" "$ROOT/build/cmake/" && cd "$ROOT/build/cmake" || exit 1
export MDMI_PLUGIN_STATS=1

run() {
  name="$1"; rate="$2"; block="$3"; mode="$4"; instances="$5"
  echo "=== $name: rate=$rate block=$block $mode instances=$instances"
  timeout 60 ./clap_host.exe "MDMI Mega Drive.clap" "$(cygpath -m "$OUT")/$name" "$rate" "$block" "$mode" "$instances" > "$OUT/$name.log" 2>&1
  code=$?
  grep -E "wall=|clean_shutdown|failed|stats " "$OUT/$name.log"
  [ "$code" = 0 ] || { echo "HOST EXIT $code"; tail -5 "$OUT/$name.log"; }
  i=0
  while [ "$i" -lt "$instances" ]; do
    [ -f "$OUT/$name-$i.wav" ] && python -I "$WIN_ROOT/tools/test/check_wav.py" "$(cygpath -m "$OUT")/$name.log" "$(cygpath -m "$OUT")/$name-$i.wav"
    i=$((i + 1))
  done
}

run offline-48k 48000 128 offline 1
run offline-44k 44100 512 offline 1
run offline-96k 96000 256 offline 1
run realtime-48k-128 48000 128 realtime 1
run realtime-48k-64 48000 64 realtime 1
run realtime-two 48000 128 realtime 2
run offline-four 48000 256 offline 4
run realtime-eight 48000 64 realtime 8
(export MDMI_TEST_REACTIVATE=1; run reactivate 48000 128 realtime 1)
(export MDMI_TEST_CRASH=fatal; run crash-fatal 48000 128 realtime 1)
(export MDMI_TEST_CRASH=segv; run crash-segv 48000 128 realtime 1)
