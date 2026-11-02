#!/bin/sh
# Runs the plugin through the headless hosts across rates, block sizes, modes and instance counts,
# then the knob test. Exits with the number of failed checks, so 0 means everything passed.
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WIN_ROOT="$(cygpath -m "$ROOT")"
OUT="$ROOT/build/test"
WIN_OUT="$(cygpath -m "$OUT")"
mkdir -p "$OUT"
cp "$ROOT/build/cmake/out/CLAP/MDMI Mega Drive.clap" "$ROOT/build/cmake/" && cd "$ROOT/build/cmake" || exit 1
export MDMI_PLUGIN_STATS=1
failures=0

fail() {
  echo "FAIL: $*"
  failures=$((failures + 1))
}

# expect is "sound" when the engine must keep playing, "dead" when it must fail while the host lives.
run() {
  name="$1"; rate="$2"; block="$3"; mode="$4"; instances="$5"; expect="${6:-sound}"
  echo "=== $name: rate=$rate block=$block $mode instances=$instances expect=$expect"
  log="$OUT/$name.log"
  timeout 60 ./clap_host.exe "MDMI Mega Drive.clap" "$WIN_OUT/$name" "$rate" "$block" "$mode" "$instances" > "$log" 2>&1
  code=$?
  grep -E "wall=|stats " "$log"
  [ "$code" = 0 ] || { fail "$name: host exit $code"; tail -5 "$log"; }
  grep -q "clean_shutdown=1" "$log" || fail "$name: no clean shutdown"
  if [ "$expect" = dead ]; then
    grep -q "failed=1" "$log" || fail "$name: engine was expected to fail"
    return
  fi
  ! grep -q "failed=1" "$log" || fail "$name: engine failed"
  i=0
  while [ "$i" -lt "$instances" ]; do
    python -I "$WIN_ROOT/tools/test/check_wav.py" "$WIN_OUT/$name.log" "$WIN_OUT/$name-$i.wav" || fail "$name: instance $i audio"
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
export MDMI_TEST_REACTIVATE=1; run reactivate 48000 128 realtime 1; unset MDMI_TEST_REACTIVATE
export MDMI_TEST_CRASH=fatal; run crash-fatal 48000 128 realtime 1 dead
export MDMI_TEST_CRASH=segv; run crash-segv 48000 128 realtime 1 dead
export MDMI_TEST_CRASH=freeze; run crash-freeze 48000 128 realtime 1; unset MDMI_TEST_CRASH

echo "=== vst3"
log="$OUT/vst3.log"
timeout 60 ./vst3_host.exe "out/VST3/MDMI Mega Drive.vst3" "$WIN_OUT/vst3" 48000 128 realtime > "$log" 2>&1
code=$?
[ "$code" = 0 ] || { fail "vst3: host exit $code"; tail -5 "$log"; }
label="$(tr -d '\r\n' < "$ROOT/build/label.txt")"
grep -qF "version=$label" "$log" || fail "vst3: version is not $label"
grep -q "automatable=62 " "$log" || fail "vst3: expected 62 automatable controls"
grep -q "clean_shutdown=1" "$log" || fail "vst3: no clean shutdown"
python -I "$WIN_ROOT/tools/test/check_wav.py" "$WIN_OUT/vst3.log" "$WIN_OUT/vst3-0.wav" || fail "vst3: audio"

echo "=== knobs"
log="$OUT/param_test.log"
timeout 600 ./param_test.exe "MDMI Mega Drive.clap" > "$log" 2>&1
code=$?
grep -E "FAIL|PASSED|FAILED" "$log"
[ "$code" = 0 ] || fail "param_test: exit $code"

if [ "$failures" = 0 ]; then
  echo "ALL PASSED"
else
  echo "$failures CHECKS FAILED"
fi
exit "$failures"
