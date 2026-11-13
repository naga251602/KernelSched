#pragma once
// Userspace mirror of struct sched_event (bpf/kernelsched.bpf.h).
// Field order and types must match exactly; static_asserts below verify size.
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ks {

enum EventType : uint8_t {
    EVENT_RUN = 1,   // a thread got on CPU: wakeup_latency_ns is valid
    EVENT_SLICE = 2, // a thread left CPU: runtime_ns / voluntary are valid
};

struct SchedEvent {
    uint64_t timestamp_ns;
    uint32_t pid;
    uint32_t tgid;
    uint32_t cpu;
    int32_t nice;
    uint64_t wakeup_latency_ns;
    uint64_t runtime_ns;
    uint8_t voluntary;
    uint8_t type;
    uint16_t pad;
};

static_assert(sizeof(SchedEvent) == 48, "SchedEvent must match the BPF struct");
static_assert(offsetof(SchedEvent, wakeup_latency_ns) == 32,
              "SchedEvent layout drifted from bpf/kernelsched.bpf.h");
static_assert(offsetof(SchedEvent, runtime_ns) == 40,
              "SchedEvent layout drifted from bpf/kernelsched.bpf.h");
static_assert(std::is_standard_layout<SchedEvent>::value,
              "SchedEvent must be standard layout to match the BPF struct");

} // namespace ks
