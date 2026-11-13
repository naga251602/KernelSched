#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "cli.h"

namespace ks {

struct ThreadInfo {
    uint32_t pid = 0;
    std::string label; // "cpu" or "interactive"
    int nice = 0;
};

// Registry of workload threads, shared with the metrics collector so that
// every traced pid can be labeled with its workload class and nice value.
class ThreadRegistry {
public:
    void add(uint32_t pid, std::string label, int nice);
    bool lookup(uint32_t pid, ThreadInfo& out) const;
    std::vector<ThreadInfo> snapshot() const;

private:
    mutable std::mutex mu_;
    std::map<uint32_t, ThreadInfo> threads_;
};

// Spawns controlled multi-threaded workloads:
//  - CPU-bound workers: tight arithmetic loops that saturate run queues.
//  - Interactive workers: short compute bursts followed by sleeps, to
//    simulate latency-sensitive RPC/web request threads.
class Workload {
public:
    Workload(const Options& opts, ThreadRegistry& registry);
    ~Workload();
    Workload(const Workload&) = delete;
    Workload& operator=(const Workload&) = delete;

    void start(); // spawns all workers
    void stop();  // signals stop and joins every worker

private:
    void spawn_cpu_worker(int nice);
    void spawn_io_worker(int nice);
    static void cpu_worker_main(Workload* self, int nice);
    static void io_worker_main(Workload* self, int nice);

    Options opts_;
    ThreadRegistry& registry_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> sink_{0}; // keeps the burn loop observable
    std::vector<std::thread> threads_;
};

} // namespace ks
