#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ks {

struct Options {
    int cpu_workers = 2;             // number of CPU-bound worker threads
    int io_workers = 1;              // number of interactive worker threads
    std::vector<int> cpu_nice{0};    // nice values, applied round-robin to CPU workers
    int io_nice = 0;                 // nice value for interactive workers
    std::vector<int> pin_cpus;       // CPU ids to pin all workers to (empty = free)
    double duration_sec = 10.0;      // how long to run
    double interval_sec = 2.0;       // live summary print interval
    uint64_t burst_us = 2000;        // interactive worker compute burst
    uint64_t sleep_us = 8000;        // interactive worker sleep between bursts
    bool trace_all = false;          // trace whole system instead of this process
    bool dump_events = false;        // write every raw event to <prefix>_events.csv
    bool json_summary = true;        // write <prefix>_summary.json
    std::string out_prefix = "results/kernelsched";
};

// Parses argv; prints usage and exits (code 0 for -h, 2 for bad input).
Options parse_args(int argc, char** argv);
std::string usage_text();

} // namespace ks
