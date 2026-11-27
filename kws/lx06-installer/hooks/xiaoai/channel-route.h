/* Pure routing decisions shared by webd, the official wake bridge and tests.
 * WT2 calls the listen-window and matcher APIs; it does not reimplement this. */
#ifndef LX06_CHANNEL_ROUTE_H
#define LX06_CHANNEL_ROUTE_H

enum lx06_local_route {
    LX06_LOCAL_IGNORE = 0,       /* task or listen window already active */
    LX06_LOCAL_PASSTHROUGH = 1,  /* LX06-LOCAL-CHANNEL empty: --wakeup */
    LX06_LOCAL_LISTEN = 2        /* non-empty: open a listen window, never passthrough */
};

enum lx06_official_route {
    LX06_OFFICIAL_STOCK = 0,       /* unbound, or official wake is off */
    LX06_OFFICIAL_PAUSED = 1,      /* bind kept, scripts paused: still stock */
    LX06_OFFICIAL_PASSTHROUGH = 2, /* bound script, empty channel */
    LX06_OFFICIAL_LISTEN = 3       /* bound script, channel set */
};

enum lx06_bind_state {
    LX06_BIND_NONE = 0,
    LX06_BIND_ARMED = 1,
    LX06_BIND_PAUSED = 2,
    LX06_BIND_CLEARED = 3
};

/* Compatibility helper: the limit is now measured from speech start. */
static inline int lx06_window_ms_clamp(unsigned ms) {
    if (ms == 0) return 10000;
    if (ms < 3000) return 3000;
    if (ms > 15000) return 15000;
    return (int)ms;
}

static inline int lx06_window_start_ms_clamp(unsigned ms) {
    if (ms == 0) return 5000;
    if (ms < 1000) return 1000;
    if (ms > 15000) return 15000;
    return (int)ms;
}

/* Empty channel is passthrough, including scripts that never wrote the key.
 * A non-empty channel never falls back to passthrough. */
static inline int lx06_route_local(int task_busy, int channel_nonempty) {
    if (task_busy) return LX06_LOCAL_IGNORE;
    if (!channel_nonempty) return LX06_LOCAL_PASSTHROUGH;
    return LX06_LOCAL_LISTEN;
}

/* Official preemption (WAKE=46 then TEARDOWN within 6s) exists only while
 * the official keyword switch is on. Firmware must not turn that switch on. */
static inline int lx06_preempt_allowed(int kws_on) { return kws_on ? 1 : 0; }

static inline int lx06_route_official(int kws_on, int bind_state, int channel_nonempty) {
    if (!kws_on || bind_state == LX06_BIND_NONE || bind_state == LX06_BIND_CLEARED)
        return LX06_OFFICIAL_STOCK;
    if (bind_state == LX06_BIND_PAUSED) return LX06_OFFICIAL_PAUSED;
    if (bind_state != LX06_BIND_ARMED) return LX06_OFFICIAL_STOCK;
    return channel_nonempty ? LX06_OFFICIAL_LISTEN : LX06_OFFICIAL_PASSTHROUGH;
}

/* Only a template/exact hit launches. Timeout, ASR failure, miss, reject
 * and cancel stay idle and must not call --wakeup. */
static inline int lx06_listen_launches(int status) { return status == 1; }
#endif
