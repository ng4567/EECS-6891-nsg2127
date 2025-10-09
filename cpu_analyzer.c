#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include "uthash.h"
#include <bpf/libbpf.h>
#include <bpf/bpf.h>

struct offcpu_sample {
    __u32 tid;
    __u32 tgid;
    __u64 t0_ns;
    __u64 t2_ns;
    __u64 delta_ns;
};

struct tid_to_tgid_entry {
    __u32 tid;              // key
    __u32 tgid;             // value
    UT_hash_handle hh;
};

struct tgid_agg_entry {
    __u32 tgid;             // key
    __u64 total_delta_ns;   // accumulated off-CPU time
    __u64 *deltas;          // dynamic array of deltas (ns)
    size_t len;
    size_t cap;
    UT_hash_handle hh;
};

struct tid_to_tgid_entry *g_tid_to_tgid = NULL;
struct tgid_agg_entry *g_tgid_agg = NULL;
 __u32 g_filter_tgid = 0;

unsigned long long g_offcpu_pid_running_total_ns = 0;
unsigned long long *g_offcpu_hist_counts = NULL;
size_t g_offcpu_hist_num_buckets = 0;
__u64 *g_blocked_hist_prev = NULL;
__u32 g_blocked_hist_prev_buckets = 0;

unsigned long long get_monotonic_time_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

int read_tgid_from_proc_status(__u32 tid, __u32 *tgid_out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/status", (unsigned)tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[256];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Tgid:", 5) == 0) {
            char *p = line + 5;
            while (*p == ' ' || *p == '\t') p++;
            long val = strtol(p, NULL, 10);
            if (val > 0) {
                *tgid_out = (unsigned)val;
                rc = 0;
            }
            break;
        }
    }
    fclose(f);
    return rc;
}

__u32 resolve_tgid(__u32 tid, __u32 ev_tgid) {
    if (ev_tgid != 0)
        return ev_tgid;

    struct tid_to_tgid_entry *e = NULL;
    HASH_FIND(hh, g_tid_to_tgid, &tid, sizeof(tid), e);
    if (e)
        return e->tgid;

    __u32 tgid = 0;
    if (read_tgid_from_proc_status(tid, &tgid) == 0) {
        e = (struct tid_to_tgid_entry *)calloc(1, sizeof(*e));
        if (!e)
            return tgid; // best effort
        e->tid = tid;
        e->tgid = tgid;
        HASH_ADD(hh, g_tid_to_tgid, tid, sizeof(e->tid), e);
    }
    return tgid;
}

int append_delta(struct tgid_agg_entry *ent, __u64 delta_ns) {
    if (ent->len == ent->cap) {
        size_t new_cap = ent->cap ? ent->cap * 2 : 16;
        __u64 *new_arr = (__u64 *)realloc(ent->deltas, new_cap * sizeof(__u64));
        if (!new_arr)
            return -1;
        ent->deltas = new_arr;
        ent->cap = new_cap;
    }
    ent->deltas[ent->len++] = delta_ns;
    return 0;
}

void aggregate_tgid(__u32 tgid, __u64 delta_ns) {
    struct tgid_agg_entry *ent = NULL;
    HASH_FIND(hh, g_tgid_agg, &tgid, sizeof(tgid), ent);
    if (!ent) {
        ent = (struct tgid_agg_entry *)calloc(1, sizeof(*ent));
        if (!ent)
            return;
        ent->tgid = tgid;
        ent->total_delta_ns = 0;
        ent->deltas = NULL;
        ent->len = 0;
        ent->cap = 0;
        HASH_ADD(hh, g_tgid_agg, tgid, sizeof(ent->tgid), ent);
    }
    ent->total_delta_ns += delta_ns;
    (void)append_delta(ent, delta_ns);
}

void print_off_cpu_histogram() {
    struct tgid_agg_entry *ent, *tmp;

    if (g_filter_tgid != 0) {
        unsigned long long total_ns = 0;
        HASH_ITER(hh, g_tgid_agg, ent, tmp) {
            total_ns += ent->total_delta_ns;
            HASH_DEL(g_tgid_agg, ent);
            free(ent->deltas);
            free(ent);
        }
        g_offcpu_pid_running_total_ns += total_ns;
        double interval_ms = (double)total_ns / 1e6;
        double running_ms = (double)g_offcpu_pid_running_total_ns / 1e6;
        printf("PID %u off-cpu this interval: %.3f ms (ns=%llu); running total: %.3f ms (ns=%llu)\n",
               (unsigned)g_filter_tgid, interval_ms, (unsigned long long)total_ns,
               running_ms, (unsigned long long)g_offcpu_pid_running_total_ns);
        return;
    }

    size_t max_bucket = 0;
    HASH_ITER(hh, g_tgid_agg, ent, tmp) {
        for (size_t i = 0; i < ent->len; i++) {
            unsigned long long us = ent->deltas[i] / 1000ull;
            size_t idx = 0;
            if (us > 1) {
                unsigned long long v = us;
                while (v > 1) { v >>= 1; idx++; }
            }
            if (idx > max_bucket) max_bucket = idx;
        }
    }

    size_t needed = max_bucket + 1;
    if (needed > g_offcpu_hist_num_buckets) {
        unsigned long long *new_counts = (unsigned long long *)realloc(
            g_offcpu_hist_counts, needed * sizeof(*new_counts));
        if (new_counts) {
            for (size_t i = g_offcpu_hist_num_buckets; i < needed; i++) new_counts[i] = 0ull;
            g_offcpu_hist_counts = new_counts;
            g_offcpu_hist_num_buckets = needed;
        }
    }

    HASH_ITER(hh, g_tgid_agg, ent, tmp) {
        for (size_t i = 0; i < ent->len; i++) {
            unsigned long long us = ent->deltas[i] / 1000ull;
            size_t idx = 0;
            if (us > 1) {
                unsigned long long v = us;
                while (v > 1) { v >>= 1; idx++; }
            }
            if (idx < g_offcpu_hist_num_buckets)
                g_offcpu_hist_counts[idx]++;
        }
    }

    size_t last_nonzero = 0;
    for (size_t b = 0; b < g_offcpu_hist_num_buckets; b++) {
        if (g_offcpu_hist_counts[b] != 0)
            last_nonzero = b;
    }
    const size_t cap_bucket = 21;
    size_t end_bucket = last_nonzero < cap_bucket ? last_nonzero : cap_bucket;
    unsigned long long infinity_count = 0;
    if (last_nonzero > cap_bucket) {
        for (size_t b = cap_bucket + 1; b < g_offcpu_hist_num_buckets; b++)
            infinity_count += g_offcpu_hist_counts[b];
    }

    unsigned long long max_count = 0;
    for (size_t b = 0; b <= end_bucket; b++) {
        if (g_offcpu_hist_counts[b] > max_count) max_count = g_offcpu_hist_counts[b];
    }
    if (infinity_count > max_count) max_count = infinity_count;

    const int bar_width = 40;
    printf("Off-cpu time histogram\n");
    printf("     usecs               : count    distribution\n");
    for (size_t b = 0; b <= end_bucket; b++) {
        unsigned long long lower = (b == 0) ? 0ull : (1ull << b);
        unsigned long long upper = (1ull << (b + 1)) - 1ull;

        int stars = 0;
        if (max_count > 0) {
            double ratio = (double)g_offcpu_hist_counts[b] / (double)max_count;
            stars = (int)(ratio * bar_width + 0.5);
            if (stars < 0) stars = 0;
            if (stars > bar_width) stars = bar_width;
        }

        char bar[44 + 3];
        bar[0] = '|';
        int pos = 1;
        for (int i = 0; i < stars && pos < (bar_width + 1); i++) bar[pos++] = '*';
        while (pos < (bar_width + 1)) bar[pos++] = ' ';
        bar[pos++] = '|';
        bar[pos] = '\0';

        printf(" %10llu -> %-10llu : %-8llu %s\n",
               lower, upper, (unsigned long long)g_offcpu_hist_counts[b], bar);
    }
    if (infinity_count > 0) {
        int stars = 0;
        if (max_count > 0) {
            double ratio = (double)infinity_count / (double)max_count;
            stars = (int)(ratio * bar_width + 0.5);
            if (stars < 0) stars = 0;
            if (stars > bar_width) stars = bar_width;
        }
        char bar[44 + 3];
        bar[0] = '|';
        int pos = 1;
        for (int i = 0; i < stars && pos < (bar_width + 1); i++) bar[pos++] = '*';
        while (pos < (bar_width + 1)) bar[pos++] = ' ';
        bar[pos++] = '|';
        bar[pos] = '\0';

        printf(" %10llu -> %-10s : %-8llu %s\n",
               (unsigned long long)4194303, "infinity",
               (unsigned long long)infinity_count, bar);
    }

    HASH_ITER(hh, g_tgid_agg, ent, tmp) {
        HASH_DEL(g_tgid_agg, ent);
        free(ent->deltas);
        free(ent);
    }
}

void free_tid_tgid_cache(void) {
	struct tid_to_tgid_entry *e, *etmp;
	HASH_ITER(hh, g_tid_to_tgid, e, etmp) {
		HASH_DEL(g_tid_to_tgid, e);
		free(e);
	}
}

static int handle_rb_event(void *ctx, void *data, size_t data_sz) {
    const struct offcpu_sample *ev = (const struct offcpu_sample *)data;
    (void)ctx;
    (void)data_sz;
    __u32 tgid = resolve_tgid(ev->tid, ev->tgid);
    if (g_filter_tgid == 0 || tgid == g_filter_tgid) {
        aggregate_tgid(tgid, ev->delta_ns);
    }
    return 0;
}

static struct ring_buffer *g_rb;

void err_check(int argc, char **argv) {
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  sudo %s <interval_sec> [pid]\n", argv[0]);
        exit(EXIT_FAILURE);
    }
    if (geteuid() != 0) {
        fprintf(stderr, "This program must be run as root.\n");
        exit(EXIT_FAILURE);
    }
    int interval = atoi(argv[1]);
    if (interval <= 0) {
        fprintf(stderr, "Time interval must be greater than 0.\n");
        exit(EXIT_FAILURE);
    }
    if (argc == 3) {
        int pid = atoi(argv[2]);
        if (pid <= 0) {
            fprintf(stderr, "PID must be greater than 0 when provided.\n");
            exit(EXIT_FAILURE);
        }
    }
}

struct bpf_object *g_obj;
struct bpf_link *g_link_switch;
struct bpf_link *g_link_wakeup;
struct bpf_link *g_link_wakeup_new;
int g_blocked_hist_fd = -1;

static void print_blocked_histogram(void) {
    
    if (g_blocked_hist_fd < 0)
        return;

    struct bpf_map_info info = {};
    __u32 info_len = sizeof(info);
    if (bpf_obj_get_info_by_fd(g_blocked_hist_fd, &info, &info_len) != 0)
        return;
    __u32 buckets = info.max_entries;
    if (buckets == 0)
        return;

    unsigned long long *counts = (unsigned long long *)calloc(buckets, sizeof(unsigned long long));
    if (!counts)
        return;

    for (__u32 i = 0; i < buckets; i++) {
        __u64 val = 0;
        if (bpf_map_lookup_elem(g_blocked_hist_fd, &i, &val) == 0) {
            counts[i] = (unsigned long long)val;
        }
    }

    if (g_filter_tgid != 0) {
        if (!g_blocked_hist_prev || g_blocked_hist_prev_buckets != buckets) {
            free(g_blocked_hist_prev);
            g_blocked_hist_prev = (__u64 *)calloc(buckets, sizeof(__u64));
            g_blocked_hist_prev_buckets = buckets;
        }

        unsigned long long running_us = 0;
        unsigned long long interval_us = 0;
        for (__u32 b = 0; b < buckets; b++) {
            unsigned long long lower = (b == 0) ? 0ull : (1ull << b);
            unsigned long long upper = (1ull << (b + 1)) - 1ull;
            unsigned long long mid = (lower + upper) / 2ull;
            running_us += mid * counts[b];
            __u64 prev = g_blocked_hist_prev ? g_blocked_hist_prev[b] : 0ull;
            __u64 delta = (counts[b] > prev) ? (counts[b] - prev) : 0ull;
            interval_us += mid * delta;
        }
        if (g_blocked_hist_prev) {
            for (__u32 b = 0; b < buckets; b++) g_blocked_hist_prev[b] = counts[b];
        }

        double interval_ms = (double)interval_us / 1000.0;
        double running_ms = (double)running_us / 1000.0;
        printf("PID %u blocked this interval: %.3f ms (us=%llu); running total: %.3f ms (us=%llu)\n",
               (unsigned)g_filter_tgid, interval_ms, (unsigned long long)interval_us,
               running_ms, (unsigned long long)running_us);
        free(counts);
        return;
    }

    __u32 last_nonzero = 0;
    for (__u32 b = 0; b < buckets; b++) {
        if (counts[b] != 0)
            last_nonzero = b;
    }
    const __u32 cap_bucket = 21;
    __u32 end_bucket = last_nonzero < cap_bucket ? last_nonzero : cap_bucket;
    unsigned long long infinity_count = 0;
    if (last_nonzero > cap_bucket) {
        for (__u32 b = cap_bucket + 1; b < buckets; b++)
            infinity_count += counts[b];
    }

    unsigned long long max_count = 0;
    for (__u32 b = 0; b <= end_bucket; b++) {
        if (counts[b] > max_count) max_count = counts[b];
    }
    if (infinity_count > max_count) max_count = infinity_count;

    const int bar_width = 40;
    printf("Blocked time histogram\n");
    printf("     usecs                : count    distribution\n");
    for (__u32 b = 0; b <= end_bucket; b++) {
        unsigned long long lower = (b == 0) ? 0ull : (1ull << b);
        unsigned long long upper = (1ull << (b + 1)) - 1ull;

        int stars = 0;
        if (max_count > 0) {
            double ratio = (double)counts[b] / (double)max_count;
            stars = (int)(ratio * bar_width + 0.5);
            if (stars < 0) stars = 0;
            if (stars > bar_width) stars = bar_width;
        }

        char bar[44 + 3];
        bar[0] = '|';
        int pos = 1;
        for (int i = 0; i < stars && pos < (bar_width + 1); i++) bar[pos++] = '*';
        while (pos < (bar_width + 1)) bar[pos++] = ' ';
        bar[pos++] = '|';
        bar[pos] = '\0';

        printf(" %10llu -> %-10llu : %-8llu %s\n",
               lower, upper, (unsigned long long)counts[b], bar);
    }
    if (infinity_count > 0) {
        int stars = 0;
        if (max_count > 0) {
            double ratio = (double)infinity_count / (double)max_count;
            stars = (int)(ratio * bar_width + 0.5);
            if (stars < 0) stars = 0;
            if (stars > bar_width) stars = bar_width;
        }
        char bar[44 + 3];
        bar[0] = '|';
        int pos = 1;
        for (int i = 0; i < stars && pos < (bar_width + 1); i++) bar[pos++] = '*';
        while (pos < (bar_width + 1)) bar[pos++] = ' ';
        bar[pos++] = '|';
        bar[pos] = '\0';

        printf(" %10llu -> %-10s : %-8llu %s\n",
               (unsigned long long)4194303, "infinity",
               (unsigned long long)infinity_count, bar);
    }

    free(counts);
}

int load_bpf_program(__u32 pid)
{
    struct bpf_program *prog;
    int err;

    fprintf(stderr, "Loading BPF code in memory\n");
    g_obj = bpf_object__open_file("cpu_analyzer.bpf.o", NULL);
    if (libbpf_get_error(g_obj)) {
        fprintf(stderr, "ERROR: opening BPF object file failed\n");
        return -1;
    }

    fprintf(stderr, "Loading and verifying the code in the kernel\n");
    err = bpf_object__load(g_obj);
    if (err) {
        fprintf(stderr, "ERROR: loading BPF object file failed: %d\n", err);
        return -1;
    }

    bpf_object__for_each_program(prog, g_obj) {
        struct bpf_link *link = bpf_program__attach(prog);
        if (libbpf_get_error(link)) {
            fprintf(stderr, "ERROR: attaching program failed\n");
            return -1;
        }
        const char *name = bpf_program__name(prog);
        if (strstr(name, "handle_sched_switch"))
            g_link_switch = link;
        else if (strstr(name, "handle_sched_wakeup"))
            g_link_wakeup = link;
        else if (strstr(name, "handle_sched_wakeup_new"))
            g_link_wakeup_new = link;
    }

    fprintf(stderr, "BPF programs loaded and attached. Set PID=%u\n", pid);

    struct bpf_map *blocked_map = bpf_object__find_map_by_name(g_obj, "blocked_hist");
    if (!blocked_map) {
        fprintf(stderr, "WARNING: could not find map 'blocked_hist' (blocked histogram disabled)\n");
    } else {
        g_blocked_hist_fd = bpf_map__fd(blocked_map);
        if (g_blocked_hist_fd < 0) {
            fprintf(stderr, "WARNING: failed to get fd for 'blocked_hist'\n");
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    err_check(argc, argv);

    __u32 pid = 0;
    int interval = atoi(argv[1]);
    if (argc == 3) {
        pid = (__u32)atoi(argv[2]);
    }
    g_filter_tgid = pid;

    if (load_bpf_program(pid) != 0) {
        fprintf(stderr, "Failed to load/attach BPF program.\n");
        return EXIT_FAILURE;
    }

    struct bpf_map *rb_map = bpf_object__find_map_by_name(g_obj, "rb");
    if (!rb_map) {
        fprintf(stderr, "ERROR: could not find ring buffer map 'rb'\n");
        goto cleanup;
    }
    int rb_fd = bpf_map__fd(rb_map);
    if (rb_fd < 0) {
        fprintf(stderr, "ERROR: failed to get ring buffer fd\n");
        goto cleanup;
    }
    g_rb = ring_buffer__new(rb_fd, handle_rb_event, NULL, NULL);
    if (!g_rb) {
        fprintf(stderr, "ERROR: failed to create ring buffer consumer\n");
        goto cleanup;
    }

    unsigned long long interval_ns = (unsigned long long)interval * 1000000000ull;
    unsigned long long next_print_ns = get_monotonic_time_ns() + interval_ns;
    for (;;) {
        unsigned long long now = get_monotonic_time_ns();
        long long remain_ns = (long long)(next_print_ns - now);
        int timeout_ms;
        if (remain_ns <= 0) {
            timeout_ms = 0;
        } else {
            unsigned long long remain_ms = (unsigned long long)(remain_ns / 1000000ll);
            if (remain_ms == 0) remain_ms = 1;
            if (remain_ms > 250) remain_ms = 250;
            timeout_ms = (int)remain_ms;
        }

        int ret = ring_buffer__poll(g_rb, timeout_ms);
        if (ret < 0 && ret != -EINTR) {
            fprintf(stderr, "ERROR: ring_buffer__poll failed: %d\n", ret);
            break;
        }

        now = get_monotonic_time_ns();
        if (now >= next_print_ns) {
            print_off_cpu_histogram();
            print_blocked_histogram();
            do {
                next_print_ns += interval_ns;
            } while (next_print_ns <= now);
        }
    }

cleanup:
    if (g_rb) {
        ring_buffer__free(g_rb);
        g_rb = NULL;
    }
    free_tid_tgid_cache();
    if (g_link_switch) bpf_link__destroy(g_link_switch);
    if (g_link_wakeup) bpf_link__destroy(g_link_wakeup);
    if (g_link_wakeup_new) bpf_link__destroy(g_link_wakeup_new);
    if (g_obj) bpf_object__close(g_obj);
    return EXIT_SUCCESS;
}