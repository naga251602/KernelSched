// KernelSched: Linux scheduler profiler + synthetic workload generator.
//
// Traces its own workload (or the whole system) through eBPF scheduler
// tracepoints, computes run-queue latency / fairness / preemption metrics and
// exports them as CSV / JSON for plotting.
#include <getopt.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "kernelsched/cli.h"
#include "kernelsched/metrics.h"
#include "kernelsched/tracer.h"
#include "kernelsched/workload.h"

using namespace ks;

static std::atomic<bool> g_stop{false};

static void on_signal(int)
{
    g_stop.store(true);
}

// ------------------------------- CLI parsing -------------------------------

std::string usage_text()
{
    return R"(KernelSched: Linux scheduler profiler + workload generator

Usage: kernelsched [options]

Workload:
  --cpu-workers N     CPU-bound worker threads (default 2)
  --io-workers N      interactive worker threads (default 1)
  --nice-cpu LIST     comma list of nice values for CPU workers,
                      applied round-robin (default 0). Needs root for nice < 0.
  --nice-io N         nice value for interactive workers (default 0)
  --pin-cpus LIST     comma list of CPU ids to pin all workers to (default: free)
  --burst-us US       interactive worker compute burst (default 2000)
  --sleep-us US       interactive worker sleep between bursts (default 8000)

Tracing / output:
  --duration SEC      seconds to run (default 10)
  --interval SEC      live summary print interval (default 2)
  --all-system        trace all processes, not just this one
  --dump-events       write every raw event to <prefix>_events.csv
  --out-prefix PATH   prefix for output files (default results/kernelsched)
  --no-json           skip the JSON summary
  -h, --help          show this help
)";
}

static std::vector<int> parse_int_list(const std::string& s, const char* flag)
{
    std::vector<int> out;
    std::string item;
    std::istringstream ss(s);
    while (std::getline(ss, item, ',')) {
        try {
            out.push_back(std::stoi(item));
        } catch (...) {
            std::fprintf(stderr, "error: bad integer '%s' in --%s\n", item.c_str(), flag);
            std::exit(2);
        }
    }
    if (out.empty()) {
        std::fprintf(stderr, "error: --%s needs at least one value\n", flag);
        std::exit(2);
    }
    return out;
}

Options parse_args(int argc, char** argv)
{
    Options o;
    static option long_opts[] = {
        {"cpu-workers", required_argument, nullptr, 'c'},
        {"io-workers", required_argument, nullptr, 'i'},
        {"nice-cpu", required_argument, nullptr, 'n'},
        {"nice-io", required_argument, nullptr, 'N'},
        {"pin-cpus", required_argument, nullptr, 'p'},
        {"duration", required_argument, nullptr, 'd'},
        {"interval", required_argument, nullptr, 't'},
        {"burst-us", required_argument, nullptr, 'b'},
        {"sleep-us", required_argument, nullptr, 's'},
        {"all-system", no_argument, nullptr, 'A'},
        {"dump-events", no_argument, nullptr, 'E'},
        {"out-prefix", required_argument, nullptr, 'o'},
        {"no-json", no_argument, nullptr, 'J'},
        {"help", no_argument, nullptr, 'h'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "c:i:n:N:p:d:t:b:s:AEo:Jh", long_opts, nullptr)) != -1) {
        switch (c) {
        case 'c': o.cpu_workers = std::atoi(optarg); break;
        case 'i': o.io_workers = std::atoi(optarg); break;
        case 'n': o.cpu_nice = parse_int_list(optarg, "nice-cpu"); break;
        case 'N': o.io_nice = std::atoi(optarg); break;
        case 'p': o.pin_cpus = parse_int_list(optarg, "pin-cpus"); break;
        case 'd': o.duration_sec = std::atof(optarg); break;
        case 't': o.interval_sec = std::atof(optarg); break;
        case 'b': o.burst_us = std::strtoull(optarg, nullptr, 10); break;
        case 's': o.sleep_us = std::strtoull(optarg, nullptr, 10); break;
        case 'A': o.trace_all = true; break;
        case 'E': o.dump_events = true; break;
        case 'o': o.out_prefix = optarg; break;
        case 'J': o.json_summary = false; break;
        case 'h': std::fputs(usage_text().c_str(), stdout); std::exit(0);
        default: std::fputs(usage_text().c_str(), stderr); std::exit(2);
        }
    }
    if (o.cpu_workers < 0 || o.io_workers < 0) {
        std::fprintf(stderr, "error: worker counts must be >= 0\n");
        std::exit(2);
    }
    if (o.cpu_workers + o.io_workers == 0) {
        std::fprintf(stderr, "error: at least one worker thread is required\n");
        std::exit(2);
    }
    return o;
}

// ------------------------------ Output helpers -----------------------------

static void print_table(const std::vector<ThreadSummary>& rows)
{
    std::printf("\n%-12s %7s %5s %8s %10s %10s %10s %10s %10s %8s\n",
                "LABEL", "PID", "NICE", "WAKEUPS", "P50_US", "P95_US", "P99_US",
                "MAX_US", "CPU_MS", "MIGR");
    for (const auto& r : rows) {
        std::printf("%-12s %7u %5d %8llu %10.1f %10.1f %10.1f %10.1f %10.1f %8llu\n",
                    r.label.c_str(), r.pid, r.nice,
                    (unsigned long long)r.wakeup.count,
                    r.wakeup.p50_ns / 1e3, r.wakeup.p95_ns / 1e3,
                    r.wakeup.p99_ns / 1e3, r.wakeup.max_ns / 1e3,
                    r.cpu_time_ms, (unsigned long long)r.migrations);
    }
    std::fflush(stdout);
}

static void write_summary_csv(const std::string& path, const std::vector<ThreadSummary>& rows)
{
    std::ofstream f(path);
    f << "pid,label,nice,wakeups,wakeup_min_us,wakeup_p50_us,wakeup_p95_us,"
         "wakeup_p99_us,wakeup_max_us,wakeup_mean_us,cpu_time_ms,"
         "voluntary_switches,involuntary_switches,migrations\n";
    for (const auto& r : rows) {
        f << r.pid << ',' << r.label << ',' << r.nice << ',' << r.wakeup.count << ','
          << r.wakeup.min_ns / 1e3 << ',' << r.wakeup.p50_ns / 1e3 << ','
          << r.wakeup.p95_ns / 1e3 << ',' << r.wakeup.p99_ns / 1e3 << ','
          << r.wakeup.max_ns / 1e3 << ',' << r.wakeup.mean_ns / 1e3 << ','
          << r.cpu_time_ms << ',' << r.voluntary_switches << ','
          << r.involuntary_switches << ',' << r.migrations << '\n';
    }
}

static void write_timeseries_csv(const std::string& path, const std::vector<BucketRow>& rows)
{
    std::ofstream f(path);
    f << "bucket_start_s,label,wakeups,avg_wakeup_us,max_wakeup_us,cpu_time_ms,switches\n";
    for (const auto& r : rows) {
        f << r.bucket_start_s << ',' << r.label << ',' << r.wakeups << ','
          << r.avg_wakeup_us << ',' << r.max_wakeup_us << ',' << r.cpu_time_ms << ','
          << r.switches << '\n';
    }
}

static void write_events_csv(const std::string& path, const std::vector<SchedEvent>& events)
{
    std::ofstream f(path);
    f << "timestamp_ns,pid,tgid,cpu,nice,type,wakeup_latency_ns,runtime_ns,voluntary\n";
    for (const auto& e : events) {
        f << e.timestamp_ns << ',' << e.pid << ',' << e.tgid << ',' << e.cpu << ','
          << e.nice << ',' << static_cast<int>(e.type) << ',' << e.wakeup_latency_ns
          << ',' << e.runtime_ns << ',' << static_cast<int>(e.voluntary) << '\n';
    }
}

static void write_json(const std::string& path, uint32_t tgid, double duration_sec,
                       const std::vector<ThreadSummary>& rows, const LatencyStats& overall,
                       const std::vector<std::pair<std::string, double>>& jain)
{
    std::ofstream f(path);
    f << std::setprecision(6);
    f << "{\n";
    f << "  \"target_tgid\": " << tgid << ",\n";
    f << "  \"duration_sec\": " << duration_sec << ",\n";
    f << "  \"overall_wakeup_latency\": {\"count\": " << overall.count
      << ", \"mean_ns\": " << overall.mean_ns << ", \"p50_ns\": " << overall.p50_ns
      << ", \"p95_ns\": " << overall.p95_ns << ", \"p99_ns\": " << overall.p99_ns
      << ", \"max_ns\": " << overall.max_ns << "},\n";
    f << "  \"jain_fairness\": [";
    for (size_t i = 0; i < jain.size(); ++i) {
        f << (i ? ", " : "") << "{\"group\": \"" << jain[i].first
          << "\", \"index\": " << jain[i].second << "}";
    }
    f << "],\n";
    f << "  \"threads\": [\n";
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& r = rows[i];
        f << "    {\"pid\": " << r.pid << ", \"label\": \"" << r.label
          << "\", \"nice\": " << r.nice << ", \"wakeups\": " << r.wakeup.count
          << ", \"wakeup_p50_ns\": " << r.wakeup.p50_ns
          << ", \"wakeup_p95_ns\": " << r.wakeup.p95_ns
          << ", \"wakeup_p99_ns\": " << r.wakeup.p99_ns
          << ", \"wakeup_max_ns\": " << r.wakeup.max_ns
          << ", \"cpu_time_ms\": " << r.cpu_time_ms
          << ", \"voluntary_switches\": " << r.voluntary_switches
          << ", \"involuntary_switches\": " << r.involuntary_switches
          << ", \"migrations\": " << r.migrations << "}"
          << (i + 1 < rows.size() ? "," : "") << "\n";
    }
    f << "  ]\n}\n";
}

// ----------------------------------- main ----------------------------------

int main(int argc, char** argv)
{
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    Options o = parse_args(argc, argv);
    const uint32_t tgid = o.trace_all ? 0 : static_cast<uint32_t>(getpid());

    std::printf("KernelSched: tracing %s for %.1fs\n",
                o.trace_all ? "the whole system" : ("pid " + std::to_string(tgid)).c_str(),
                o.duration_sec);
    std::printf("  workload: %d cpu workers (nice", o.cpu_workers);
    for (int n : o.cpu_nice) std::printf(" %d", n);
    std::printf("), %d interactive workers (nice %d), burst %lluus, sleep %lluus\n",
                o.io_workers, o.io_nice, (unsigned long long)o.burst_us,
                (unsigned long long)o.sleep_us);
    if (!o.pin_cpus.empty()) {
        std::printf("  pinning to cpus:");
        for (int c : o.pin_cpus) std::printf(" %d", c);
        std::printf("\n");
    }
    if (geteuid() != 0)
        std::printf("  note: not running as root; eBPF may fail and nice < 0 is ignored\n");

    // 1. Attach the eBPF tracer before spawning the workload so we catch
    //    every wakeup from the very first thread.
    Tracer tracer;
    std::string err;
    if (!tracer.init(tgid, err)) {
        std::fprintf(stderr, "error: %s\n", err);
        return 1;
    }

    ThreadRegistry registry;
    MetricsCollector metrics(registry);
    metrics.set_bucket_seconds(o.interval_sec);
    metrics.set_dump_events(o.dump_events);
    tracer.start([&metrics](const SchedEvent& e) { metrics.record(e); });

    // 2. Run the workload, printing live summaries.
    Workload workload(o, registry);
    workload.start();

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double>(o.duration_sec));
    while (!g_stop.load()) {
        std::this_thread::sleep_for(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(std::max(0.1, o.interval_sec))));
        print_table(metrics.summaries());
        if (std::chrono::steady_clock::now() >= deadline)
            break;
    }

    // 3. Shut down and export.
    workload.stop();
    tracer.stop();

    const auto rows = metrics.summaries();
    const auto overall = metrics.overall_wakeup_latency();
    const auto jain = metrics.jain_by_group();

    std::printf("\n===== final summary =====\n");
    print_table(rows);

    std::printf("\nJain's Fairness Index (per class/nice group, 1.0 = perfectly fair):\n");
    for (const auto& [group, j] : jain)
        std::printf("  %-24s %.4f\n", group.c_str(), j);

    std::printf("\nOverall wakeup latency: count=%llu mean=%.1fus p50=%.1fus p95=%.1fus p99=%.1fus max=%.1fus\n",
                (unsigned long long)overall.count, overall.mean_ns / 1e3,
                overall.p50_ns / 1e3, overall.p95_ns / 1e3, overall.p99_ns / 1e3,
                overall.max_ns / 1e3);

    const auto ensure_parent = [](const std::string& path) {
        if (auto pos = path.find_last_of('/'); pos != std::string::npos) {
            std::string dir = path.substr(0, pos);
            std::string cmd = "mkdir -p '" + dir + "'";
            if (std::system(cmd.c_str()) != 0) { /* best effort */ }
        }
    };
    ensure_parent(o.out_prefix);

    write_summary_csv(o.out_prefix + "_summary.csv", rows);
    write_timeseries_csv(o.out_prefix + "_timeseries.csv", metrics.buckets());
    if (o.dump_events)
        write_events_csv(o.out_prefix + "_events.csv", metrics.dumped_events());
    if (o.json_summary)
        write_json(o.out_prefix + "_summary.json", tgid, o.duration_sec, rows, overall, jain);

    std::printf("\nWrote: %s_summary.csv, %s_timeseries.csv", o.out_prefix.c_str(),
                o.out_prefix.c_str());
    if (o.dump_events)
        std::printf(", %s_events.csv", o.out_prefix.c_str());
    if (o.json_summary)
        std::printf(", %s_summary.json", o.out_prefix.c_str());
    std::printf("\n");
    return 0;
}
