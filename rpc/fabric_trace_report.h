struct trace_pair { uint64_t id, ns; };
struct trace_samples { uint64_t *ns; size_t count; };
static int trace_compare(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x > y ? 1 : x < y ? -1 : 0;
}
static uint64_t trace_find(const struct trace_pair *p, size_t n, uint64_t id) {
    size_t lo = 0, hi = n;
    while (lo < hi) { size_t mid = lo + (hi - lo) / 2; if (p[mid].id < id) lo = mid + 1; else hi = mid; }
    return lo < n && p[lo].id == id ? p[lo].ns : 0;
}
static void trace_sample(struct trace_samples *s, uint64_t a, uint64_t b) {
    if (a && b >= a) s->ns[s->count++] = b - a;
}
static void trace_summary(FILE *out, const char *name, struct trace_samples *s) {
    qsort(s->ns, s->count, sizeof(uint64_t), trace_compare);
    fprintf(out, "mcdma_trace: phase=%s samples=%zu p50_us=%.3f p99_us=%.3f\n", name, s->count,
            s->count ? (double)s->ns[(s->count - 1) / 2] / 1000 : 0,
            s->count ? (double)s->ns[(s->count - 1) * 99 / 100] / 1000 : 0);
}
static const char *trace_name(unsigned phase) {
    static const char *names[] = {"post", "wire_complete_cq_observed", "receive_complete", "copy_done",
        "signal_published", "peer_poll_seen", "app_post", "batch_ready", "batch_go", "batch_fallback"};
    return phase < sizeof(names) / sizeof(*names) ? names[phase] : "unknown";
}
static void trace_line(FILE *out, const struct trace_event *e) {
    fprintf(out, "mcdma_trace: event=%s lane=%u message=%llu group=%llu offset=%llu value=%llu bytes=%u ns=%llu\n",
            trace_name(e->phase), e->lane, (unsigned long long)e->message, (unsigned long long)e->group,
            (unsigned long long)e->off, (unsigned long long)e->value, e->bytes, (unsigned long long)e->ns);
}

int mcdma_fabric_tracing(const struct mcdma_fabric_peer *p) { return p && p->trace != NULL; }
void mcdma_fabric_trace_poll_seen(struct mcdma_fabric_peer *p, uint64_t offset, uint64_t value) {
    if (!p || !p->trace) return;
    uint64_t now = link_now_ns();
    enter(p->f);
    struct trace_buffer *b = &p->trace->main;
    b->events[b->count++ % TRACE_CAP] = (struct trace_event){now, 0, 0, offset, value, 0,
                                                         (uint16_t)p->trace->lanes, TR_PEER_POLL_SEEN};
    pthread_mutex_unlock(&p->f->lock);
}

int mcdma_fabric_trace_report(struct mcdma_fabric_peer *p, FILE *out) {
    if (!p || !out) return MCDMA_FABRIC_INVALID;
    struct trace_log *log = p->trace;
    if (!log) return MCDMA_FABRIC_OK;
    const size_t bound = (size_t)(log->lanes + 2) * TRACE_CAP;
    uint64_t *storage = calloc(6 * bound, sizeof(uint64_t));
    struct trace_pair *copy[BOND_LINKS] = {0}, *post = calloc(TRACE_CAP, sizeof(*post)), *received = calloc(TRACE_CAP, sizeof(*received));
    size_t copies[BOND_LINKS] = {0};
    for (unsigned k = 0; k < log->lanes; ++k) copy[k] = calloc(TRACE_CAP, sizeof(struct trace_pair));
    int bad = !storage || !post || !received;
    for (unsigned k = 0; k < log->lanes; ++k) bad |= copy[k] == NULL;
    if (bad) {
        for (unsigned k = 0; k < log->lanes; ++k) free(copy[k]);
        free(storage); free(post); free(received); return MCDMA_FABRIC_NOMEM;
    }
    struct trace_samples samples[6];
    for (unsigned k = 0; k < 6; ++k) samples[k] = (struct trace_samples){storage + k * bound, 0};
    enter(p->f);
    if (p->bonded) {
        for (unsigned k = 0; k < log->lanes; ++k) enter(p->part[k]->f);
        pthread_mutex_lock(&p->rx_lock);
    }
    uint64_t dropped = log->main.count > TRACE_CAP ? log->main.count - TRACE_CAP : 0;
    if (log->publications > TRACE_CAP) dropped += log->publications - TRACE_CAP;
    fprintf(out, "mcdma_trace: clock=local_monotonic_ns wire_complete=local_SEND_CQ_observation lanes=%u capacity_per_buffer=%u\n", log->lanes, TRACE_CAP);
    for (unsigned k = 0; k < log->lanes; ++k) {
        struct trace_buffer *b = &log->lane[k];
        size_t np = 0, nr = 0;
        uint64_t start = b->count > TRACE_CAP ? b->count - TRACE_CAP : 0;
        dropped += start;
        for (uint64_t j = start; j < b->count; ++j) {
            const struct trace_event *e = &b->events[j % TRACE_CAP]; trace_line(out, e);
            if (e->phase == TR_POST) post[np++] = (struct trace_pair){e->message, e->ns};
            if (e->phase == TR_RECEIVE_COMPLETE) received[nr++] = (struct trace_pair){e->message, e->ns};
            if (e->phase == TR_COPY_DONE && e->bytes) copy[k][copies[k]++] = (struct trace_pair){e->group, e->ns};
        }
        for (uint64_t j = start; j < b->count; ++j) {
            const struct trace_event *e = &b->events[j % TRACE_CAP];
            if (e->phase == TR_WIRE_COMPLETE) trace_sample(&samples[0], trace_find(post, np, e->message), e->ns);
            if (e->phase == TR_COPY_DONE && e->bytes) trace_sample(&samples[1], trace_find(received, nr, e->message), e->ns);
        }
    }
    uint64_t prev[BOND_LINKS] = {0};
    uint64_t start = log->publications > TRACE_CAP ? log->publications - TRACE_CAP : 0;
    for (uint64_t j = start; j < log->publications; ++j) {
        const struct trace_publication *s = &log->published[j % TRACE_CAP]; trace_line(out, &s->event);
        uint64_t latest = 0; int any = 0, complete = 1;
        for (unsigned k = 0; k < log->lanes; ++k) {
            if (s->need[k] > prev[k]) {
                uint64_t ns = trace_find(copy[k], copies[k], s->need[k]); any = 1;
                if (!ns) complete = 0;
                if (ns > latest) latest = ns;
            }
            prev[k] = s->need[k];
        }
        if (any && complete) trace_sample(&samples[2], latest, s->event.ns);
    }
    uint64_t main_start = log->main.count > TRACE_CAP ? log->main.count - TRACE_CAP : 0;
    for (uint64_t j = main_start; j < log->main.count; ++j) {
        const struct trace_event *e = &log->main.events[j % TRACE_CAP]; trace_line(out, e);
        if (e->phase == TR_PEER_POLL_SEEN) {
            for (uint64_t a = log->publications; a > start; --a) {
                const struct trace_event *s = &log->published[(a - 1) % TRACE_CAP].event;
                if (s->off == e->off && s->value == e->value) { trace_sample(&samples[3], s->ns, e->ns); break; }
            }
        }
        if (e->phase == TR_BATCH_GO) {
            for (uint64_t a = j; a > main_start; --a) {
                const struct trace_event *s = &log->main.events[(a - 1) % TRACE_CAP];
                if (s->phase == TR_APP_POST && s->group == e->group) { trace_sample(&samples[4], s->ns, e->ns); break; }
            }
            for (unsigned k = 0; k < log->lanes; ++k) {
                struct trace_buffer *b = &log->lane[k];
                uint64_t bs = b->count > TRACE_CAP ? b->count - TRACE_CAP : 0;
                for (uint64_t a = bs; a < b->count; ++a) {
                    const struct trace_event *s = &b->events[a % TRACE_CAP];
                    if (s->phase == TR_BATCH_READY && s->group == e->group) { trace_sample(&samples[5], s->ns, e->ns); break; }
                }
            }
        }
    }
    const char *names[] = {"post_to_wire_complete", "receive_to_copy_done", "copy_done_to_signal_published",
                           "signal_published_to_poll_seen", "app_post_to_batch_go", "batch_ready_to_go"};
    for (unsigned k = 0; k < 6; ++k) trace_summary(out, names[k], &samples[k]);
    fprintf(out, "mcdma_trace: dropped_events=%llu\n", (unsigned long long)dropped);
    if (p->bonded) {
        pthread_mutex_unlock(&p->rx_lock);
        for (unsigned k = log->lanes; k > 0; --k) pthread_mutex_unlock(&p->part[k - 1]->f->lock);
    }
    pthread_mutex_unlock(&p->f->lock);
    for (unsigned k = 0; k < log->lanes; ++k) free(copy[k]);
    free(storage); free(post); free(received); return MCDMA_FABRIC_OK;
}
