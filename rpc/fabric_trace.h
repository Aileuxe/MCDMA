/* Trace buffers have one serialized writer: a lane's lock, the parent's lock, or its signal lock.
 * Allocation, counters and clocks exist only when MCDMA_TRACE=1. Reporting freezes those writers. */
#define TRACE_CAP 16384u
struct trace_event { uint64_t ns, message, group, off, value; uint32_t bytes; uint16_t lane; uint8_t phase; };
struct trace_buffer { struct trace_event *events; uint64_t count; };
struct trace_publication { struct trace_event event; uint64_t need[BOND_LINKS]; };
struct trace_log {
    unsigned lanes;
    struct trace_buffer lane[BOND_LINKS], main;
    struct trace_publication *published;
    uint64_t publications;
};
struct trace_arg { struct trace_log *log; unsigned lane; };

static void trace_free(struct trace_log *log) {
    if (!log) return;
    for (unsigned k = 0; k < log->lanes; ++k) free(log->lane[k].events);
    free(log->main.events); free(log->published); free(log);
}
static struct trace_log *trace_new(unsigned lanes) {
    struct trace_log *log = calloc(1, sizeof(*log));
    if (!log) return NULL;
    log->lanes = lanes;
    for (unsigned k = 0; k < lanes; ++k) {
        log->lane[k].events = calloc(TRACE_CAP, sizeof(struct trace_event));
        if (!log->lane[k].events) { trace_free(log); return NULL; }
    }
    log->main.events = calloc(TRACE_CAP, sizeof(struct trace_event));
    log->published = calloc(TRACE_CAP, sizeof(struct trace_publication));
    if (!log->main.events || !log->published) { trace_free(log); return NULL; }
    return log;
}
static inline void trace_put(struct trace_buffer *b, unsigned lane, unsigned phase, uint64_t message,
                              uint64_t group, uint64_t off, uint64_t value, uint32_t bytes) {
    b->events[b->count++ % TRACE_CAP] = (struct trace_event){link_now_ns(), message, group, off, value, bytes,
                                                          (uint16_t)lane, (uint8_t)phase};
}
static void trace_lane(void *arg, unsigned phase, uint64_t message, uint64_t group, uint64_t off,
                         uint64_t value, uint32_t bytes) {
    struct trace_arg *a = arg;
    if (phase == TR_SIGNAL_PUBLISHED) {
        struct trace_publication *s = &a->log->published[a->log->publications++ % TRACE_CAP];
        memset(s, 0, sizeof(*s));
        s->event = (struct trace_event){link_now_ns(), message, 0, off, value, 0, (uint16_t)a->lane, (uint8_t)phase};
        s->need[a->lane] = group; /* single-QP path: its placed prefix */
    } else trace_put(&a->log->lane[a->lane], a->lane, phase, message, group, off, value, bytes);
}
static inline void trace_main(struct trace_log *log, unsigned phase, uint64_t group, uint64_t off,
                              uint64_t value, uint32_t bytes) {
    if (log) trace_put(&log->main, log->lanes, phase, 0, group, off, value, bytes);
}
static inline void trace_publish(struct trace_log *log, uint64_t sequence, uint64_t off, uint64_t value,
                                 const uint64_t *need) {
    if (!log) return;
    struct trace_publication *s = &log->published[log->publications++ % TRACE_CAP];
    memset(s, 0, sizeof(*s));
    s->event = (struct trace_event){link_now_ns(), 0, sequence, off, value, 0, (uint16_t)log->lanes, TR_SIGNAL_PUBLISHED};
    memcpy(s->need, need, log->lanes * sizeof(*need));
}
