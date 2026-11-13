# KernelSched

A Linux kernel scheduling profiler and experimental harness, built with
**C++20** and **eBPF**. KernelSched hooks the built-in Linux scheduler
tracepoints to measure run-queue (wakeup) latency, CPU time-slice distribution
and preemption behavior in real time, paired with a synthetic workload
generator. The goal is to observe and explain modern Linux scheduling
(EEVDF, kernel 6.6+) from empirical data rather than documentation alone.

```
+-------------------------------------------------------------------+
|                        Userspace (C++20)                          |
|                                                                   |
|  +-----------------------+       +-----------------------------+  |
|  |  Workload Generator   |       |   Telemetry & Metrics CLI   |  |
|  | - CPU-bound workers   |       | - Ring-buffer consumer      |  |
|  | - Interactive workers |       | - p50/p95/p99 latency stats |  |
|  | - nice / CPU pinning  |       | - CSV / JSON exporter       |  |
|  +-----------+-----------+       +--------------+--------------+  |
+--------------|----------------------------------|-----------------+
               | (syscalls / threads)             ^ (BPF RingBuf)
===============|==================================|=================
               v                                  |
+-------------------------------------------------------------------+
|                       Linux Kernel (6.8+)                         |
|                                                                   |
|  [sched_wakeup] ---> Record t_wakeup in BPF Hash Map (by PID)     |
|  [sched_switch] ---> Compute (t_run - t_wakeup) & CPU slice time  |
|                      Push compact event struct to Ring Buffer     |
+-------------------------------------------------------------------+
```

## Repository layout

```
kernelsched/
├── bpf/
│   ├── kernelsched.bpf.c     # eBPF program (4 scheduler tracepoints)
│   └── kernelsched.bpf.h     # shared event/map definitions
├── include/kernelsched/
│   ├── cli.h                 # CLI options
│   ├── event.h               # userspace mirror of the BPF event struct
│   ├── metrics.h             # aggregation: percentiles, Jain's index, buckets
│   ├── tracer.h              # libbpf loader + ring-buffer collector thread
│   └── workload.h            # synthetic workload generator + thread registry
├── src/
│   ├── main.cpp              # CLI, orchestration, CSV/JSON export
│   ├── tracer.cpp            # skeleton load/attach, ring_buffer__poll loop
│   ├── metrics.cpp           # percentile + fairness engine
│   └── workload.cpp          # CPU-bound and interactive workers
├── scripts/
│   ├── setup_env.sh          # installs clang/llvm/libbpf/bpftool/cmake
│   ├── exp1_noisy_neighbor.sh
│   ├── exp2_nice_weighting.sh
│   ├── exp3_pinning_vs_migration.sh
│   └── plot_results.py       # pandas/matplotlib plots (CDFs, bars)
└── CMakeLists.txt            # builds .bpf.o + skeleton + CLI binary
```

## Requirements

- Linux kernel **6.8+** with `CONFIG_DEBUG_INFO_BTF=y` (Ubuntu 24.04 LTS or
  Fedora both work out of the box). Native or a QEMU/Multipass VM.
- `clang`, `llvm`, `libbpf-dev`, `bpftool`, `cmake`, a C++20 compiler.
- Root (or `CAP_BPF` + `CAP_PERFMON`) to load the eBPF program and to use
  negative nice values.

Quick setup on Ubuntu:

```
./scripts/setup_env.sh
```

## Build

```
cmake -S . -B build
cmake --build build -j
```

The build runs three steps automatically:

1. `bpftool btf dump file /sys/kernel/btf/vmlinux format c > vmlinux.h`
   (CO-RE header generated from the running kernel),
2. `clang -target bpf` compiles `bpf/kernelsched.bpf.c` to `kernelsched.bpf.o`,
3. `bpftool gen skeleton` produces `kernelsched.skel.h`, embedded in the
   C++20 binary.

## Usage

```
# Default: 2 CPU workers + 1 interactive worker, tracing this process only.
sudo ./build/kernelsched --duration 10

# One interactive thread starved by 16 CPU-bound threads on core 2.
sudo ./build/kernelsched --cpu-workers 16 --io-workers 1 \
     --pin-cpus 2 --nice-io -5 --duration 10 --dump-events \
     --out-prefix results/demo

# Trace the whole system instead of just our own workload.
sudo ./build/kernelsched --all-system --duration 5
```

Full option list: `./build/kernelsched --help`.

Output files (prefix defaults to `results/kernelsched`):

| File | Contents |
| --- | --- |
| `<prefix>_summary.csv` | per-thread: wakeups, min/p50/p95/p99/max wakeup latency, CPU time, voluntary/involuntary switches, migrations |
| `<prefix>_timeseries.csv` | time-bucketed per-class aggregates for trend plots |
| `<prefix>_events.csv` | raw per-event dump (with `--dump-events`) |
| `<prefix>_summary.json` | machine-readable summary incl. Jain's fairness indices |

## What is measured

- **Run-queue (wakeup) latency**: `t_on_cpu - t_wakeup`. The eBPF program
  records the timestamp in a `BPF_MAP_TYPE_HASH` keyed by pid on
  `sched_wakeup` / `sched_wakeup_new`, and computes the delta when the task
  is finally dispatched on `sched_switch`.
- **CPU execution slice**: `t_off_cpu - t_on_cpu` per dispatch, from the
  `run_start` map.
- **Voluntary vs involuntary preemption**: `prev_state` in `sched_switch`.
  `prev_state == 0` means the task was still runnable, so it was preempted
  involuntarily; a nonzero state means it blocked (sleep, I/O, exit).
- **Migrations**: counted in userspace whenever a thread resumes on a
  different CPU than its previous slice.
- **Jain's Fairness Index** over total CPU runtime per class/nice group:
  `J = (sum x)^2 / (n * sum x^2)`, where 1.0 is perfectly fair and 1/n means
  one thread took everything.

## Experiments

Each script is a single command and writes CSVs plus plots under `results/`.

```
sudo ./scripts/exp1_noisy_neighbor.sh         # CORE=2 DURATION=10 ./scripts/exp1...
sudo ./scripts/exp2_nice_weighting.sh
sudo ./scripts/exp3_pinning_vs_migration.sh   # CORES=2,3 ...
python3 scripts/plot_results.py exp1 results/exp1   # also: exp2, exp3
```

1. **Noisy neighbor tail latency**: 1 interactive thread + N CPU-bound
   threads (N in {1,2,4,8,16}) pinned to one core. Expect p50/p99 wakeup
   latency to grow roughly linearly with run-queue depth, and the p99 tail to
   grow faster than the p50.
2. **Nice weighting and slice allocation**: three CPU workers at nice
   -10/0/+10 on one core. Under EEVDF weights are approximately
   `1.25^nice`, so expect a CPU share around 86% / 14% / 2% for
   (-10, 0, +10), with higher-weight threads getting longer slices before
   preemption.
3. **CPU pinning vs load balancing**: the same workload pinned to a small
   core subset vs free to migrate. Pinning usually improves interactive tail
   latency (cache warmth, no migration spikes) while the free case shows many
   more migrations.

## Connecting results to EEVDF

Since kernel 6.6 the default scheduler is EEVDF (Earliest Eligible Virtual
Deadline First), which replaced CFS:

- Every task has a **virtual runtime** that advances at a rate scaled by its
  nice weight, plus **lag** (service received minus the ideal fair share).
- A task becomes **eligible** once its lag reaches zero; among eligible tasks
  the scheduler picks the smallest **virtual deadline**.
- Shorter requested slices produce tighter virtual deadlines, which is why
  the interactive workers (short bursts + sleeps) should win the CPU even
  against many CPU-bound threads, until the run queue gets deep enough that
  their wakeup latency becomes visible in the p99 numbers from Experiment 1.
- The measured CPU share ratios in Experiment 2 should track the nice weight
  ratios, and the slice-length distribution should show that heavier-weight
  tasks run longer before being preempted.

## Troubleshooting

- **`kernelsched_bpf__load() failed`**: make sure you run with sudo, that
  `/sys/kernel/btf/vmlinux` exists, and that the sched tracepoints are
  visible under `/sys/kernel/tracing/events/sched/`.
- **`Operation not permitted` on `setpriority`**: negative nice values need
  root. The experiment scripts already use sudo.
- **VMs**: any Linux 6.8+ VM works; give it at least 2 vCPUs so Experiment 3
  has cores to migrate between.
- **No events / empty CSVs**: the default filter traces only the
  `kernelsched` process itself. Use `--all-system` to sanity-check that the
  tracer works on your kernel.

## Deliverables checklist

1. Clean CMake project building both the eBPF object (`kernelsched.bpf.o`)
   and the C++20 `kernelsched` CLI binary.
2. Reproducible benchmark scripts automating Experiments 1-3 with a single
   command each.
3. Plotting tooling (latency CDFs, fairness/CPU-share charts) and this
   write-up connecting measurements to EEVDF concepts (virtual runtime, lag,
   virtual deadlines).
