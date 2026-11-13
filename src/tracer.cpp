#include "kernelsched/tracer.h"

#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <sys/resource.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

#include "kernelsched.skel.h"

namespace ks {

int Tracer::on_event(void* ctx, void* data, size_t size)
{
    auto* self = static_cast<Tracer*>(ctx);
    if (size < sizeof(SchedEvent))
        return 0;
    SchedEvent e;
    std::memcpy(&e, data, sizeof(e));
    if (self->cb_)
        self->cb_(e);
    return 0;
}

bool Tracer::init(uint32_t tgid, std::string& error)
{
    // Only needed on kernels < 5.11, harmless otherwise.
    struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };
    setrlimit(RLIMIT_MEMLOCK, &rl);

    libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

    auto* skel = kernelsched_bpf__open();
    if (!skel) {
        error = "kernelsched_bpf__open() failed (are you root? is CONFIG_DEBUG_INFO_BTF enabled?)";
        return false;
    }

    skel->rodata->target_tgid = static_cast<int>(tgid);

    if (int err = kernelsched_bpf__load(skel); err) {
        error = "kernelsched_bpf__load() failed: " + std::string(strerror(-err)) +
                " (check that /sys/kernel/btf/vmlinux exists and tracepoints are available)";
        kernelsched_bpf__destroy(skel);
        return false;
    }

    if (int err = kernelsched_bpf__attach(skel); err) {
        error = "kernelsched_bpf__attach() failed: " + std::string(strerror(-err));
        kernelsched_bpf__destroy(skel);
        return false;
    }

    skel_ = skel;
    return true;
}

void Tracer::start(EventCallback cb)
{
    cb_ = std::move(cb);
    auto* skel = static_cast<kernelsched_bpf*>(skel_);
    rb_ = ring_buffer__new(bpf_map__fd(skel->maps.events), on_event, this, nullptr);
    if (!rb_) {
        std::fprintf(stderr, "error: ring_buffer__new() failed\n");
        return;
    }
    running_.store(true);
    poller_ = std::thread([this] { poll_loop(); });
}

void Tracer::poll_loop()
{
    auto* rb = static_cast<ring_buffer*>(rb_);
    while (running_.load()) {
        // 100 ms timeout keeps the loop responsive to shutdown.
        int n = ring_buffer__poll(rb, 100);
        if (n < 0 && n != -EINTR) {
            std::fprintf(stderr, "warning: ring_buffer__poll() error %d, stopping collector\n", n);
            break;
        }
    }
}

Tracer::~Tracer()
{
    stop();
}

void Tracer::stop()
{
    running_.store(false);
    if (poller_.joinable())
        poller_.join();
    if (rb_) {
        ring_buffer__free(static_cast<ring_buffer*>(rb_));
        rb_ = nullptr;
    }
    if (skel_) {
        kernelsched_bpf__destroy(static_cast<kernelsched_bpf*>(skel_));
        skel_ = nullptr;
    }
}

} // namespace ks
