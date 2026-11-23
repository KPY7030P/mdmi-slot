#!/bin/sh
# E7: N full reg_test runs, one after another, logs into build/e7/<series>-<i>.log.
# Usage: series.sh <worktree> <series> <N>
ROOT="$1"; SERIES="$2"; N="$3"
WIN_ROOT="$(cygpath -m "$ROOT")"
mkdir -p "$ROOT/build/e7"
cd "$ROOT/build/cmake" || exit 1
for i in $(seq 1 "$N"); do
  log="$ROOT/build/e7/$SERIES-$i.log"
  echo "== $SERIES $i $(date +%H:%M:%S) others: $(tasklist 2>/dev/null | grep -ciE 'clap_host|reg_test|gui_test|Ableton')" >> "$ROOT/build/e7/$SERIES-runs.txt"
  timeout 900 ./reg_test.exe "$WIN_ROOT/tools/test/files" > "$log" 2>&1
  echo "   exit $? $(date +%H:%M:%S) $(grep -E '^(PASSED|FAILED)' "$log")" >> "$ROOT/build/e7/$SERIES-runs.txt"
done
