#include "kernelsched/metrics.h"

#include <algorithm>
#include <cmath>

namespace ks {

uint64_t percentile_ns(std::vector<uint64_t> samples, double p)
{
    if (samples.empty())
        return 0;
    std::sort(samples.begin(), samples.end());
    size_t idx = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(samples.size())));
    if (idx == 0)
        idx = 1;
    if (idx > samples.size())
        idx = samples.size();
    return samples[idx - 1];
}

static LatencyStats stats_from(std::vector<uint64_t> samples)
{
    LatencyStats s;
    if (samples.empty())
        return s;
    std::sort(samples.begin(), samples.end());
    s.count = samples.size();
    s.min_ns = samples.front();
    s.max_ns = samples.back();
    s.p50_ns = percentile_ns(samples, 50.0);
    s.p95_ns = percentile_ns(samples, 95.0);
    s.p99_ns = percentile_ns(samples, 99.0);
    double sum = 0.0;
    for (uint64_t v : samples)
        sum += static_cast<double>(v);
    s.mean_ns = sum / static_cast<double>(samples.size());
    return s;
}

void MetricsCollector::record(const SchedEvent& e)
{
    std::lock_guard<std::mutex> lk(mu_);

    ThreadAcc& st = threads_[e.pid];
    ThreadInfo info;
    if (registry_.lookup(e.pid, info)) {
        st.label = info.label;
        st.nice = info.nice;
    }

    if (e.type == EVENT_RUN) {
        st.wakeup_latencies.push_back(e.wakeup_latency_ns);
    } else if (e.type == EVENT_SLICE) {
        st.total_runtime_ns += e.runtime_ns;
        if (e.voluntary)
            ++st.voluntary;
        else
            ++st.involuntary;
        if (st.last_cpu != 0xFFFFFFFFu && st.last_cpu != e.cpu)
            ++st.migrations;
        st.last_cpu = e.cpu;
    }

    // Time-bucketed aggregates (bucket width = bucket_sec_).
    const uint64_t bucket_width_ns =
        static_cast<uint64_t>(bucket_sec_ * 1e9);
    const uint64_t bucket = bucket_width_ns ? e.timestamp_ns / bucket_width_ns : 0;
    BucketAcc& b = buckets_[bucket][st.label];
    if (e.type == EVENT_RUN) {
        ++b.wakeups;
        b.lat_sum_ns += static_cast<double>(e.wakeup_latency_ns);
        b.lat_max_ns = std::max(b.lat_max_ns, static_cast<double>(e.wakeup_latency_ns));
    } else {
        b.cpu_ns += static_cast<double>(e.runtime_ns);
        ++b.switches;
    }

    if (dump_events_)
        events_.push_back(e);
}

std::vector<ThreadSummary> MetricsCollector::summaries() const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ThreadSummary> out;
    out.reserve(threads_.size());
    for (auto& [pid, st] : threads_) {
        ThreadSummary s;
        s.pid = pid;
        s.label = st.label;
        s.nice = st.nice;
        // Threads register themselves right after they first run, so the
        // very first events may predate registration; re-resolve now.
        ThreadInfo info;
        if (s.label == "other" && registry_.lookup(pid, info)) {
            s.label = info.label;
            s.nice = info.nice;
        }
        s.wakeup = stats_from(st.wakeup_latencies);
        s.cpu_time_ms = static_cast<double>(st.total_runtime_ns) / 1e6;
        s.voluntary_switches = st.voluntary;
        s.involuntary_switches = st.involuntary;
        s.migrations = st.migrations;
        out.push_back(s);
    }
    return out;
}

LatencyStats MetricsCollector::overall_wakeup_latency() const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<uint64_t> all;
    size_t total = 0;
    for (auto& [pid, st] : threads_)
        total += st.wakeup_latencies.size();
    all.reserve(total);
    for (auto& [pid, st] : threads_)
        all.insert(all.end(), st.wakeup_latencies.begin(), st.wakeup_latencies.end());
    return stats_from(std::move(all));
}

std::vector<std::pair<std::string, double>> MetricsCollector::jain_by_group() const
{
    std::lock_guard<std::mutex> lk(mu_);

    // x_i = total CPU runtime of thread i (ms); group by (label, nice).
    std::map<std::string, std::vector<double>> groups;
    for (auto& [pid, st] : threads_) {
        std::string key = st.label + "/nice=" + std::to_string(st.nice);
        groups[key].push_back(static_cast<double>(st.total_runtime_ns) / 1e6);
    }

    std::vector<std::pair<std::string, double>> out;
    for (auto& [key, xs] : groups) {
        double sum = 0.0, sq = 0.0;
        for (double x : xs) {
            sum += x;
            sq += x * x;
        }
        const double n = static_cast<double>(xs.size());
        const double j = sq > 0.0 ? (sum * sum) / (n * sq) : 0.0;
        out.emplace_back(key, j);
    }
    return out;
}

std::vector<BucketRow> MetricsCollector::buckets() const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<BucketRow> out;
    for (auto& [bucket, per_label] : buckets_) {
        for (auto& [label, b] : per_label) {
            BucketRow r;
            r.bucket_start_s = static_cast<double>(bucket) * bucket_sec_;
            r.label = label;
            r.wakeups = b.wakeups;
            r.avg_wakeup_us = b.wakeups ? (b.lat_sum_ns / b.wakeups) / 1e3 : 0.0;
            r.max_wakeup_us = b.lat_max_ns / 1e3;
            r.cpu_time_ms = b.cpu_ns / 1e6;
            r.switches = b.switches;
            out.push_back(r);
        }
    }
    return out;
}

std::vector<SchedEvent> MetricsCollector::dumped_events() const
{
    std::lock_guard<std::mutex> lk(mu_);
    return events_;
}

} // namespace ks
