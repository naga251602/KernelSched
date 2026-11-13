#!/usr/bin/env bash
# Experiment 3: CPU pinning vs kernel load balancing.
#
# Runs the same workload twice: once pinned to a small subset of cores, once
# free to migrate across all cores. Compares tail latency and migration
# frequency (a migration is counted whenever a thread continues running on a
# different CPU than its previous slice).
#
# Env overrides: BIN, CORES, DURATION, OUT
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=${BIN:-./build/kernelsched}
BIN=$(readlink -f "$BIN")
CORES=${CORES:-2,3}
DURATION=${DURATION:-10}
OUT=${OUT:-results/exp3}
mkdir -p "$OUT"

echo "Experiment 3: pinning (CPUs $CORES) vs free migration"
echo "=== pinned to CPUs $CORES ==="
sudo "$BIN" \
  --cpu-workers 4 --io-workers 2 \
  --pin-cpus "$CORES" \
  --duration "$DURATION" --interval 2 \
  --out-prefix "$OUT/pinned"

echo "=== free to migrate across all cores ==="
sudo "$BIN" \
  --cpu-workers 4 --io-workers 2 \
  --duration "$DURATION" --interval 2 \
  --out-prefix "$OUT/free"

echo "Plotting comparison into $OUT/plots ..."
python3 scripts/plot_results.py exp3 "$OUT"
