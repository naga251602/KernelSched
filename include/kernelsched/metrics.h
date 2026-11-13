#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include "event.h"
#include "workload.h"

namespace ks {

struct LatencyStats {
    uint64_t count = 0;
    double mean_ns = 0.0;
    uint64_t min_ns = 0;
    uint64_t p50_ns = 0;
    uint64_t p95_ns = 0;
    uint64_t p99_ns = 0;
    uint64_t max_ns = 0;
};

struct ThreadSummary {
    uint32_t pid = 0;
    std::string label = "other";
    int nice = 0;
    LatencyStats wakeup;
    double cpu_time_ms = 0.0;
    uint64_t voluntary_switches = 0;
    uint64_t involuntary_switches = 0;
    uint64_t migrations = 0;
};

struct BucketRow {
    double bucket_start_s = 0.0;
    std::string label;
    uint64_t wakeups = 0;
    double avg_wakeup_us = 0.0;
    double max_wakeup_us = 0.0;
    double cpu_time_ms = 0.0;
    uint64_t switches = 0;
};

// Aggregates raw trace events into per-thread latency/fairness metrics.
// All methods are thread-safe; the tracer's collector thread is the writer.
class MetricsCollector {
public:
    explicit MetricsCollector(ThreadRegistry& registry) : registry_(registry) {}

    void record(const SchedEvent& e);
    void set_bucket_seconds(double s) { bucket_sec_ = s; }
    void set_dump_events(bool d) { dump_events_ = d; }

    std::vector<ThreadSummary> summaries() const;
    LatencyStats overall_wakeup_latency() const;

    // Jain's Fairness Index over CPU runtime, computed per group of threads
    // that share the same workload class and nice value. J = 1.0 is perfect
    // fairness, J = 1/n is complete starvation of all but one thread.
    std::vector<std::pair<std::string, double>> jain_by_group() const;

    std::vector<BucketRow> buckets() const;
    std::vector<SchedEvent> dumped_events() const;

private:
    struct ThreadAcc {
        std::string label = "other";
        int nice = 0;
        std::vector<uint64_t> wakeup_latencies; // ns
        uint64_t total_runtime_ns = 0;
        uint64_t voluntary = 0;
        uint64_t involuntary = 0;
        uint32_t last_cpu = 0xFFFFFFFFu;
        uint64_t migrations = 0;
    };
    struct BucketAcc {
        uint64_t wakeups = 0;
        double lat_sum_ns = 0.0;
        double lat_max_ns = 0.0;
        double cpu_ns = 0.0;
        uint64_t switches = 0;
    };

    ThreadRegistry& registry_;
    mutable std::mutex mu_;
    std::map<uint32_t, ThreadAcc> threads_;
    std::map<uint64_t, std::map<std::string, BucketAcc>> buckets_;
    double bucket_sec_ = 1.0;
    bool dump_events_ = false;
    std::vector<SchedEvent> events_;
};

// Nearest-rank percentile over a copy of the given samples (ns).
uint64_t percentile_ns(std::vector<uint64_t> samples, double p);

} // namespace ks
