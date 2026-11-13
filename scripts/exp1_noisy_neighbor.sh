#!/usr/bin/env bash
# Experiment 1: Noisy neighbor tail latency.
#
# Pins 1 interactive thread and N CPU-bound threads (N in {1,2,4,8,16}) to a
# single CPU core and measures how p50/p99 wakeup latency degrades as the
# run-queue depth grows.
#
# Env overrides: BIN, CORE, DURATION, OUT
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./build/kernelsched}
BIN=$(readlink -f "$BIN")
CORE=${CORE:-2}
DURATION=${DURATION:-10}
OUT=${OUT:-results/exp1}
mkdir -p "$OUT"

echo "Experiment 1: noisy neighbor tail latency (all threads pinned to CPU $CORE)"
for N in 1 2 4 8 16; do
  echo "=== N=$N CPU workers + 1 interactive worker ==="
  sudo "$BIN" \
    --cpu-workers "$N" --io-workers 1 \
    --nice-cpu 0 --nice-io -5 \
    --pin-cpus "$CORE" \
    --duration "$DURATION" --interval 2 \
    --dump-events \
    --out-prefix "$OUT/n${N}"
done

echo "Plotting CDFs into $OUT/plots ..."
python3 scripts/plot_results.py exp1 "$OUT"
