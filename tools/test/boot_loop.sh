#!/bin/sh
# Experiment E8 (ROADMAP): runs clap_host N times in a row, one run at a time, at below-normal
# priority, each a realtime run of a few instances, and writes two CSV files:
#   <out>.csv        one line per run: boots seen, engines that did not boot (stage:error), the
#                    slowest boot's parts in ms, host exit, clean shutdown, SLOW BOOT, what else ran;
#   <out>-boots.csv  one line per engine that booted: its parts in ms.
# SLOW BOOT means what it means in run_tests.sh: the slowest boot outlasted the first note (3 s).
#   sh tools/test/boot_loop.sh [runs] [out.csv] [instances] [block]
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
COUNT="${1:-200}"
CSV="$(realpath -m "${2:-$ROOT/build/e8/boots.csv}")"
BOOTS="${CSV%.csv}-boots.csv"
INSTANCES="${3:-1}"
BLOCK="${4:-128}"
OUT="$ROOT/build/e8"
mkdir -p "$OUT"
WIN_OUT="$(cygpath -m "$OUT")"
cp "$ROOT/build/cmake/out/CLAP/MDMI Slot.clap" "$ROOT/build/cmake/" && cd "$ROOT/build/cmake" || exit 1
export MDMI_PLUGIN_STATS=1
# Children of this shell inherit its priority class.
powershell -NoProfile -Command "(Get-Process -Id $(cat /proc/$$/winpid)).PriorityClass='BelowNormal'"
echo "run,start,instances,boot_lines,not_booted,failed,fail_stages,unpack_ms,loadlibrary_ms,load_game_ms,frames_ms,total_ms,exit,clean,first_note_ms,slow,others" > "$CSV"
echo "run,unpack_ms,loadlibrary_ms,load_game_ms,frames_ms,total_ms" > "$BOOTS"
i=1
while [ "$i" -le "$COUNT" ]; do
  log="$OUT/run-$i.log"
  start="$(date +%H:%M:%S)"
  # What else runs: the other test hosts, Live and the docker client, before this run's own host starts.
  others="$(tasklist //FO CSV //NH 2>/dev/null | grep -ioE '^"(clap_host|vst3_host|reg_test|gui_test|param_test|patch_test|Ableton[^"]*|docker)\.exe' | tr -d '"' | sort | uniq -c | awk '{ n = $1; sub(/^ *[0-9]+ /, ""); gsub(/ /, "_"); printf "%s%s:%s", (NR > 1 ? " " : ""), $0, n }')"
  timeout 60 ./clap_host.exe "MDMI Slot.clap" "$WIN_OUT/e8" 48000 "$BLOCK" realtime "$INSTANCES" > "$log" 2>&1
  code=$?
  awk -v run="$i" -v start="$start" -v code="$code" -v rate=48000 -v inst="$INSTANCES" -v others="$others" -v boots="$BOOTS" '
    /^EVENT 9/ && !first { split($4, a, "="); first = a[2] / rate * 1000 }
    /^boot / {
      n++; t = 0
      for (k = 2; k <= 5; k++) { split($k, a, "="); q[k] = a[2] + 0; t += q[k] }
      printf "%d,%d,%d,%d,%d,%d\n", run, q[2], q[3], q[4], q[5], t >> boots
      if (t > worst) { worst = t; for (k = 2; k <= 5; k++) p[k] = q[k] }
    }
    /^stats / {
      delete s
      for (k = 2; k <= NF; k++) { split($k, a, "="); s[a[1]] = a[2] }
      if (s["failed"] == 1) failed++
      if (s["booted"] != 1) { down++; stages = stages (stages ? " " : "") s["boot_stage"] ":" s["boot_error"] }
    }
    /clean_shutdown=1/ { clean = 1 }
    END {
      printf "%d,%s,%d,%d,%d,%d,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%s\n", run, start, inst, n, down, failed, stages,
        p[2], p[3], p[4], p[5], worst, code, clean, first, (worst > first) ? 1 : 0, others
    }' "$log" | tee -a "$CSV"
  rm -f "$OUT"/e8-*.wav
  i=$((i + 1))
done
