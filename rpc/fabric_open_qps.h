/* Shared window/PD per physical device; independent QP and progress lock per lane. */
static int open_member(const char *device, int gid, int mtu, void *window, size_t length, int fd,
                         uint64_t offset, uint32_t flags, struct mcdma_fabric *owner,
                         struct mcdma_fabric **out) {
    struct mcdma_fabric *f = calloc(1, sizeof(*f));
    if (!f) return MCDMA_FABRIC_NOMEM;
    pthread_mutex_init(&f->lock, NULL);
    f->devices = f->qps = 1;
    f->wait_poll = !getenv("MCDMA_FABRIC_WAIT_POLL") || strcmp(getenv("MCDMA_FABRIC_WAIT_POLL"), "0");
    int status = MCDMA_FABRIC_OK;
    if (owner) { f->shared = 1; f->dev = owner->dev; f->win = owner->win; }
    else {
        if (ep_open(&f->dev, device, gid, mtu)) status = MCDMA_FABRIC_DEVICE;
        if (!status) status = register_window(f, window, length, fd, offset);
    }
    if (!status && (flags & MCDMA_FABRIC_PROGRESS_THREAD)) {
        f->threaded = !pthread_create(&f->thread, NULL, progress_main, f);
        if (!f->threaded) status = MCDMA_FABRIC_NOMEM;
    }
    if (status) { if (!f->shared) ep_close(&f->dev); pthread_mutex_destroy(&f->lock); free(f); return status; }
    *out = f; return MCDMA_FABRIC_OK;
}

int mcdma_fabric_open_qps(const char *device, int gid, int mtu, void *window, size_t length, int fd,
                          uint64_t offset, uint32_t flags, unsigned requested, struct mcdma_fabric **out) {
    long page = sysconf(_SC_PAGESIZE);
    if (!out) return MCDMA_FABRIC_INVALID;
    *out = NULL;
    if (!device || !window || !length || page <= 0 || (uintptr_t)window % (uintptr_t)page ||
        length % (size_t)page || flags & ~MCDMA_FABRIC_PROGRESS_THREAD || requested > 3) return MCDMA_FABRIC_INVALID;
    char names[BOND_LINKS][128];
    int n = split_links(device, names, 1);
    if (!n) return MCDMA_FABRIC_INVALID;
    struct mcdma_fabric *f = calloc(1, sizeof(*f));
    if (!f) return MCDMA_FABRIC_NOMEM;
    pthread_mutex_init(&f->lock, NULL);
    f->devices = n; f->qps = (int)(requested ? requested : 2); f->bonded = n;
    f->win.base = window; f->win.length = length;
    int status = MCDMA_FABRIC_OK, thunderbolt = 1;
    for (int d = 0; !status && d < n; ++d) {
        status = open_member(names[d], gid, mtu, window, length, fd, offset,
                             flags | MCDMA_FABRIC_PROGRESS_THREAD, NULL, &f->part[d]);
        if (status) break;
        thunderbolt &= f->part[d]->dev.kind == LINK_TB;
        if (f->part[d]->dev.kind == LINK_TB) {
            struct ibv_device_attr attr = {0};
            if (ibv_query_device(f->part[d]->dev.ctx, &attr) || attr.max_qp < 1) { status = MCDMA_FABRIC_DEVICE; break; }
            f->qp_limit[d] = (unsigned)attr.max_qp < 3 ? (unsigned)attr.max_qp : 3;
            f->part[d]->qp_limit[0] = f->qp_limit[d];
            if (requested > f->qp_limit[d]) {
                link_log("%s: requested %u QPs but provider allows %u", names[d], requested, f->qp_limit[d]);
                status = MCDMA_FABRIC_DEVICE;
            } else if (!requested && f->qp_limit[d] < (unsigned)f->qps) f->qps = (int)f->qp_limit[d];
        }
    }
    if (!status && !thunderbolt) {
        if (n > 1 || requested > 1) status = MCDMA_FABRIC_UNSUPPORTED;
        else f->qps = 1;
    }
    if (!status && n == 1 && f->qps == 1) {
        if (!requested && thunderbolt) link_log("%s: provider limit selects one QP", names[0]);
        struct mcdma_fabric *single = f->part[0];
        if (!(flags & MCDMA_FABRIC_PROGRESS_THREAD)) {
            __atomic_store_n(&single->stop, 1, __ATOMIC_RELEASE);
            pthread_join(single->thread, NULL); single->threaded = 0; single->stop = 0;
        }
        pthread_mutex_destroy(&f->lock); free(f); *out = single; return MCDMA_FABRIC_OK;
    }
    if (!status) {
        f->bonded = n * f->qps;
        for (int q = 1; !status && q < f->qps; ++q)
            for (int d = 0; !status && d < n; ++d)
                status = open_member(names[d], gid, mtu, window, length, fd, offset,
                                     flags | MCDMA_FABRIC_PROGRESS_THREAD, f->part[d], &f->part[q * n + d]);
    }
    if (status) {
        for (unsigned k = (unsigned)f->bonded; k > 0; --k) mcdma_fabric_close(&f->part[k - 1]);
        pthread_mutex_destroy(&f->lock); free(f); return status;
    }
    if (f->qps > 1 || !requested)
        link_log("bond: %d physical devices, %d QPs per device, %d progress threads", n, f->qps, f->bonded);
    *out = f; return MCDMA_FABRIC_OK;
}

int mcdma_fabric_open(const char *device, int gid, int mtu, void *window, size_t length, int fd,
                      uint64_t offset, uint32_t flags, struct mcdma_fabric **out) {
    const char *env = getenv("MCDMA_FABRIC_QPS");
    unsigned requested = 0;
    if (env) {
        if (strlen(env) != 1 || env[0] < '1' || env[0] > '3') {
            if (out) *out = NULL;
            link_log("MCDMA_FABRIC_QPS must be 1, 2 or 3"); return MCDMA_FABRIC_INVALID;
        }
        requested = (unsigned)(env[0] - '0');
    }
    return mcdma_fabric_open_qps(device, gid, mtu, window, length, fd, offset, flags, requested, out);
}
