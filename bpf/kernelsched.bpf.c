// SPDX-License-Identifier: GPL-2.0
/*
 * kernelsched.bpf.c - eBPF scheduler tracer for KernelSched.
 *
 * Hooks four built-in scheduler tracepoints:
 *   sched_wakeup / sched_wakeup_new : remember when a task became runnable
 *   sched_switch                    : compute run-queue (wakeup) latency and
 *                                     CPU execution slices
 *   sched_process_exit              : clean up stale map entries
 *
 * Events are streamed to userspace through a BPF ring buffer and can be
 * filtered by target tgid inside the kernel to drop system-wide noise.
 */
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "kernelsched.bpf.h"

char LICENSE[] SEC("license") = "GPL";

/* Set from userspace before load. 0 = trace everything (system-wide). */
const volatile pid_t target_tgid = 0;

/* pid -> when the task became runnable (plus tgid/nice of the task). */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, __u32);
	__type(value, struct wrec);
} wakeup_ts SEC(".maps");

/* pid -> timestamp of the last switch-in (start of the current CPU slice). */
struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__uint(max_entries, 65536);
	__type(key, __u32);
	__type(value, __u64);
} run_start SEC(".maps");

/* Fixed-size events streamed to the userspace collector (8 MiB). */
struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 1 << 23);
} events SEC(".maps");

static __always_inline bool tgid_matches(__u32 tgid)
{
	return target_tgid == 0 || tgid == target_tgid;
}

/*
 * sched_wakeup / sched_wakeup_new: a task just became runnable.
 * current is the waker here, so we look the woken task up by pid to learn
 * its tgid for the in-kernel filter.
 */
static int handle_wakeup(struct trace_event_raw_sched_wakeup_template *ctx)
{
	struct wrec rec = {};
	__u32 pid = (__u32)ctx->pid;
	__u64 ts = bpf_ktime_get_ns();
	struct task_struct *p;

	p = bpf_task_from_pid(pid);
	if (!p)
		return 0;
	rec.tgid = BPF_CORE_READ(p, tgid);
	bpf_task_put(p);

	if (!tgid_matches(rec.tgid))
		return 0;

	rec.ts = ts;
	/* For SCHED_NORMAL tasks prio == nice + 120. */
	rec.nice = (__s32)ctx->prio - 120;
	bpf_map_update_elem(&wakeup_ts, &pid, &rec, BPF_ANY);
	return 0;
}

SEC("tp/sched/sched_wakeup")
int sched_wakeup(struct trace_event_raw_sched_wakeup_template *ctx)
{
	return handle_wakeup(ctx);
}

SEC("tp/sched/sched_wakeup_new")
int sched_wakeup_new(struct trace_event_raw_sched_wakeup_template *ctx)
{
	return handle_wakeup(ctx);
}

/*
 * sched_switch: prev stopped running, next is about to run.
 *  - next: look up the wakeup timestamp, emit wakeup latency, then remember
 *          the run start so the slice length can be measured later.
 *  - prev: compute the CPU execution slice and whether the switch out was
 *          voluntary (prev_state != 0, i.e. the task blocked/exited) or an
 *          involuntary preemption (prev_state == 0).
 */
SEC("tp/sched/sched_switch")
int sched_switch(struct trace_event_raw_sched_switch *ctx)
{
	__u64 now = bpf_ktime_get_ns();
	__u32 next_pid = (__u32)ctx->next_pid;
	__u32 prev_pid = (__u32)ctx->prev_pid;
	struct sched_event *e;
	struct wrec *w;

	/* --- next: run-queue (wakeup) latency --- */
	w = bpf_map_lookup_elem(&wakeup_ts, &next_pid);
	if (w) {
		if (tgid_matches(w->tgid)) {
			e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
			if (e) {
				e->timestamp_ns = now;
				e->pid = next_pid;
				e->tgid = w->tgid;
				e->cpu = (__u32)bpf_get_smp_processor_id();
				e->nice = w->nice;
				e->wakeup_latency_ns = now - w->ts;
				e->runtime_ns = 0;
				e->voluntary = 0;
				e->type = EVENT_RUN;
				bpf_ringbuf_submit(e, 0);
			}
		}
		bpf_map_delete_elem(&wakeup_ts, &next_pid);
	}

	/* Remember when `next` got on CPU so its slice can be measured. */
	if (next_pid)
		bpf_map_update_elem(&run_start, &next_pid, &now, BPF_ANY);

	/* --- prev: CPU execution slice --- */
	if (prev_pid) {
		__u64 *rs = bpf_map_lookup_elem(&run_start, &prev_pid);

		if (rs) {
			/* current == prev inside sched_switch. */
			__u32 tgid = (__u32)(bpf_get_current_pid_tgid() >> 32);

			if (tgid_matches(tgid)) {
				e = bpf_ringbuf_reserve(&events, sizeof(*e), 0);
				if (e) {
					e->timestamp_ns = now;
					e->pid = prev_pid;
					e->tgid = tgid;
					e->cpu = (__u32)bpf_get_smp_processor_id();
					e->nice = (__s32)ctx->prev_prio - 120;
					e->wakeup_latency_ns = 0;
					e->runtime_ns = now - *rs;
					e->voluntary = ctx->prev_state != 0;
					e->type = EVENT_SLICE;
					bpf_ringbuf_submit(e, 0);
				}
			}
			bpf_map_delete_elem(&run_start, &prev_pid);
		}
	}
	return 0;
}

/*
 * sched_process_exit: drop stale entries so the hash maps cannot exhaust.
 * Fires for both processes and threads.
 */
SEC("tp/sched/sched_process_exit")
int sched_process_exit(struct trace_event_raw_sched_process_exit *ctx)
{
	__u32 pid = (__u32)ctx->pid;

	bpf_map_delete_elem(&wakeup_ts, &pid);
	bpf_map_delete_elem(&run_start, &pid);
	return 0;
}
