#!/bin/bash
# Build and run the QPID concurrency correctness matrix.
#
#   ./run_qpid_validate.sh          # standard matrix
#   ./run_qpid_validate.sh quick    # short smoke matrix
#
# Each configuration runs in its own process so that no state (in particular the
# STM's per-thread SMR registry) carries over between them.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CPS="$(cd "$HERE/.." && pwd)"
OUT="${TMPDIR:-/tmp}/qpid_validate.$$"
CXXFLAGS="-std=c++20 -O3 -march=native -fpermissive -I$CPS"

cleanup() { rm -f "$OUT" "$OUT.abl"; }
trap cleanup EXIT

echo "building..."
g++ $CXXFLAGS -o "$OUT" "$HERE/qpid_validate.cpp" -lpthread || exit 1
# Ablation build: restores the transactional claim (no atomic claim / publish)
g++ $CXXFLAGS -DQPID_NO_ATOMIC_CLAIM -o "$OUT.abl" "$HERE/qpid_validate.cpp" -lpthread || exit 1

# threads secs prios q c strict
if [ "${1:-}" = "quick" ]; then
  CONFIGS=(
    "1 2 15 128 128 0"  "24 2 15 128 128 0"
    "1 2 15 128 128 1"  "24 2 15 128 128 1"
  )
else
  CONFIGS=(
    # chunk-batched extract_min()
    "1 2 15 128 128 0"   "12 3 15 128 128 0"   "24 3 15 128 128 0"
    "48 3 15 128 128 0"  "96 3 15 128 128 0"   "96 3 15 8 8 0"
    "24 3 100 32 32 0"   "96 3 100 128 128 0"  "48 3 1000 8 8 0"
    # strict extract_min_strict()
    "1 2 15 128 128 1"   "12 3 15 128 128 1"   "24 3 15 128 128 1"
    "48 3 15 128 128 1"  "96 3 15 128 128 1"   "96 3 15 8 8 1"
    "96 3 15 8 128 1"    "24 3 100 32 32 1"    "96 3 100 128 128 1"
    "48 3 1000 128 128 1" "96 3 1000 8 8 1"
  )
fi

fail=0; pass=0
run_matrix() {
  local bin="$1" label="$2"
  echo
  echo "=================== $label ==================="
  for cfg in "${CONFIGS[@]}"; do
    printf '%-24s ' "$cfg"
    if out=$(timeout 180 "$bin" $cfg 2>&1); then
      echo "PASS"; pass=$((pass+1))
    else
      echo "FAIL"; fail=$((fail+1))
      echo "$out" | sed 's/^/      /'
    fi
  done
}

run_matrix "$OUT"     "default build"
run_matrix "$OUT.abl" "-DQPID_NO_ATOMIC_CLAIM (transactional claim)"

echo
echo "===================================================="
echo "  $pass passed, $fail failed"
[ "$fail" -eq 0 ] || echo "  NOTE: a hang shows up as FAIL via the 180s timeout"
exit $((fail > 0))
