#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
char LICENSE[] SEC("license") = "Dual BSD/GPL";

// Ring buffer
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 1 << 24); // 16 MB
} rb SEC(".maps");

// Per-thread (TID) start timestamp when descheduled
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);   // tid
    __type(value, __u64); // t0_ns
    __uint(max_entries, 16384);
} offcpu_start SEC(".maps");

// Per-thread blocked start timestamp (when switched out to sleep)
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, __u32);   // tid
    __type(value, __u64); // t0_ns
    __uint(max_entries, 16384);
} blocked_start SEC(".maps");

#define BLOCKED_HIST_BUCKETS 64
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, BLOCKED_HIST_BUCKETS);
    __type(key, __u32);   // bucket index
    __type(value, __u64); // count
} blocked_hist SEC(".maps");

struct offcpu_sample {
    __u32 tid;
    __u32 tgid;
    __u64 t0_ns;
    __u64 t2_ns;
    __u64 delta_ns;
};

struct sched_wakeup_args {
    __u64 pad;
    char comm[16];
    __u32 pid;
    __u32 prio;
    __u32 success;
    __u32 target_cpu;
};

static __always_inline __u32 log2_u64(__u64 v)
{
    __u32 r = 0;
    if (v >> 32) { v >>= 32; r += 32; }
    if (v >> 16) { v >>= 16; r += 16; }
    if (v >> 8)  { v >>= 8;  r += 8;  }
    if (v >> 4)  { v >>= 4;  r += 4;  }
    if (v >> 2)  { v >>= 2;  r += 2;  }
    if (v >> 1)  {           r += 1;  }
    return r;
}

SEC("tracepoint/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    
    __u64 now = bpf_ktime_get_ns();

    __u32 next_tid = ctx->next_pid;
    __u64 *t0p = bpf_map_lookup_elem(&offcpu_start, &next_tid);
    if (t0p) {
        __u64 t0 = *t0p;
        struct offcpu_sample *ev = bpf_ringbuf_reserve(&rb, sizeof(*ev), 0);
        if (ev) {
            ev->tid = next_tid;
            ev->tgid = 0;
            ev->t0_ns = t0;
            ev->t2_ns = now;
            ev->delta_ns = now - t0;
            bpf_ringbuf_submit(ev, 0);
        }
        bpf_map_delete_elem(&offcpu_start, &next_tid);
    }

    __u32 prev_tid = ctx->prev_pid;
    bpf_map_update_elem(&offcpu_start, &prev_tid, &now, BPF_ANY);


    if (ctx->prev_state != 0) {
        bpf_map_update_elem(&blocked_start, &prev_tid, &now, BPF_ANY);
    }

    return 0;
}

SEC("tracepoint/sched/sched_wakeup")
int handle_sched_wakeup(struct sched_wakeup_args *ctx)
{
    __u64 now = bpf_ktime_get_ns();
    __u32 tid = ctx->pid;

    __u64 *t0p = bpf_map_lookup_elem(&blocked_start, &tid);
    if (!t0p)
        return 0;

    __u64 delta_ns = now - *t0p;
    __u64 delta_us = delta_ns / 1000;
    __u32 bucket = log2_u64(delta_us);
    if (bucket >= BLOCKED_HIST_BUCKETS)
        bucket = BLOCKED_HIST_BUCKETS - 1;

    __u64 *cnt = bpf_map_lookup_elem(&blocked_hist, &bucket);
    if (cnt)
        __sync_fetch_and_add(cnt, 1);

    bpf_map_delete_elem(&blocked_start, &tid);
    return 0;
}

SEC("tracepoint/sched/sched_wakeup_new")
int handle_sched_wakeup_new(struct sched_wakeup_args *ctx)
{
    __u64 now = bpf_ktime_get_ns();
    __u32 tid = ctx->pid;

    __u64 *t0p = bpf_map_lookup_elem(&blocked_start, &tid);
    if (!t0p)
        return 0;

    __u64 delta_ns = now - *t0p;
    __u64 delta_us = delta_ns / 1000;
    __u32 bucket = log2_u64(delta_us);
    if (bucket >= BLOCKED_HIST_BUCKETS)
        bucket = BLOCKED_HIST_BUCKETS - 1;

    __u64 *cnt = bpf_map_lookup_elem(&blocked_hist, &bucket);
    if (cnt)
        __sync_fetch_and_add(cnt, 1);

    bpf_map_delete_elem(&blocked_start, &tid);
    return 0;
}

