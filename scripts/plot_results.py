#!/usr/bin/env python3
"""Plot KernelSched experiment results (pandas + matplotlib).

Usage:
    python3 scripts/plot_results.py exp1 results/exp1
    python3 scripts/plot_results.py exp2 results/exp2
    python3 scripts/plot_results.py exp3 results/exp3

Plots are written into <results_dir>/plots/.
"""
import sys
from pathlib import Path

import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


def outdir(results_dir: Path) -> Path:
    d = results_dir / "plots"
    d.mkdir(parents=True, exist_ok=True)
    return d


def load_summary(prefix: Path) -> pd.DataFrame:
    return pd.read_csv(f"{prefix}_summary.csv")


def load_events(prefix: Path) -> pd.DataFrame:
    return pd.read_csv(f"{prefix}_events.csv")


# --------------------------------------------------------------------- exp1
def plot_exp1(results_dir: Path) -> None:
    """CDF of wakeup latency per contention level (N CPU workers)."""
    fig, ax = plt.subplots(figsize=(8, 5))
    found = False
    for n in [1, 2, 4, 8, 16]:
        prefix = results_dir / f"n{n}"
        events_path = Path(f"{prefix}_events.csv")
        if not events_path.exists():
            print(f"  (skipping n={n}: {events_path} not found)")
            continue
        found = True
        df = load_events(prefix)
        lat = df.loc[df["type"] == 1, "wakeup_latency_ns"].sort_values() / 1e3  # us
        if lat.empty:
            continue
        cdf = (pd.Series(range(1, len(lat) + 1)) / len(lat)).values
        ax.plot(lat.values, cdf, label=f"N={n} CPU workers (p99={lat.quantile(0.99):.0f}us)")

    if not found:
        print("No exp1 event files found; run scripts/exp1_noisy_neighbor.sh first.")
        return
    ax.set_xscale("log")
    ax.set_xlabel("Wakeup (run-queue) latency [us, log scale]")
    ax.set_ylabel("CDF")
    ax.set_title("Noisy neighbor: wakeup latency vs run-queue depth (1 core)")
    ax.grid(True, alpha=0.3)
    ax.legend()
    path = outdir(results_dir) / "exp1_latency_cdf.png"
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    print(f"wrote {path}")


# --------------------------------------------------------------------- exp2
def plot_exp2(results_dir: Path) -> None:
    """Stacked bar of CPU share per nice value + mean slice length."""
    prefix = results_dir / "run"
    df = load_summary(prefix)
    cpu = df[df["label"] == "cpu"].copy()
    if cpu.empty:
        print("No cpu workers found in exp2 summary.")
        return

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 5))

    share = cpu["cpu_time_ms"] / cpu["cpu_time_ms"].sum() * 100.0
    bottom = 0.0
    for nice, frac in zip(cpu["nice"], share):
        ax1.bar(["CPU share"], [frac], bottom=bottom, label=f"nice {nice}")
        ax1.text(0, bottom + frac / 2, f"{frac:.1f}%", ha="center", va="center")
        bottom += frac
    ax1.set_ylabel("Share of measured CPU time [%]")
    ax1.set_title("CPU share by nice value (EEVDF weights)")
    ax1.legend()
    ax1.grid(True, axis="y", alpha=0.3)

    # Mean slice length: cpu_time / number of switches per thread.
    switches = cpu["voluntary_switches"] + cpu["involuntary_switches"]
    mean_slice_ms = cpu["cpu_time_ms"] / switches.replace(0, pd.NA)
    ax2.bar([str(n) for n in cpu["nice"]], mean_slice_ms.astype(float))
    ax2.set_xlabel("nice value")
    ax2.set_ylabel("Mean slice length [ms]")
    ax2.set_title("Mean CPU slice before preemption")
    ax2.grid(True, axis="y", alpha=0.3)

    path = outdir(results_dir) / "exp2_nice_share.png"
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    print(f"wrote {path}")


# --------------------------------------------------------------------- exp3
def plot_exp3(results_dir: Path) -> None:
    """Pinned vs free: p50/p99 wakeup latency and migration counts."""
    pinned = load_summary(results_dir / "pinned")
    free = load_summary(results_dir / "free")

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 5))
    for df, name, color in [(pinned, "pinned", "#1f77b4"), (free, "free", "#d62728")]:
        inter = df[df["label"] == "interactive"]
        ax1.bar([f"{name} p50", f"{name} p99"],
                [inter["wakeup_p50_us"].mean(), inter["wakeup_p99_us"].mean()],
                color=color, alpha=0.8)
        ax2.bar([name], [df["migrations"].sum()], color=color, alpha=0.8)

    ax1.set_ylabel("Interactive wakeup latency [us]")
    ax1.set_title("Tail latency: pinned vs free migration")
    ax1.grid(True, axis="y", alpha=0.3)
    ax2.set_ylabel("CPU migrations [count]")
    ax2.set_title("Migration frequency")
    ax2.grid(True, axis="y", alpha=0.3)

    path = outdir(results_dir) / "exp3_pinning_vs_free.png"
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    print(f"wrote {path}")


def main() -> None:
    if len(sys.argv) != 3 or sys.argv[1] not in {"exp1", "exp2", "exp3"}:
        print(__doc__)
        sys.exit(2)
    results_dir = Path(sys.argv[2])
    {"exp1": plot_exp1, "exp2": plot_exp2, "exp3": plot_exp3}[sys.argv[1]](results_dir)


if __name__ == "__main__":
    main()
