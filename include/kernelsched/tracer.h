#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include "event.h"

namespace ks {

using EventCallback = std::function<void(const SchedEvent&)>;

// Loads the eBPF program, attaches it to the scheduler tracepoints and pumps
// the ring buffer from a dedicated collector thread.
class Tracer {
public:
    Tracer() = default;
    ~Tracer();
    Tracer(const Tracer&) = delete;
    Tracer& operator=(const Tracer&) = delete;

    // tgid == 0 disables per-process filtering (system-wide tracing).
    // Returns false and fills `error` on failure (usually: not root, or
    // missing kernel BTF support).
    bool init(uint32_t tgid, std::string& error);

    void start(EventCallback cb);
    void stop();

private:
    void poll_loop();
    static int on_event(void* ctx, void* data, size_t size);

    std::thread poller_;
    std::atomic<bool> running_{false};
    EventCallback cb_;
    void* skel_ = nullptr; // kernelsched_bpf* (opaque; skeleton header is included in the .cpp)
    void* rb_ = nullptr;   // ring_buffer*
};

} // namespace ks
