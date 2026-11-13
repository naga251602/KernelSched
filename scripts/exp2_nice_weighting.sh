#!/usr/bin/env bash
# Experiment 2: Priority (nice) weighting and slice allocation.
#
# Runs 3 competing CPU-bound threads at nice -10, 0 and +10 on the same core
# and measures actual CPU time ratios and average slice lengths before
# preemption. Under EEVDF the CPU share should follow the weight ratios
# (weights ~ 1.25^nice, so -10 vs 0 vs +10 gives roughly a 6.2x : 1x : 0.16x
# split between adjacent pairs).
#
# Env overrides: BIN, CORE, DURATION, OUT
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./build/kernelsched}
BIN=$(readlink -f "$BIN")
CORE=${CORE:-2}
DURATION=${DURATION:-10}
OUT=${OUT:-results/exp2}
mkdir -p "$OUT"

echo "Experiment 2: nice weighting (nice -10, 0, +10 pinned to CPU $CORE)"
sudo "$BIN" \
  --cpu-workers 3 --io-workers 0 \
  --nice-cpu "-10,0,10" \
  --pin-cpus "$CORE" \
  --duration "$DURATION" --interval 2 \
  --out-prefix "$OUT/run"

echo "Plotting CPU share into $OUT/plots ..."
python3 scripts/plot_results.py exp2 "$OUT"
