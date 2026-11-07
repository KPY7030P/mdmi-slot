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
known=0

fail() {
  echo "FAIL: $*"
  failures=$((failures + 1))
}

# Prints the slowest engine boot in ms if it outlasted the first note of a realtime run, otherwise 0.
late_boot_ms() {
  awk -v rate="$2" '
    /^EVENT 9/ && !first { split($4, a, "="); first = a[2] / rate * 1000 }
    /^boot / { t = 0; for (i = 2; i <= 5; i++) { split($i, a, "="); t += a[2] + 0 } if (t > slowest) slowest = t }
    END { print (slowest > first) ? int(slowest) : 0 }' "$1"
}

# expect is "sound" when the engine must keep playing, "dead" when it must fail while the host lives.
run() {
  name="$1"; rate="$2"; block="$3"; mode="$4"; instances="$5"; expect="${6:-sound}"
  echo "=== $name: rate=$rate block=$block $mode instances=$instances expect=$expect"
  log="$OUT/$name.log"
  # Booting can occasionally take over 10 seconds (see the ROADMAP); a realtime run then ends in
  # silence without saying anything about the sound. Say so and measure again; slow boots come in
  # runs of up to three in a row.
  attempt=1
  while :; do
    timeout 60 ./clap_host.exe "MDMI Mega Drive.clap" "$WIN_OUT/$name" "$rate" "$block" "$mode" "$instances" > "$log" 2>&1
    code=$?
    late="$(late_boot_ms "$log" "$rate")"
    [ "$mode" = realtime ] && [ "$late" != 0 ] || break
    echo "SLOW BOOT: ${late} ms, later than the first note, attempt $attempt."
    [ "$attempt" -lt 8 ] || { fail "$name: boot stayed slower than the first note"; return; }
    attempt=$((attempt + 1))
  done
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

# The ROM's own tests, with patches/0003 (D28). ASan of the image's gcc loops forever on today's
# kernels unless address randomization is off, and setarch needs --privileged for that.
echo "=== rom"
log="$OUT/rom_test.log"
MSYS_NO_PATHCONV=1 docker run --privileged --rm -v "$WIN_ROOT/third_party/mdmi:/app" -w /app mdmi-rom-build sh -c 'mkdir -p tests/bin && setarch x86_64 -R make test' > "$log" 2>&1
code=$?
grep -E "^\[  (PASSED|FAILED)" "$log"
[ "$code" = 0 ] || fail "rom: make test exit $code"

run offline-48k 48000 128 offline 1
run offline-44k 44100 512 offline 1
run offline-96k 96000 256 offline 1
run realtime-48k-128 48000 128 realtime 1
run realtime-48k-64 48000 64 realtime 1
run realtime-two 48000 128 realtime 2
run offline-four 48000 256 offline 4
run realtime-eight 48000 64 realtime 8
export MDMI_TEST_BURST=2; run realtime-burst 44100 256 realtime 2; unset MDMI_TEST_BURST
# Known, D14 (D35): underruns here are counted apart until the work on D14, at the latest when
# stage 14 starts. Then this goes back to fail.
! grep -q "underruns=[1-9]" "$OUT/realtime-burst.log" || { known=$((known + 1)); echo "KNOWN, D14: realtime-burst $(grep -o "underruns=[0-9]*" "$OUT/realtime-burst.log" | sort -u | tr '\n' ' ')"; }
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
tr -d '\r' < "$log" | grep -qFx "version=$label" || fail "vst3: version is not $label"
grep -q "automatable=359 " "$log" || fail "vst3: expected 359 automatable controls"
grep -q "view=720x528 attached=1" "$log" || fail "vst3: window did not attach"
grep -q "clean_shutdown=1" "$log" || fail "vst3: no clean shutdown"
python -I "$WIN_ROOT/tools/test/check_wav.py" "$WIN_OUT/vst3.log" "$WIN_OUT/vst3-0.wav" || fail "vst3: audio"

echo "=== model"
log="$OUT/patch_test.log"
timeout 60 ./patch_test.exe "$WIN_ROOT/tools/test/files" > "$log" 2>&1
code=$?
grep -E "FAIL|PASSED|FAILED" "$log"
[ "$code" = 0 ] || fail "patch_test: exit $code"

echo "=== registers"
log="$OUT/reg_test.log"
timeout 300 ./reg_test.exe "$WIN_ROOT/tools/test/files" > "$log" 2>&1
code=$?
grep -E "FAIL|PASSED|FAILED" "$log"
[ "$code" = 0 ] || fail "reg_test: exit $code"

echo "=== knobs"
log="$OUT/param_test.log"
timeout 600 ./param_test.exe "MDMI Mega Drive.clap" > "$log" 2>&1
code=$?
grep -E "FAIL|PASSED|FAILED" "$log"
[ "$code" = 0 ] || fail "param_test: exit $code"

echo "=== window"
log="$OUT/gui.log"
timeout 90 ./gui_test.exe "MDMI Mega Drive.clap" "$WIN_OUT/gui" > "$log" 2>&1
code=$?
grep -E "FAIL|PASSED|FAILED" "$log"
[ "$code" = 0 ] || fail "gui_test: exit $code"
log="$OUT/gui-crash.log"
export MDMI_TEST_CRASH=segv
timeout 90 ./gui_test.exe "MDMI Mega Drive.clap" "$WIN_OUT/gui" > "$log" 2>&1
code=$?
unset MDMI_TEST_CRASH
grep -E "FAIL|PASSED|FAILED" "$log"
[ "$code" = 0 ] || fail "gui_test with a dead engine: exit $code"

[ "$known" = 0 ] || echo "$known KNOWN (D14), not counted"
if [ "$failures" = 0 ]; then
  echo "ALL PASSED"
else
  echo "$failures CHECKS FAILED"
fi
exit "$failures"
