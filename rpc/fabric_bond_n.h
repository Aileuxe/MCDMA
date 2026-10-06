/* Parallel parts and receiver-side N-link signal watermarks; included after the fabric's private helpers. */

static void bond_parts(const struct bond_load *load, unsigned n, uint64_t len, uint64_t *sizes) {
    uint64_t rates[BOND_LINKS], sum = 0, fastest = 0;
    for (unsigned k = 0; k < n; ++k) if (load[k].rate > fastest) fastest = load[k].rate;
    for (unsigned k = 0; k < n; ++k) {
        uint64_t rate = load[k].rate ? load[k].rate : BOND_RATE;
        if (rate < fastest / 3) rate = fastest / 3;
        rates[k] = rate > (1ull << 20) ? (1ull << 20) : rate;
        sum += rates[k];
    }
    uint64_t rest = len - n * BOND_PART, used = 0;
    for (unsigned k = 0; k + 1 < n; ++k) {
        sizes[k] = BOND_PART + (rest / sum * rates[k] + (rest % sum) * rates[k] / sum) / 64 * 64;
        used += sizes[k];
    }
    sizes[n - 1] = len - used;
}

/* Queue every member first, wait until its worker is prepared, then release the workers together. */
static int bond_parallel(struct mcdma_fabric_peer *p, uint64_t off, uint64_t roff, uint64_t len,
                          uint64_t soff, uint64_t value) {
    uint64_t wait[BOND_LINKS]; int signalled;
    bond_overlap(p, roff, len, wait, &signalled);
    struct bond_load load[BOND_LINKS] = {{0}};
    if (bond_look(p, load)) return -1;
    unsigned n = (unsigned)p->bonded;
    uint64_t sizes[BOND_LINKS] = {0}, ordinal[BOND_LINKS] = {0};
    if (len < BOND_SPLIT || len < n * BOND_PART) {
        unsigned k = p->turn++ % n;
        if (len <= TB_PACKET - (48 + n * 8)) {
            uint64_t need[BOND_LINKS] = {0}; bond_watermarks(p, need); need[k]++;
            enter(p->part[k]->f);
            int bad = tb_bond_inline_n(&p->part[k]->e, &p->part[k]->f->win, off, roff, len,
                                        soff, value, p->tx_signal + 1, need, wait, n,
                                        signalled ? BOND_WAIT_SIGNALS : 0, OP_NS);
            pthread_mutex_unlock(&p->part[k]->f->lock);
            if (bad) return -1;
            p->tx_signal++; ordinal[k] = need[k];
            bond_remember(p, roff, len, ordinal, 0);
            bond_remember(p, soff, 8, (uint64_t[BOND_LINKS]){0}, 1);
            return 0;
        }
        enter(p->part[k]->f);
        int bad = tb_bond_write_n(&p->part[k]->e, &p->part[k]->f->win, off, roff, len,
                                   p->tx_signal + 1, wait, n, signalled ? BOND_WAIT_SIGNALS : 0, OP_NS);
        ordinal[k] = tb_writes_posted(&p->part[k]->e);
        pthread_mutex_unlock(&p->part[k]->f->lock);
        if (bad) return -1;
    } else {
        bond_parts(load, n, len, sizes);
        struct bond_batch batch = {.links = n, .flags = signalled ? BOND_WAIT_SIGNALS : 0, .seq = p->tx_signal + 1};
        memcpy(batch.wait, wait, sizeof(wait));
        unsigned local[BOND_LINKS] = {0};
        uint64_t at = off, to = roff;
        for (unsigned k = 0; k < n; ++k) {
            struct mcdma_fabric *f = p->part[k]->f;
            enter(f);
            f->ask_lane = p->part[k], f->ask_off = at, f->ask_roff = to, f->ask_len = sizes[k];
            f->batch = &batch;
            __atomic_store_n(&f->ask, ASK_OPEN, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&f->lock);
            at += sizes[k], to += sizes[k];
        }
        uint64_t deadline = link_now_ns() + OP_NS, wake = link_now_ns() + 100000;
        while (__atomic_load_n(&batch.ready, __ATOMIC_ACQUIRE) < n) {
            if (link_now_ns() >= wake) {
                for (unsigned k = 0; k < n; ++k) {
                    int open = ASK_OPEN;
                    if (__atomic_compare_exchange_n(&p->part[k]->f->ask, &open, ASK_TAKEN, 0,
                                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                        local[k] = 1;
                        __atomic_add_fetch(&batch.ready, 1, __ATOMIC_ACQ_REL);
                        if (!p->fallback_logged) {
                            link_log("%s: bond worker wake exceeded 100 us; caller fallback may serialize parts", p->name);
                            p->fallback_logged = 1;
                        }
                    }
                }
            }
            if (peer_down(p) || link_now_ns() >= deadline) {
                __atomic_store_n(&batch.cancel, 1, __ATOMIC_RELEASE);
                for (unsigned k = 0; k < n; ++k) {
                    int open = ASK_OPEN;
                    __atomic_compare_exchange_n(&p->part[k]->f->ask, &open, ASK_FAILED, 0,
                                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                }
                break;
            }
        }
        __atomic_store_n(&batch.go, 1, __ATOMIC_RELEASE);
        int bad = __atomic_load_n(&batch.cancel, __ATOMIC_ACQUIRE);
        for (unsigned k = 0; k < n; ++k) if (local[k]) {
            struct mcdma_fabric *f = p->part[k]->f;
            enter(f);
            __atomic_store_n(&f->ask, post_ask(f), __ATOMIC_RELEASE);
            pthread_mutex_unlock(&f->lock);
        }
        for (unsigned k = 0; k < n; ++k) {
            struct mcdma_fabric *f = p->part[k]->f;
            int state;
            while ((state = __atomic_load_n(&f->ask, __ATOMIC_ACQUIRE)) == ASK_OPEN || state == ASK_TAKEN) {}
            bad |= state != ASK_DONE;
            enter(f);
            ordinal[k] = tb_writes_posted(&p->part[k]->e);
            f->batch = NULL;
            __atomic_store_n(&f->ask, ASK_NONE, __ATOMIC_RELEASE);
            pthread_mutex_unlock(&f->lock);
        }
        if (bad) { peer_fail(p, "a parallel bond member could not post its part", 1); return -1; }
    }
    bond_remember(p, roff, len, ordinal, 0);
    return bond_signal_locked(p, soff, value);
}
