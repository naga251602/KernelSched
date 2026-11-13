#include "kernelsched/workload.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>

namespace ks {

static uint32_t tid_self()
{
    return static_cast<uint32_t>(syscall(SYS_gettid));
}

static void pin_to_cpus(const std::vector<int>& cpus)
{
    if (cpus.empty())
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : cpus)
        CPU_SET(c, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: pthread_setaffinity_np failed\n");
}

static void set_nice(int nice)
{
    if (setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), nice) != 0)
        std::fprintf(stderr, "warning: setpriority(%d) failed (need root for nice < 0)\n", nice);
}

// ------------------------------- ThreadRegistry ---------------------------

void ThreadRegistry::add(uint32_t pid, std::string label, int nice)
{
    std::lock_guard<std::mutex> lk(mu_);
    threads_[pid] = ThreadInfo{pid, std::move(label), nice};
}

bool ThreadRegistry::lookup(uint32_t pid, ThreadInfo& out) const
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = threads_.find(pid);
    if (it == threads_.end())
        return false;
    out = it->second;
    return true;
}

std::vector<ThreadInfo> ThreadRegistry::snapshot() const
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<ThreadInfo> out;
    out.reserve(threads_.size());
    for (const auto& [pid, info] : threads_)
        out.push_back(info);
    return out;
}

// --------------------------------- Workload --------------------------------

Workload::Workload(const Options& opts, ThreadRegistry& registry)
    : opts_(opts), registry_(registry)
{
}

Workload::~Workload()
{
    stop();
}

void Workload::start()
{
    for (int i = 0; i < opts_.cpu_workers; ++i) {
        int nice = opts_.cpu_nice.empty() ? 0
                                          : opts_.cpu_nice[i % opts_.cpu_nice.size()];
        threads_.emplace_back(cpu_worker_main, this, nice);
    }
    for (int i = 0; i < opts_.io_workers; ++i)
        threads_.emplace_back(io_worker_main, this, opts_.io_nice);
}

void Workload::stop()
{
    stop_.store(true, std::memory_order_relaxed);
    for (auto& t : threads_) {
        if (t.joinable())
            t.join();
    }
    threads_.clear();
}

void Workload::cpu_worker_main(Workload* self, int nice)
{
    pin_to_cpus(self->opts_.pin_cpus);
    set_nice(nice);
    self->registry_.add(tid_self(), "cpu", nice);

    uint64_t x = tid_self() * 2654435761ull + 1;
    uint64_t iter = 0;
    // Tight arithmetic loop: saturates the run queue. The stop flag is only
    // checked every few thousand iterations to keep the loop tight.
    while (!self->stop_.load(std::memory_order_relaxed)) {
        for (int i = 0; i < 4096; ++i)
            x = x * 6364136223846793805ull + 1442695040888963407ull;
        asm volatile("" ::: "memory");
        if (++iter % 64 == 0)
            self->sink_.store(x, std::memory_order_relaxed);
    }
}

void Workload::io_worker_main(Workload* self, int nice)
{
    pin_to_cpus(self->opts_.pin_cpus);
    set_nice(nice);
    self->registry_.add(tid_self(), "interactive", nice);

    using namespace std::chrono;
    const auto burst = microseconds(self->opts_.burst_us);
    const auto sleep = microseconds(self->opts_.sleep_us);
    uint64_t x = tid_self() * 40503ull + 7;

    // Short compute burst (simulates handling a request), then sleep
    // (simulates waiting on I/O / the network). Every wakeup is a fresh
    // trip through the run queue, which is what we want to measure.
    while (!self->stop_.load(std::memory_order_relaxed)) {
        auto burst_end = steady_clock::now() + burst;
        while (steady_clock::now() < burst_end) {
            x = x * 6364136223846793805ull + 1442695040888963407ull;
            asm volatile("" ::: "memory");
        }
        if (self->stop_.load(std::memory_order_relaxed))
            break;
        std::this_thread::sleep_for(sleep);
    }
    self->sink_.store(x, std::memory_order_relaxed);
}

} // namespace ks
