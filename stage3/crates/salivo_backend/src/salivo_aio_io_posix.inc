/* Salivo Stage 36.1 async runtime - POSIX I/O backend (included by salivo_aio_runtime.c).
 *
 * Readiness model: sockets are non-blocking. An operation first tries the system call; on
 * EAGAIN the task registers a waiter on the socket and parks, and the reactor wakes it when the
 * socket becomes readable/writable, after which the call is retried. No worker thread ever blocks
 * in the OS for socket data.
 *
 * Reactor: epoll (edge-triggered, every socket registered once) on Linux; poll(2) elsewhere
 * (macOS/BSD), where the reactor polls only sockets that currently have waiters. On Linux,
 * SALIVO_AIO_POLL=1 selects the poll reactor too, so both paths are exercised by the tests.
 *
 * Lost-wakeup protection: every socket has read/write readiness sequence numbers, bumped by the
 * reactor (under the socket lock) whenever it reports readiness. An operation reads the sequence
 * before its system call; if the call returns EAGAIN and the sequence has moved since, it retries
 * instead of parking.
 *
 * Files: regular files are always "ready" to poll/epoll, so reads and writes (pread/pwrite), open,
 * flush (fsync) and close run on the bounded blocking pool - never on an async worker.
 *
 * Cancellation needs no kernel coordination here: a cancelled waiter just unlinks itself.
 */

#if defined(__linux__)
#define SA_HAVE_EPOLL 1
#endif
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct SaReactor {
    int use_poll;
    int epfd;       /* epoll instance (epoll mode) */
    int wake_rd;    /* self-pipe: the read end is watched by the reactor */
    int wake_wr;
    sa_mutex ilock; /* poll mode: handles of sockets that currently have waiters */
    int64_t* interest;
    int n, cap;
} SaReactor;

static int sa_net_init(void) {
    signal(SIGPIPE, SIG_IGN); /* a write to a reset socket must return EPIPE, not kill the process */
    return 1;
}

static void sa_net_fini(void) {}

static int sa_set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0 || fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return 0;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 1;
}

static int sa_reactor_init(SaRuntime* rt) {
    SaReactor* r = (SaReactor*)calloc(1, sizeof(SaReactor));
    if (!r) return 0;
    int p[2];
    if (pipe(p) != 0) {
        free(r);
        return 0;
    }
    sa_set_nonblock(p[0]);
    sa_set_nonblock(p[1]);
    r->wake_rd = p[0];
    r->wake_wr = p[1];
    r->epfd = -1;
    sa_mutex_init(&r->ilock);
#ifdef SA_HAVE_EPOLL
    const char* force_poll = getenv("SALIVO_AIO_POLL");
    r->use_poll = force_poll && force_poll[0] == '1';
    if (!r->use_poll) {
        r->epfd = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN;
        ev.data.u64 = 0; /* 0 = the wake pipe */
        if (r->epfd < 0 || epoll_ctl(r->epfd, EPOLL_CTL_ADD, r->wake_rd, &ev) != 0) {
            if (r->epfd >= 0) close(r->epfd);
            close(p[0]);
            close(p[1]);
            sa_mutex_destroy(&r->ilock);
            free(r);
            return 0;
        }
    }
#else
    r->use_poll = 1;
#endif
    rt->reactor = r;
    return 1;
}

static void sa_reactor_fini(SaRuntime* rt) {
    SaReactor* r = (SaReactor*)rt->reactor;
    if (!r) return;
    if (r->epfd >= 0) close(r->epfd);
    close(r->wake_rd);
    close(r->wake_wr);
    sa_mutex_destroy(&r->ilock);
    free(r->interest);
    free(r);
    rt->reactor = NULL;
}

static void sa_reactor_notify(SaRuntime* rt) {
    SaReactor* r = (SaReactor*)rt->reactor;
    char b = 1;
    ssize_t n = write(r->wake_wr, &b, 1); /* EAGAIN: a wake is already pending */
    (void)n;
}

static void sa_drain_wake(SaReactor* r) {
    char buf[256];
    while (read(r->wake_rd, buf, sizeof(buf)) > 0) {
    }
}

/* ---- errors ------------------------------------------------------------------------------ */
static int64_t sa_map_errno(int e) {
    sa_set_os_error((int64_t)e);
    switch (e) {
    case 0: return SA_OK;
    case ECONNRESET:
    case EPIPE:
    case ECONNABORTED:
    case ENETRESET: return SA_ERESET;
    case ECONNREFUSED: return SA_EREFUSED;
    case EADDRINUSE: return SA_EADDRINUSE;
    case EBADF:
    case ENOTSOCK: return SA_ECLOSED;
    case EINVAL:
    case EAFNOSUPPORT:
    case EADDRNOTAVAIL:
    case EDESTADDRREQ: return SA_EINVAL;
    case ETIMEDOUT: return SA_ETIMEOUT;
    case EMSGSIZE: return SA_ETRUNC;
    default: return SA_EOS;
    }
}

static int64_t sa_io_ctx(SaRuntime** out) {
    int64_t err = SA_ESHUTDOWN;
    SaRuntime* rt = sa_ctx_rt(&err);
    if (!rt) return err;
    if (!A_LOAD(&rt->reactor_running)) return SA_ESHUTDOWN;
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) return sa_cancelled_here();
    *out = rt;
    return SA_OK;
}

static int64_t sa_io_check(SaObj* o) {
    if (!A_LOAD(&o->rt->reactor_running)) return SA_ESHUTDOWN;
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) return sa_cancelled_here();
    return SA_OK;
}

/* ---- sockets ----------------------------------------------------------------------------- */
typedef struct SaSock {
    SaObj obj;
    int fd;
    int32_t family;
    int32_t port;
    int32_t closed;   /* atomic */
    int64_t handle;   /* for reactor lookups (generation-checked) */
    sa_rwlock iolock; /* shared: a system call on fd; exclusive: closing (no fd-reuse races) */
    sa_mutex lock;    /* waiter lists and readiness sequences */
    uint64_t rd_seq, wr_seq;
    SaWaiter *rd_h, *rd_t, *wr_h, *wr_t;
} SaSock;

static void sa_sock_destroy(SaObj* o) {
    SaSock* k = (SaSock*)o;
    if (!k->closed) close(k->fd);
    A_DEC(&o->rt->st_socks);
    sa_rw_destroy(&k->iolock);
    sa_mutex_destroy(&k->lock);
    free(k);
}

/* Reactor side: the socket may be readable and/or writable now. */
static void sa_sock_ready(SaSock* k, int readable, int writable) {
    sa_mutex_lock(&k->lock);
    SaWaiter* w;
    if (readable) {
        k->rd_seq++;
        while ((w = sa_list_pop(&k->rd_h, &k->rd_t)) != NULL) {
            w->status = SA_OK;
            sa_waiter_fire(w);
        }
    }
    if (writable) {
        k->wr_seq++;
        while ((w = sa_list_pop(&k->wr_h, &k->wr_t)) != NULL) {
            w->status = SA_OK;
            sa_waiter_fire(w);
        }
    }
    sa_mutex_unlock(&k->lock);
}

static uint64_t sa_sock_seq(SaSock* k, int dir) {
    sa_mutex_lock(&k->lock);
    uint64_t s = dir ? k->wr_seq : k->rd_seq;
    sa_mutex_unlock(&k->lock);
    return s;
}

static void sa_poll_interest(SaRuntime* rt, SaSock* k) {
    SaReactor* r = (SaReactor*)rt->reactor;
    sa_mutex_lock(&r->ilock);
    int present = 0;
    for (int i = 0; i < r->n; i++) {
        if (r->interest[i] == k->handle) {
            present = 1;
            break;
        }
    }
    if (!present) {
        if (r->n == r->cap) {
            int ncap = r->cap ? r->cap * 2 : 64;
            int64_t* ni = (int64_t*)realloc(r->interest, sizeof(int64_t) * (size_t)ncap);
            if (ni) {
                r->interest = ni;
                r->cap = ncap;
            }
        }
        if (r->n < r->cap) r->interest[r->n++] = k->handle;
    }
    sa_mutex_unlock(&r->ilock);
    if (!present) sa_reactor_notify(rt);
}

/* Parks until the socket may be ready in direction `dir` (0 read, 1 write). `seq` is the
 * readiness sequence read before the failed system call. Returns SA_OK to retry, SA_ECLOSED, or
 * Cancelled. */
static int64_t sa_sock_wait(SaSock* k, int dir, uint64_t seq) {
    SaRuntime* rt = k->obj.rt;
    SaWaiter w;
    sa_waiter_init(&w);
    sa_mutex_lock(&k->lock);
    if (A_LOAD(&k->closed)) {
        sa_mutex_unlock(&k->lock);
        return SA_ECLOSED;
    }
    if ((dir ? k->wr_seq : k->rd_seq) != seq) { /* readiness arrived meanwhile: retry now */
        sa_mutex_unlock(&k->lock);
        return SA_OK;
    }
    if (dir) sa_list_push(&k->wr_h, &k->wr_t, &w);
    else sa_list_push(&k->rd_h, &k->rd_t, &w);
    A_INC(&rt->st_pending_io);
    sa_mutex_unlock(&k->lock);
    if (((SaReactor*)rt->reactor)->use_poll) sa_poll_interest(rt, k);
    int fired = sa_wait(&w, WK_IO);
    sa_mutex_lock(&k->lock);
    if (!A_LOAD(&w.fired)) {
        if (dir) sa_list_unlink(&k->wr_h, &k->wr_t, &w);
        else sa_list_unlink(&k->rd_h, &k->rd_t, &w);
    }
    A_DEC(&rt->st_pending_io);
    sa_mutex_unlock(&k->lock);
    if (!fired && !A_LOAD(&w.fired)) return sa_cancelled_here();
    return w.status;
}

static int sa_reactor_poll(SaRuntime* rt, int64_t timeout_ms) {
    SaReactor* r = (SaReactor*)rt->reactor;
    int to = timeout_ms < 0 ? -1 : (timeout_ms > 0x7FFFFFFF ? 0x7FFFFFFF : (int)timeout_ms);
#ifdef SA_HAVE_EPOLL
    if (!r->use_poll) {
        struct epoll_event evs[128];
        int n = epoll_wait(r->epfd, evs, 128, to);
        for (int i = 0; i < n; i++) {
            if (evs[i].data.u64 == 0) {
                sa_drain_wake(r);
                continue;
            }
            SaSock* k = (SaSock*)sa_lookup((int64_t)evs[i].data.u64, 0);
            if (!k) continue; /* closed since the event was queued */
            uint32_t e = evs[i].events;
            sa_sock_ready(k, (e & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0,
                          (e & (EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0);
            sa_obj_unref(&k->obj);
        }
        return !A_LOAD(&rt->reactor_stop);
    }
#endif
    /* poll mode: snapshot the sockets that have waiters, plus the wake pipe */
    sa_mutex_lock(&r->ilock);
    int n = r->n;
    int64_t* hs = (int64_t*)malloc(sizeof(int64_t) * (size_t)(n + 1));
    struct pollfd* pf = (struct pollfd*)malloc(sizeof(struct pollfd) * (size_t)(n + 1));
    if (hs && pf && n) memcpy(hs, r->interest, sizeof(int64_t) * (size_t)n);
    sa_mutex_unlock(&r->ilock);
    if (!hs || !pf) {
        free(hs);
        free(pf);
        struct pollfd one = {r->wake_rd, POLLIN, 0};
        poll(&one, 1, to < 0 || to > 10 ? 10 : to);
        return !A_LOAD(&rt->reactor_stop);
    }
    SaSock** ks = (SaSock**)calloc((size_t)(n + 1), sizeof(SaSock*));
    int m = 0;
    pf[m].fd = r->wake_rd;
    pf[m].events = POLLIN;
    pf[m].revents = 0;
    m++;
    for (int i = 0; i < n && ks; i++) {
        SaSock* k = (SaSock*)sa_lookup(hs[i], 0);
        if (!k) continue;
        sa_mutex_lock(&k->lock);
        short ev = (short)((k->rd_h ? POLLIN : 0) | (k->wr_h ? POLLOUT : 0));
        sa_mutex_unlock(&k->lock);
        if (!ev || A_LOAD(&k->closed)) {
            sa_obj_unref(&k->obj);
            continue;
        }
        ks[m] = k;
        pf[m].fd = k->fd;
        pf[m].events = ev;
        pf[m].revents = 0;
        m++;
    }
    int got = poll(pf, (nfds_t)m, to);
    if (got > 0 && (pf[0].revents & POLLIN)) sa_drain_wake(r);
    for (int i = 1; i < m; i++) {
        short re = pf[i].revents;
        if (got > 0 && re)
            sa_sock_ready(ks[i], (re & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0, (re & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0);
    }
    /* drop sockets without waiters (or gone) from the interest set */
    sa_mutex_lock(&r->ilock);
    int keep = 0;
    for (int i = 0; i < r->n; i++) {
        SaSock* k = (SaSock*)sa_lookup(r->interest[i], 0);
        int wanted = 0;
        if (k) {
            sa_mutex_lock(&k->lock);
            wanted = (k->rd_h || k->wr_h) && !A_LOAD(&k->closed);
            sa_mutex_unlock(&k->lock);
            sa_obj_unref(&k->obj);
        }
        if (wanted) r->interest[keep++] = r->interest[i];
    }
    r->n = keep;
    sa_mutex_unlock(&r->ilock);
    for (int i = 1; i < m; i++) sa_obj_unref(&ks[i]->obj);
    free(ks);
    free(hs);
    free(pf);
    return !A_LOAD(&rt->reactor_stop);
}

static int sa_socket(int family, int type) {
    int s = socket(family, type, 0);
    if (s < 0) return -1;
    if (!sa_set_nonblock(s)) {
        close(s);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    if (family == AF_INET6) {
        int v6only = 0; /* dual-stack: an IPv6 socket also serves IPv4-mapped peers */
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));
    }
    return s;
}

static int64_t sa_sock_wrap(SaRuntime* rt, int fd, int type, int family) {
    SaSock* k = (SaSock*)calloc(1, sizeof(SaSock));
    if (!k) {
        close(fd);
        return SA_EOS;
    }
    sa_obj_init(rt, &k->obj, type, 1, sa_sock_destroy);
    A_INC(&rt->st_socks);
    k->fd = fd;
    k->family = family;
    sa_rw_init(&k->iolock);
    sa_mutex_init(&k->lock);
    struct sockaddr_storage a;
    socklen_t al = sizeof(a);
    if (getsockname(fd, (struct sockaddr*)&a, &al) == 0) k->port = sa_sockaddr_port(&a);
    int64_t h = sa_tab_insert(rt, &k->obj);
    if (!h) {
        sa_obj_unref(&k->obj);
        return SA_EOS;
    }
    k->handle = h;
#ifdef SA_HAVE_EPOLL
    SaReactor* r = (SaReactor*)rt->reactor;
    if (!r->use_poll) {
        struct epoll_event ev;
        memset(&ev, 0, sizeof(ev));
        ev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
        ev.data.u64 = (uint64_t)h;
        if (epoll_ctl(r->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
            int e = errno;
            SaObj* o = sa_tab_remove(h, type);
            if (o) sa_obj_unref(o);
            return sa_map_errno(e);
        }
    }
#endif
    return h;
}

int64_t salivo_aio_tcp_listen(const char* host, int64_t port, int64_t backlog) {
    SaRuntime* rt;
    int64_t r = sa_io_ctx(&rt);
    if (r) return r;
    SaAddr addr;
    if ((r = sa_resolve(rt, host, port, &addr)) != SA_OK) return r;
    int fam = addr.ss.ss_family;
    int s = sa_socket(fam, SOCK_STREAM);
    if (s < 0) return sa_map_errno(errno);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)); /* restart without TIME_WAIT stalls */
    if (bind(s, (struct sockaddr*)&addr.ss, (socklen_t)addr.len) != 0 ||
        listen(s, backlog <= 0 ? SOMAXCONN : (int)(backlog > 0x7FFF ? 0x7FFF : backlog)) != 0) {
        int e = errno;
        close(s);
        return sa_map_errno(e);
    }
    return sa_sock_wrap(rt, s, OT_TCP_LISTENER, fam);
}

static void sa_nodelay(int s) {
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

int64_t salivo_aio_tcp_accept(int64_t h) {
    SaSock* ls = (SaSock*)sa_lookup(h, OT_TCP_LISTENER);
    if (!ls) return SA_EHANDLE;
    int64_t r = sa_io_check(&ls->obj);
    while (!r) {
        uint64_t seq = sa_sock_seq(ls, 0);
        sa_rw_rdlock(&ls->iolock);
        if (A_LOAD(&ls->closed)) {
            sa_rw_rdunlock(&ls->iolock);
            r = SA_ECLOSED;
            break;
        }
        int c = accept(ls->fd, NULL, NULL);
        int e = errno;
        sa_rw_rdunlock(&ls->iolock);
        if (c >= 0) {
            if (!sa_set_nonblock(c)) {
                close(c);
                continue;
            }
#ifdef SO_NOSIGPIPE
            int one = 1;
            setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
            sa_nodelay(c);
            r = sa_sock_wrap(ls->obj.rt, c, OT_TCP_STREAM, ls->family);
            break;
        }
        if (e == EINTR || e == ECONNABORTED) continue;
        if (e == EAGAIN || e == EWOULDBLOCK) {
            r = sa_sock_wait(ls, 0, seq);
            continue;
        }
        r = sa_map_errno(e);
    }
    sa_obj_unref(&ls->obj);
    return r;
}

int64_t salivo_aio_tcp_connect(const char* host, int64_t port) {
    SaRuntime* rt;
    int64_t r = sa_io_ctx(&rt);
    if (r) return r;
    SaAddr addr;
    if ((r = sa_resolve(rt, host, port, &addr)) != SA_OK) return r;
    if (port == 0 || sa_addr_is_any(&addr)) return SA_EINVAL;
    int fam = addr.ss.ss_family;
    int s = sa_socket(fam, SOCK_STREAM);
    if (s < 0) return sa_map_errno(errno);
    sa_nodelay(s);
    /* Wrap first so the reactor watches the socket while the connection is in progress. */
    int64_t kh = sa_sock_wrap(rt, s, OT_TCP_STREAM, fam);
    if (kh < 0) return kh;
    SaSock* k = (SaSock*)sa_lookup(kh, OT_TCP_STREAM);
    uint64_t seq = sa_sock_seq(k, 1);
    int rc = connect(s, (struct sockaddr*)&addr.ss, (socklen_t)addr.len);
    int e = rc == 0 ? 0 : errno;
    if (rc != 0 && (e == EINPROGRESS || e == EINTR)) {
        for (;;) {
            r = sa_sock_wait(k, 1, seq);
            if (r) break;
            int so = 0;
            socklen_t sl = sizeof(so);
            getsockopt(s, SOL_SOCKET, SO_ERROR, &so, &sl);
            if (so == 0) {
                /* writable and no error: connected unless the peer is still pending */
                struct sockaddr_storage pa;
                socklen_t pl = sizeof(pa);
                if (getpeername(s, (struct sockaddr*)&pa, &pl) == 0) {
                    e = 0;
                    break;
                }
                seq = sa_sock_seq(k, 1);
                continue;
            }
            e = so;
            break;
        }
    }
    if (!r && e) r = sa_map_errno(e);
    sa_obj_unref(&k->obj);
    if (r) {
        SaObj* o = sa_tab_remove(kh, OT_TCP_STREAM);
        if (o) sa_obj_unref(o); /* closes the fd */
        return r;
    }
    struct sockaddr_storage la;
    socklen_t ll = sizeof(la);
    if (getsockname(s, (struct sockaddr*)&la, &ll) == 0) k->port = sa_sockaddr_port(&la);
    return kh;
}

/* recv/send with readiness waits. dir 0 = read, 1 = write. */
static int64_t sa_sock_io(SaSock* k, int dir, char* p, int64_t n, int* out_errno) {
    for (;;) {
        uint64_t seq = sa_sock_seq(k, dir);
        sa_rw_rdlock(&k->iolock);
        if (A_LOAD(&k->closed)) {
            sa_rw_rdunlock(&k->iolock);
            return SA_ECLOSED;
        }
        ssize_t got = dir ? send(k->fd, p, (size_t)n, MSG_NOSIGNAL) : recv(k->fd, p, (size_t)n, 0);
        int e = errno;
        sa_rw_rdunlock(&k->iolock);
        if (got >= 0) return got;
        if (e == EINTR) continue;
        if (e == EAGAIN || e == EWOULDBLOCK) {
            int64_t r = sa_sock_wait(k, dir, seq);
            if (r) return r;
            continue;
        }
        *out_errno = e;
        return sa_map_errno(e);
    }
}

static int64_t sa_sock_read(SaSock* k, int64_t buf, int64_t cap) {
    int e = 0;
    int64_t r = sa_sock_io(k, 0, (char*)(intptr_t)buf, cap, &e);
    if (r == 0) return SA_EEOF;
    return r;
}

static int64_t sa_sock_write(SaSock* k, int64_t buf, int64_t len) {
    int64_t total = 0;
    while (total < len) {
        SaTask* self = sa_current_task();
        if (self && A_LOAD(&self->cancel_req)) {
            sa_set_last(total);
            return sa_cancelled_here();
        }
        int e = 0;
        int64_t r = sa_sock_io(k, 1, (char*)(intptr_t)(buf + total), len - total, &e);
        if (r < 0) {
            sa_set_last(total);
            return r;
        }
        total += r;
    }
    return total;
}

int64_t salivo_aio_tcp_shutdown(int64_t h) {
    SaSock* k = (SaSock*)sa_lookup(h, OT_TCP_STREAM);
    if (!k) return SA_EHANDLE;
    int64_t r = sa_io_check(&k->obj);
    if (!r) {
        sa_rw_rdlock(&k->iolock);
        r = A_LOAD(&k->closed) ? SA_ECLOSED : (shutdown(k->fd, SHUT_WR) == 0 ? SA_OK : sa_map_errno(errno));
        sa_rw_rdunlock(&k->iolock);
    }
    sa_obj_unref(&k->obj);
    return r;
}

int64_t salivo_aio_local_port(int64_t h) {
    SaObj* o = sa_lookup(h, 0);
    if (!o) return SA_EHANDLE;
    int64_t r = (o->type == OT_TCP_LISTENER || o->type == OT_TCP_STREAM || o->type == OT_UDP) ? ((SaSock*)o)->port : SA_EINVAL;
    sa_obj_unref(o);
    return r;
}

/* ---- UDP --------------------------------------------------------------------------------- */
int64_t salivo_aio_udp_bind(const char* host, int64_t port) {
    SaRuntime* rt;
    int64_t r = sa_io_ctx(&rt);
    if (r) return r;
    SaAddr addr;
    if ((r = sa_resolve(rt, host, port, &addr)) != SA_OK) return r;
    int fam = addr.ss.ss_family;
    int s = sa_socket(fam, SOCK_DGRAM);
    if (s < 0) return sa_map_errno(errno);
    if (bind(s, (struct sockaddr*)&addr.ss, (socklen_t)addr.len) != 0) {
        int e = errno;
        close(s);
        return sa_map_errno(e);
    }
    return sa_sock_wrap(rt, s, OT_UDP, fam);
}

static void sa_map_v4_to_v6(SaAddr* a) {
    struct sockaddr_in v4 = *(struct sockaddr_in*)&a->ss;
    struct sockaddr_in6* v6 = (struct sockaddr_in6*)&a->ss;
    memset(v6, 0, sizeof(*v6));
    v6->sin6_family = AF_INET6;
    v6->sin6_port = v4.sin_port;
    v6->sin6_addr.s6_addr[10] = 0xFF;
    v6->sin6_addr.s6_addr[11] = 0xFF;
    memcpy(&v6->sin6_addr.s6_addr[12], &v4.sin_addr, 4);
    a->len = (int)sizeof(*v6);
}

int64_t salivo_aio_udp_send_to(int64_t h, int64_t buf, int64_t len, const char* host, int64_t port) {
    if (len < 0 || len > 65507 || (len > 0 && buf == 0) || port <= 0 || port > 65535) return SA_EINVAL;
    SaSock* k = (SaSock*)sa_lookup(h, OT_UDP);
    if (!k) return SA_EHANDLE;
    int64_t r = sa_io_check(&k->obj);
    SaAddr peer;
    if (!r) r = sa_resolve(k->obj.rt, host, port, &peer);
    if (!r && sa_addr_is_any(&peer)) r = SA_EINVAL;
    if (!r && peer.ss.ss_family != k->family) {
        if (k->family == AF_INET6) sa_map_v4_to_v6(&peer);
        else r = SA_EINVAL;
    }
    while (!r) {
        uint64_t seq = sa_sock_seq(k, 1);
        sa_rw_rdlock(&k->iolock);
        if (A_LOAD(&k->closed)) {
            sa_rw_rdunlock(&k->iolock);
            r = SA_ECLOSED;
            break;
        }
        ssize_t n = sendto(k->fd, (const char*)(intptr_t)buf, (size_t)len, MSG_NOSIGNAL, (struct sockaddr*)&peer.ss, (socklen_t)peer.len);
        int e = errno;
        sa_rw_rdunlock(&k->iolock);
        if (n >= 0) {
            r = n;
            break;
        }
        if (e == EINTR) continue;
        if (e == EAGAIN || e == EWOULDBLOCK || e == ENOBUFS) {
            r = sa_sock_wait(k, 1, seq);
            continue;
        }
        r = e == ECONNREFUSED ? SA_OK : sa_map_errno(e); /* ICMP from an earlier datagram: not this send's error */
        if (r == SA_OK) continue;
    }
    sa_obj_unref(&k->obj);
    return r;
}

int64_t salivo_aio_udp_recv_from(int64_t h, int64_t buf, int64_t cap) {
    if (buf == 0 || cap <= 0 || cap > 0x7FFFFFFF) return SA_EINVAL;
    SaSock* k = (SaSock*)sa_lookup(h, OT_UDP);
    if (!k) return SA_EHANDLE;
    int64_t r = sa_io_check(&k->obj);
    while (!r) {
        uint64_t seq = sa_sock_seq(k, 0);
        struct sockaddr_storage peer;
        struct iovec iov;
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = (void*)(intptr_t)buf;
        iov.iov_len = (size_t)cap;
        msg.msg_name = &peer;
        msg.msg_namelen = sizeof(peer);
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        sa_rw_rdlock(&k->iolock);
        if (A_LOAD(&k->closed)) {
            sa_rw_rdunlock(&k->iolock);
            r = SA_ECLOSED;
            break;
        }
        ssize_t n = recvmsg(k->fd, &msg, 0);
        int e = errno;
        sa_rw_rdunlock(&k->iolock);
        if (n >= 0) {
            sa_record_peer(&peer);
            r = (msg.msg_flags & MSG_TRUNC) ? SA_ETRUNC : n;
            break;
        }
        if (e == EINTR || e == ECONNREFUSED) continue; /* ICMP from an earlier send */
        if (e == EAGAIN || e == EWOULDBLOCK) {
            r = sa_sock_wait(k, 0, seq);
            continue;
        }
        r = sa_map_errno(e);
    }
    sa_obj_unref(&k->obj);
    return r;
}

/* ---- files (blocking pool) --------------------------------------------------------------- */
typedef struct SaFile {
    SaObj obj;
    int fd;
    sa_rwlock iolock;
    int32_t closed; /* atomic */
    int32_t append;
    int32_t busy; /* atomic: enforces one positional user at a time */
    int64_t pos;
} SaFile;

static void sa_file_destroy(SaObj* o) {
    SaFile* f = (SaFile*)o;
    if (!f->closed) close(f->fd);
    A_DEC(&o->rt->st_files);
    sa_rw_destroy(&f->iolock);
    free(f);
}

static int64_t sa_open_job(SaJob* j) {
    int flags;
    switch (j->a) {
    case 0: flags = O_RDONLY; break;
    case 1: flags = O_WRONLY | O_CREAT | O_TRUNC; break;
    case 2: flags = O_WRONLY | O_CREAT | O_APPEND; break;
    default: flags = O_RDWR | O_CREAT; break;
    }
    int fd = open((const char*)j->q, flags | O_CLOEXEC, 0644);
    if (fd < 0) {
        j->os_error = errno; /* ENOENT = 2, same code as Windows ERROR_FILE_NOT_FOUND */
        return SA_EOS;
    }
    j->b = fd;
    return SA_OK;
}

/* a = 0 read, 1 write; p = file; b = buffer; c = length */
static int64_t sa_file_rw_job(SaJob* j) {
    SaFile* f = (SaFile*)j->p;
    sa_rw_rdlock(&f->iolock);
    int64_t r;
    if (A_LOAD(&f->closed)) {
        r = SA_ECLOSED;
    } else {
        for (;;) {
            ssize_t n;
            if (j->a == 0) n = pread(f->fd, (void*)(intptr_t)j->b, (size_t)j->c, (off_t)f->pos);
            else if (f->append) n = write(f->fd, (const void*)(intptr_t)j->b, (size_t)j->c);
            else n = pwrite(f->fd, (const void*)(intptr_t)j->b, (size_t)j->c, (off_t)f->pos);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) {
                j->os_error = errno;
                r = SA_EOS;
            } else {
                r = n;
            }
            break;
        }
    }
    sa_rw_rdunlock(&f->iolock);
    return r;
}

static int64_t sa_flush_job(SaJob* j) {
    SaFile* f = (SaFile*)j->p;
    sa_rw_rdlock(&f->iolock);
    int64_t r = A_LOAD(&f->closed) ? SA_ECLOSED : (fsync(f->fd) == 0 ? SA_OK : SA_EOS);
    if (r == SA_EOS) j->os_error = errno;
    sa_rw_rdunlock(&f->iolock);
    return r;
}

static int64_t sa_file_close_job(SaJob* j) {
    SaFile* f = (SaFile*)j->p;
    sa_rw_wrlock(&f->iolock);
    A_STORE(&f->closed, 1);
    close(f->fd);
    sa_rw_wrunlock(&f->iolock);
    return SA_OK;
}

int64_t salivo_aio_file_open(const char* path, int64_t mode) {
    SaRuntime* rt;
    int64_t r = sa_io_ctx(&rt);
    if (r) return r;
    if (!path || !*path || mode < 0 || mode > 3) return SA_EINVAL;
    SaJob j;
    memset(&j, 0, sizeof(j));
    j.run = sa_open_job;
    j.q = path;
    j.a = mode;
    r = sa_pool_run(rt, &j);
    if (r != SA_OK) {
        if (r == SA_EOS) sa_set_os_error(j.os_error);
        return r;
    }
    SaFile* f = (SaFile*)calloc(1, sizeof(SaFile));
    if (!f) {
        close((int)j.b);
        return SA_EOS;
    }
    sa_obj_init(rt, &f->obj, OT_FILE, 1, sa_file_destroy);
    A_INC(&rt->st_files);
    f->fd = (int)j.b;
    f->append = mode == 2;
    sa_rw_init(&f->iolock);
    int64_t fh = sa_tab_insert(rt, &f->obj);
    if (!fh) {
        sa_obj_unref(&f->obj);
        return SA_EOS;
    }
    return fh;
}

static int64_t sa_file_xfer(SaFile* f, int write, int64_t buf, int64_t n) {
    SaJob j;
    memset(&j, 0, sizeof(j));
    j.run = sa_file_rw_job;
    j.a = write;
    j.p = f;
    j.b = buf;
    j.c = n;
    int64_t r = sa_pool_run(f->obj.rt, &j);
    if (r == SA_EOS) {
        sa_set_os_error(j.os_error);
        return sa_map_errno((int)j.os_error);
    }
    return r;
}

static int64_t sa_file_read(SaFile* f, int64_t buf, int64_t cap) {
    int64_t r = sa_file_xfer(f, 0, buf, cap);
    if (r == 0) return SA_EEOF;
    if (r > 0) f->pos += r;
    return r;
}

static int64_t sa_file_write(SaFile* f, int64_t buf, int64_t len) {
    int64_t total = 0;
    while (total < len) {
        SaTask* self = sa_current_task();
        if (self && A_LOAD(&self->cancel_req)) {
            sa_set_last(total);
            return sa_cancelled_here();
        }
        int64_t chunk = len - total > (1 << 30) ? (1 << 30) : len - total;
        int64_t r = sa_file_xfer(f, 1, buf + total, chunk);
        if (r < 0) {
            sa_set_last(total);
            return r;
        }
        if (r == 0) {
            sa_set_last(total);
            return SA_EOS;
        }
        if (!f->append) f->pos += r;
        total += r;
    }
    return total;
}

/* Claims the file position for one read, write or seek. A concurrent second positional user on
 * the same handle fails with Invalid instead of racing on the position. */
static int sa_file_claim(SaFile* f) {
    int32_t e = 0;
    return A_CAS(&f->busy, &e, 1);
}

int64_t salivo_aio_file_seek(int64_t h, int64_t off, int64_t whence) {
    if (whence < 0 || whence > 2) return SA_EINVAL;
    SaFile* f = (SaFile*)sa_lookup(h, OT_FILE);
    if (!f) return SA_EHANDLE;
    if (!sa_file_claim(f)) {
        sa_obj_unref(&f->obj);
        return SA_EINVAL;
    }
    int64_t base = 0, r;
    if (whence == 1) base = f->pos;
    if (whence == 2) {
        struct stat st;
        sa_rw_rdlock(&f->iolock);
        int ok = !A_LOAD(&f->closed) && fstat(f->fd, &st) == 0;
        int e = errno;
        sa_rw_rdunlock(&f->iolock);
        if (!ok) {
            A_STORE(&f->busy, 0);
            sa_obj_unref(&f->obj);
            return sa_map_errno(e);
        }
        base = (int64_t)st.st_size;
    }
    r = base + off;
    if (r < 0) r = SA_EINVAL;
    else f->pos = r;
    A_STORE(&f->busy, 0);
    sa_obj_unref(&f->obj);
    return r;
}

int64_t salivo_aio_flush(int64_t h) {
    SaFile* f = (SaFile*)sa_lookup(h, OT_FILE);
    if (!f) return SA_EHANDLE;
    int64_t r = sa_io_check(&f->obj);
    if (!r) {
        SaJob j;
        memset(&j, 0, sizeof(j));
        j.run = sa_flush_job;
        j.p = f;
        r = sa_pool_run(f->obj.rt, &j);
        if (r == SA_EOS) sa_set_os_error(j.os_error);
    }
    sa_obj_unref(&f->obj);
    return r;
}

/* ---- generic read / write / close -------------------------------------------------------- */
int64_t salivo_aio_read(int64_t h, int64_t buf, int64_t cap) {
    if (buf == 0 || cap <= 0 || cap > 0x7FFFFFFF) return SA_EINVAL;
    SaObj* o = sa_lookup(h, 0);
    if (!o) return SA_EHANDLE;
    int64_t r = sa_io_check(o);
    if (!r) {
        if (o->type == OT_TCP_STREAM) r = sa_sock_read((SaSock*)o, buf, cap);
        else if (o->type == OT_FILE) {
            if (!sa_file_claim((SaFile*)o)) r = SA_EINVAL;
            else {
                r = sa_file_read((SaFile*)o, buf, cap);
                A_STORE(&((SaFile*)o)->busy, 0);
            }
        }
        else r = SA_EINVAL;
    }
    sa_obj_unref(o);
    return r;
}

int64_t salivo_aio_write(int64_t h, int64_t buf, int64_t len) {
    if (len < 0 || (len > 0 && buf == 0)) return SA_EINVAL;
    SaObj* o = sa_lookup(h, 0);
    if (!o) return SA_EHANDLE;
    int64_t r = sa_io_check(o);
    if (!r) {
        if (o->type == OT_TCP_STREAM) r = sa_sock_write((SaSock*)o, buf, len);
        else if (o->type == OT_FILE) {
            if (!sa_file_claim((SaFile*)o)) r = SA_EINVAL;
            else {
                r = sa_file_write((SaFile*)o, buf, len);
                A_STORE(&((SaFile*)o)->busy, 0);
            }
        }
        else r = SA_EINVAL;
    }
    sa_obj_unref(o);
    return r;
}

int64_t salivo_aio_write_text(int64_t h, const char* text) {
    if (!text) return SA_EINVAL;
    return salivo_aio_write(h, (int64_t)(intptr_t)text, (int64_t)strlen(text));
}

int64_t salivo_aio_close(int64_t h) {
    SaObj* o = sa_lookup(h, 0);
    if (!o) return SA_EHANDLE;
    int type = o->type;
    SaRuntime* rt = o->rt;
    sa_obj_unref(o);
    if (type == OT_TASK) return SA_EINVAL; /* tasks are released, not closed */
    o = sa_tab_remove(h, type);
    if (!o) return SA_EHANDLE; /* lost a race with another close */
    if (type == OT_CHAN) {
        SaChan* c = (SaChan*)o;
        sa_mutex_lock(&c->m);
        sa_chan_close_locked_fire(c);
        sa_mutex_unlock(&c->m);
    } else if (type == OT_FILE) {
        SaFile* f = (SaFile*)o;
        SaJob j;
        memset(&j, 0, sizeof(j));
        j.run = sa_file_close_job;
        j.p = f;
        /* Uncancellable: a cancelled closer must still release the OS handle. */
        if (!A_LOAD(&rt->reactor_running) || sa_pool_run_ex(rt, &j, 0) != SA_OK) sa_file_close_job(&j);
    } else {
        SaSock* k = (SaSock*)o;
        sa_rw_wrlock(&k->iolock); /* no system call is in progress on the fd now */
        A_STORE(&k->closed, 1);
#ifdef SA_HAVE_EPOLL
        SaReactor* r = (SaReactor*)rt->reactor;
        if (!r->use_poll) epoll_ctl(r->epfd, EPOLL_CTL_DEL, k->fd, NULL);
#endif
        close(k->fd);
        sa_rw_wrunlock(&k->iolock);
        /* waiters wake with Closed */
        sa_mutex_lock(&k->lock);
        SaWaiter* w;
        while ((w = sa_list_pop(&k->rd_h, &k->rd_t)) != NULL) {
            w->status = SA_ECLOSED;
            sa_waiter_fire(w);
        }
        while ((w = sa_list_pop(&k->wr_h, &k->wr_t)) != NULL) {
            w->status = SA_ECLOSED;
            sa_waiter_fire(w);
        }
        sa_mutex_unlock(&k->lock);
    }
    sa_obj_unref(o);
    return SA_OK;
}
