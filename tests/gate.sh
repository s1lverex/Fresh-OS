#!/bin/bash
# Release gate: run the complete test suite N times in a row (default 3).
# Every run must pass completely; the first failure stops the gate.
set -u
cd "$(dirname "$0")/.."
RUNS=${1:-3}
SOAK=${SOAK:-600}
LOG=${LOG:-build/gate}
mkdir -p "$LOG"
make -s image test-image gcc-image >/dev/null || { echo "gate: build failed"; exit 1; }
for i in $(seq 1 "$RUNS"); do
  echo "gate: run $i/$RUNS started $(date -u +%H:%M:%S)"
  make -s host-test > "$LOG/run$i-host.log" 2>&1 || { echo "gate: run $i: host tests FAILED"; exit 1; }
  echo "gate: run $i: host tests passed"
  tests/analyze.sh > "$LOG/run$i-analyze.log" 2>&1 || { echo "gate: run $i: static analysis FAILED"; exit 1; }
  echo "gate: run $i: static analysis clean"
  python3 tests/e2e/run_all.py --soak "$SOAK" > "$LOG/run$i-e2e.log" 2>&1
  rc=$?
  grep -E "end-to-end tests passed" "$LOG/run$i-e2e.log" | sed "s/^/gate: run $i: /"
  [ $rc -eq 0 ] || { echo "gate: run $i: end-to-end FAILED"; grep -E "^\[ FAIL" -A25 "$LOG/run$i-e2e.log"; exit 1; }
done
echo "gate: PASSED - $RUNS consecutive full runs, zero failures"
