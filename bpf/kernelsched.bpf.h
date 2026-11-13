// SPDX-License-Identifier: GPL-2.0
/*
 * kernelsched.bpf.h - shared definitions for the KernelSched eBPF program.
 * Included by the BPF side after vmlinux.h. The userspace side mirrors
 * struct sched_event in include/kernelsched/event.h and static_asserts
 * that the layouts agree.
 */
#ifndef KERNELSCHED_BPF_H
#define KERNELSCHED_BPF_H

/* Event types carried in sched_event.type */
#define EVENT_RUN   1  /* a thread got on CPU: wakeup_latency_ns is valid   */
#define EVENT_SLICE 2  /* a thread left CPU: runtime_ns / voluntary valid   */

/* Value stored in the wakeup_ts hash map, keyed by pid. */
struct wrec {
	__u64 ts;      /* time the task became runnable */
	__u32 tgid;    /* process id of the woken task  */
	__s32 nice;    /* nice value (prio - 120)       */
};

/*
 * Fixed-size event pushed to the BPF ring buffer.
 * NOTE: keep in sync with include/kernelsched/event.h (userspace mirror).
 */
struct sched_event {
	__u64 timestamp_ns;
	__u32 pid;
	__u32 tgid;
	__u32 cpu;
	__s32 nice;
	__u64 wakeup_latency_ns;
	__u64 runtime_ns;
	__u8  voluntary;
	__u8  type;
	__u16 _pad;
};

#endif /* KERNELSCHED_BPF_H */
