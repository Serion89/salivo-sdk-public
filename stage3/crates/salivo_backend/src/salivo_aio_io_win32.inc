/* Salivo Stage 36.1 async runtime - Windows I/O backend (included by salivo_aio_runtime.c).
 *
 * Completion model: every socket and file is associated with the runtime's I/O completion port.
 * An operation is issued as overlapped I/O (WSARecv, WSASend, AcceptEx, ConnectEx, WSARecvFrom,
 * WSASendTo, ReadFile, WriteFile), the issuing task parks, and the reactor thread wakes it when
 * the completion packet arrives. No worker thread ever blocks in the OS for socket or file data.
 * Sockets are IPv4 or IPv6, following the family of the resolved address; IPv6 sockets are
 * dual-stack.
 *
 * Blocking-pool operations (not native completions): CreateFileW, FlushFileBuffers, file
 * CloseHandle and getaddrinfo. Those can block inside the OS, so they never run on a worker.
 *
 * Cancellation: a cancelled task calls CancelIoEx for its own OVERLAPPED and keeps waiting until
 * the (aborted) completion arrives, so the kernel never writes into a frame that has returned.
 * An operation that completed despite the cancellation returns its real result (no data loss).
 */

#ifndef SO_REUSE_UNICASTPORT
#define SO_REUSE_UNICASTPORT 0x3007
#endif

#define SA_KEY_IO 1
#define SA_KEY_WAKE 2

typedef LONG(NTAPI* sa_status_to_dos_fn)(LONG);
static sa_status_to_dos_fn sa_status_to_dos;
static LPFN_ACCEPTEX sa_AcceptEx;
static LPFN_CONNECTEX sa_ConnectEx;

static int sa_net_init(void) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return 0;
    if (!sa_status_to_dos)
        sa_status_to_dos = (sa_status_to_dos_fn)(void*)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlNtStatusToDosError");
    if (!sa_AcceptEx || !sa_ConnectEx) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s != INVALID_SOCKET) {
            GUID ga = WSAID_ACCEPTEX, gc = WSAID_CONNECTEX;
            DWORD n = 0;
            WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &ga, sizeof(ga), &sa_AcceptEx, sizeof(sa_AcceptEx), &n, NULL, NULL);
            WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER, &gc, sizeof(gc), &sa_ConnectEx, sizeof(sa_ConnectEx), &n, NULL, NULL);
            closesocket(s);
        }
    }
    if (!sa_status_to_dos || !sa_AcceptEx || !sa_ConnectEx) {
        WSACleanup();
        return 0;
    }
    return 1;
}

static void sa_net_fini(void) { WSACleanup(); }

/* The IOCP wait timeout is rounded to the system timer tick (15.6 ms by default), which would make
 * every runtime timer up to a tick late. While a runtime is running we request 1 ms resolution,
 * as the Go runtime does (higher power use while it runs; released when the runtime is destroyed).
 * winmm is loaded dynamically so programs need no extra link library. */
typedef unsigned(WINAPI* sa_time_period_fn)(unsigned);
static void sa_timer_resolution(int enable) {
    static HMODULE winmm;
    static sa_time_period_fn begin, end;
    if (!winmm) {
        winmm = LoadLibraryA("winmm.dll");
        if (!winmm) return;
        begin = (sa_time_period_fn)(void*)GetProcAddress(winmm, "timeBeginPeriod");
        end = (sa_time_period_fn)(void*)GetProcAddress(winmm, "timeEndPeriod");
    }
    if (enable && begin) begin(1);
    if (!enable && end) end(1);
}

static int sa_reactor_init(SaRuntime* rt) {
    HANDLE p = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    if (!p) return 0;
    rt->reactor = p;
    sa_timer_resolution(1);
    return 1;
}

static void sa_reactor_fini(SaRuntime* rt) {
    if (rt->reactor) {
        CloseHandle((HANDLE)rt->reactor);
        sa_timer_resolution(0);
    }
    rt->reactor = NULL;
}

static void sa_reactor_notify(SaRuntime* rt) {
    PostQueuedCompletionStatus((HANDLE)rt->reactor, 0, SA_KEY_WAKE, NULL);
}

/* ---- operations ------------------------------------------------------------------------- */
typedef struct SaIoOp {
    OVERLAPPED ov;
    SaWaiter w;
    int32_t done; /* atomic */
    DWORD bytes;
    DWORD err; /* Win32 error, 0 = success */
} SaIoOp;

static void sa_io_begin(SaRuntime* rt, SaIoOp* op) {
    memset(op, 0, sizeof(*op));
    sa_waiter_init(&op->w);
    /* The reactor holds this reference until it has finished waking the task. */
    if (op->w.task) sa_obj_ref(&op->w.task->obj);
    A_INC(&rt->st_pending_io);
}

/* The OS rejected the request synchronously: no completion packet will ever arrive. */
static void sa_io_abort(SaRuntime* rt, SaIoOp* op) {
    if (op->w.task) sa_obj_unref(&op->w.task->obj);
    A_DEC(&rt->st_pending_io);
}

static void sa_io_complete(SaRuntime* rt, OVERLAPPED_ENTRY* e) {
    SaIoOp* op = CONTAINING_RECORD(e->lpOverlapped, SaIoOp, ov);
    LONG st = (LONG)e->lpOverlapped->Internal;
    op->bytes = e->dwNumberOfBytesTransferred;
    op->err = st == 0 ? 0 : (DWORD)sa_status_to_dos(st);
    A_DEC(&rt->st_pending_io);
    SaTask* t = op->w.task;
    SaParker* p = op->w.parker;
    if (t) {
        A_STORE(&op->done, 1); /* op may be gone after this store; only `t` is used below */
        sa_task_wake(t);
        sa_obj_unref(&t->obj);
    } else {
        sa_mutex_lock(&p->m);
        A_STORE(&op->done, 1);
        sa_cond_broadcast(&p->c);
        sa_mutex_unlock(&p->m);
    }
}

static int sa_reactor_poll(SaRuntime* rt, int64_t timeout_ms) {
    OVERLAPPED_ENTRY ents[128];
    ULONG n = 0;
    DWORD to = timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms;
    if (GetQueuedCompletionStatusEx((HANDLE)rt->reactor, ents, 128, &n, to, FALSE)) {
        for (ULONG i = 0; i < n; i++)
            if (ents[i].lpCompletionKey == SA_KEY_IO && ents[i].lpOverlapped) sa_io_complete(rt, &ents[i]);
    }
    return !A_LOAD(&rt->reactor_stop);
}

/* Waits for the completion. `lk`/`closed` guard the handle so CancelIoEx never targets a closed
 * (possibly reused) handle value. Returns 1 when this call requested the cancellation. */
static int sa_io_wait(SaIoOp* op, HANDLE h, sa_rwlock* lk, int32_t* closed) {
    SaTask* t = op->w.task;
    int cancel_sent = 0;
    if (t) {
        while (!A_LOAD(&op->done)) {
            if (!cancel_sent && A_LOAD(&t->cancel_req)) {
                sa_rw_rdlock(lk);
                if (!A_LOAD(closed)) CancelIoEx(h, &op->ov);
                sa_rw_rdunlock(lk);
                cancel_sent = 1;
                continue;
            }
            sa_park_task(t, WK_IO);
        }
    } else {
        SaParker* p = op->w.parker;
        sa_mutex_lock(&p->m);
        while (!A_LOAD(&op->done)) sa_cond_wait(&p->c, &p->m);
        sa_mutex_unlock(&p->m);
    }
    return cancel_sent;
}

static int64_t sa_map_err(DWORD e, int cancel_sent) {
    sa_set_os_error((int64_t)e);
    switch (e) {
    case 0: return SA_OK;
    case ERROR_HANDLE_EOF:
    case ERROR_BROKEN_PIPE: return SA_EEOF;
    case ERROR_OPERATION_ABORTED: return cancel_sent ? sa_cancelled_here() : SA_ECLOSED;
    case ERROR_NETNAME_DELETED:
    case ERROR_CONNECTION_ABORTED:
    case WSAECONNRESET:
    case WSAECONNABORTED:
    case WSAENETRESET: return SA_ERESET;
    case ERROR_CONNECTION_REFUSED:
    case ERROR_PORT_UNREACHABLE:
    case WSAECONNREFUSED: return SA_EREFUSED;
    case ERROR_MORE_DATA:
    case WSAEMSGSIZE: return SA_ETRUNC;
    case WSAEADDRINUSE: return SA_EADDRINUSE;
    case ERROR_INVALID_HANDLE:
    case WSAENOTSOCK: return SA_ECLOSED;
    case ERROR_INVALID_PARAMETER:
    case WSAEINVAL:
    case WSAEAFNOSUPPORT:
    case WSAEADDRNOTAVAIL: return SA_EINVAL;
    case ERROR_SEM_TIMEOUT:
    case WSAETIMEDOUT: return SA_ETIMEOUT;
    default: return SA_EOS;
    }
}

/* Runtime for a handle-less I/O call, and the caller's cancellation. */
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

/* The object's runtime must be running and the caller not cancelled. */
static int64_t sa_io_check(SaObj* o) {
    if (!A_LOAD(&o->rt->reactor_running)) return SA_ESHUTDOWN;
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) return sa_cancelled_here();
    return SA_OK;
}

/* ---- sockets ----------------------------------------------------------------------------- */
typedef struct SaSock {
    SaObj obj;
    SOCKET s;
    sa_rwlock iolock; /* shared: issuing an operation; exclusive: closing */
    int32_t closed;   /* atomic */
    int32_t port;
    int32_t family;
} SaSock;

static void sa_sock_destroy(SaObj* o) {
    SaSock* k = (SaSock*)o;
    if (!k->closed) closesocket(k->s);
    A_DEC(&o->rt->st_socks);
    free(k);
}

/* Wraps an overlapped socket; `associate` = 0 when it is already bound to the completion port. */
static int64_t sa_sock_wrap(SaRuntime* rt, SOCKET s, int type, int family, int associate) {
    if (associate && !CreateIoCompletionPort((HANDLE)s, (HANDLE)rt->reactor, SA_KEY_IO, 0)) {
        DWORD e = GetLastError();
        closesocket(s);
        return sa_map_err(e, 0);
    }
    SaSock* k = (SaSock*)calloc(1, sizeof(SaSock));
    if (!k) {
        closesocket(s);
        return SA_EOS;
    }
    sa_obj_init(rt, &k->obj, type, 1, sa_sock_destroy);
    A_INC(&rt->st_socks);
    k->s = s;
    k->family = family;
    sa_rw_init(&k->iolock);
    struct sockaddr_storage a;
    int al = sizeof(a);
    if (getsockname(s, (struct sockaddr*)&a, &al) == 0) k->port = sa_sockaddr_port(&a);
    int64_t h = sa_tab_insert(rt, &k->obj);
    if (!h) {
        sa_obj_unref(&k->obj);
        return SA_EOS;
    }
    return h;
}

static SOCKET sa_socket(int family, int type, int proto) {
    SOCKET s = WSASocketW(family, type, proto, NULL, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (s != INVALID_SOCKET && family == AF_INET6) {
        DWORD v6only = 0; /* dual-stack: an IPv6 socket also serves IPv4-mapped peers */
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
    }
    return s;
}

int64_t salivo_aio_tcp_listen(const char* host, int64_t port, int64_t backlog) {
    SaRuntime* rt;
    int64_t r = sa_io_ctx(&rt);
    if (r) return r;
    SaAddr addr;
    if ((r = sa_resolve(rt, host, port, &addr)) != SA_OK) return r;
    int fam = addr.ss.ss_family;
    SOCKET s = sa_socket(fam, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return sa_map_err(WSAGetLastError(), 0);
    BOOL excl = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof(excl));
    if (bind(s, (struct sockaddr*)&addr.ss, addr.len) != 0 ||
        listen(s, backlog <= 0 ? SOMAXCONN : (int)(backlog > 0x7FFF ? 0x7FFF : backlog)) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        return sa_map_err(e, 0);
    }
    return sa_sock_wrap(rt, s, OT_TCP_LISTENER, fam, 1);
}

static void sa_nodelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof(on));
}

int64_t salivo_aio_tcp_accept(int64_t h) {
    SaSock* ls = (SaSock*)sa_lookup(h, OT_TCP_LISTENER);
    if (!ls) return SA_EHANDLE;
    SaRuntime* rt = ls->obj.rt;
    int64_t r = sa_io_check(&ls->obj);
    if (r) {
        sa_obj_unref(&ls->obj);
        return r;
    }
    SOCKET as = sa_socket(ls->family, SOCK_STREAM, IPPROTO_TCP);
    if (as == INVALID_SOCKET) {
        sa_obj_unref(&ls->obj);
        return sa_map_err(WSAGetLastError(), 0);
    }
    char addrs[2 * (sizeof(struct sockaddr_in6) + 16)];
    DWORD got = 0;
    SaIoOp op;
    sa_rw_rdlock(&ls->iolock);
    if (A_LOAD(&ls->closed)) {
        sa_rw_rdunlock(&ls->iolock);
        closesocket(as);
        sa_obj_unref(&ls->obj);
        return SA_ECLOSED;
    }
    sa_io_begin(rt, &op);
    BOOL ok = sa_AcceptEx(ls->s, as, addrs, 0, sizeof(struct sockaddr_in6) + 16, sizeof(struct sockaddr_in6) + 16, &got, &op.ov);
    int e = ok ? 0 : WSAGetLastError();
    sa_rw_rdunlock(&ls->iolock);
    if (!ok && e != ERROR_IO_PENDING) {
        sa_io_abort(rt, &op);
        closesocket(as);
        sa_obj_unref(&ls->obj);
        return sa_map_err(e, 0);
    }
    int cs = sa_io_wait(&op, (HANDLE)ls->s, &ls->iolock, &ls->closed);
    if (op.err) {
        closesocket(as);
        r = sa_map_err(op.err, cs);
    } else {
        setsockopt(as, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (const char*)&ls->s, sizeof(ls->s));
        sa_nodelay(as);
        r = sa_sock_wrap(rt, as, OT_TCP_STREAM, ls->family, 1);
    }
    sa_obj_unref(&ls->obj);
    return r;
}

int64_t salivo_aio_tcp_connect(const char* host, int64_t port) {
    SaRuntime* rt;
    int64_t r = sa_io_ctx(&rt);
    if (r) return r;
    SaAddr addr, any;
    if ((r = sa_resolve(rt, host, port, &addr)) != SA_OK) return r;
    if (port == 0 || sa_addr_is_any(&addr)) return SA_EINVAL;
    int fam = addr.ss.ss_family;
    SOCKET s = sa_socket(fam, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return sa_map_err(WSAGetLastError(), 0);
    /* ConnectEx needs a bound socket, and bind(port 0) would reserve an ephemeral port for every
     * destination. SO_REUSE_UNICASTPORT defers the choice to connect time so ports are shared
     * across destinations; without it, bursts of short connections exhaust the ephemeral range
     * while closed connections sit in TIME_WAIT. */
    DWORD reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSE_UNICASTPORT, (const char*)&reuse, sizeof(reuse));
    memset(&any, 0, sizeof(any));
    any.ss.ss_family = (ADDRESS_FAMILY)fam;
    any.len = fam == AF_INET6 ? (int)sizeof(struct sockaddr_in6) : (int)sizeof(struct sockaddr_in);
    if (bind(s, (struct sockaddr*)&any.ss, any.len) != 0 ||
        !CreateIoCompletionPort((HANDLE)s, (HANDLE)rt->reactor, SA_KEY_IO, 0)) {
        int e = WSAGetLastError();
        closesocket(s);
        return sa_map_err(e, 0);
    }
    SaIoOp op;
    sa_io_begin(rt, &op);
    BOOL ok = sa_ConnectEx(s, (struct sockaddr*)&addr.ss, addr.len, NULL, 0, NULL, &op.ov);
    int e = ok ? 0 : WSAGetLastError();
    if (!ok && e != ERROR_IO_PENDING) {
        sa_io_abort(rt, &op);
        closesocket(s);
        return sa_map_err(e, 0);
    }
    sa_rwlock lk = SRWLOCK_INIT;
    int32_t never_closed = 0;
    int cs = sa_io_wait(&op, (HANDLE)s, &lk, &never_closed);
    if (op.err) {
        closesocket(s);
        return sa_map_err(op.err, cs);
    }
    setsockopt(s, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    sa_nodelay(s);
    return sa_sock_wrap(rt, s, OT_TCP_STREAM, fam, 0);
}

/* One overlapped socket transfer. kind: 0 recv, 1 send, 2 recvfrom, 3 sendto. */
static int64_t sa_sock_xfer(SaSock* k, int kind, char* p, int64_t n, SaAddr* peer, SaIoOp* op, int* cs) {
    WSABUF b;
    b.len = (ULONG)n;
    b.buf = p;
    DWORD flags = 0;
    INT plen = (INT)sizeof(struct sockaddr_storage);
    int rc;
    sa_rw_rdlock(&k->iolock);
    if (A_LOAD(&k->closed)) {
        sa_rw_rdunlock(&k->iolock);
        return SA_ECLOSED;
    }
    sa_io_begin(k->obj.rt, op);
    switch (kind) {
    case 0: rc = WSARecv(k->s, &b, 1, NULL, &flags, &op->ov, NULL); break;
    case 1: rc = WSASend(k->s, &b, 1, NULL, 0, &op->ov, NULL); break;
    case 2: rc = WSARecvFrom(k->s, &b, 1, NULL, &flags, (struct sockaddr*)&peer->ss, &plen, &op->ov, NULL); break;
    default: rc = WSASendTo(k->s, &b, 1, NULL, 0, (struct sockaddr*)&peer->ss, peer->len, &op->ov, NULL); break;
    }
    int e = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    sa_rw_rdunlock(&k->iolock);
    if (rc == SOCKET_ERROR && e != WSA_IO_PENDING) {
        sa_io_abort(k->obj.rt, op);
        return sa_map_err(e, 0);
    }
    /* `plen` lives in this frame and the kernel may write it until completion: wait here. */
    *cs = sa_io_wait(op, (HANDLE)k->s, &k->iolock, &k->closed);
    return SA_OK;
}

static int64_t sa_sock_read(SaSock* k, int64_t buf, int64_t cap) {
    SaIoOp op;
    int cs = 0;
    int64_t r = sa_sock_xfer(k, 0, (char*)(intptr_t)buf, cap, NULL, &op, &cs);
    if (r) return r;
    if (op.err) return sa_map_err(op.err, cs);
    return op.bytes == 0 ? SA_EEOF : (int64_t)op.bytes;
}

/* Writes every byte, looping over partial completions. On error the bytes already written are
 * reported through rtaiolastvalue. */
static int64_t sa_sock_write(SaSock* k, int64_t buf, int64_t len) {
    int64_t total = 0;
    while (total < len) {
        SaTask* self = sa_current_task();
        if (self && A_LOAD(&self->cancel_req)) {
            sa_set_last(total);
            return sa_cancelled_here();
        }
        int64_t chunk = len - total > (1 << 30) ? (1 << 30) : len - total;
        SaIoOp op;
        int cs = 0;
        int64_t r = sa_sock_xfer(k, 1, (char*)(intptr_t)(buf + total), chunk, NULL, &op, &cs);
        if (r) {
            sa_set_last(total);
            return r;
        }
        if (op.err) {
            r = sa_map_err(op.err, cs);
            sa_set_last(total + op.bytes);
            return r;
        }
        if (op.bytes == 0) {
            sa_set_last(total);
            return SA_EOS;
        }
        total += op.bytes;
    }
    return total;
}

int64_t salivo_aio_tcp_shutdown(int64_t h) {
    SaSock* k = (SaSock*)sa_lookup(h, OT_TCP_STREAM);
    if (!k) return SA_EHANDLE;
    int64_t r = sa_io_check(&k->obj);
    if (!r) {
        sa_rw_rdlock(&k->iolock);
        r = A_LOAD(&k->closed) ? SA_ECLOSED : (shutdown(k->s, SD_SEND) == 0 ? SA_OK : sa_map_err(WSAGetLastError(), 0));
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
    SOCKET s = sa_socket(fam, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return sa_map_err(WSAGetLastError(), 0);
    /* Without this, an ICMP port-unreachable from an earlier send fails later receives. */
    BOOL off = FALSE;
    DWORD n = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), NULL, 0, &n, NULL, NULL);
    if (bind(s, (struct sockaddr*)&addr.ss, addr.len) != 0) {
        int e = WSAGetLastError();
        closesocket(s);
        return sa_map_err(e, 0);
    }
    return sa_sock_wrap(rt, s, OT_UDP, fam, 1);
}

/* An IPv4 destination on an IPv6 (dual-stack) socket is sent as an IPv4-mapped address. */
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
        else r = SA_EINVAL; /* IPv6 destination from an IPv4 socket */
    }
    if (!r) {
        SaIoOp op;
        int cs = 0;
        r = sa_sock_xfer(k, 3, (char*)(intptr_t)buf, len, &peer, &op, &cs);
        if (!r) r = op.err ? sa_map_err(op.err, cs) : (int64_t)op.bytes;
    }
    sa_obj_unref(&k->obj);
    return r;
}

int64_t salivo_aio_udp_recv_from(int64_t h, int64_t buf, int64_t cap) {
    if (buf == 0 || cap <= 0 || cap > 0x7FFFFFFF) return SA_EINVAL;
    SaSock* k = (SaSock*)sa_lookup(h, OT_UDP);
    if (!k) return SA_EHANDLE;
    int64_t r = sa_io_check(&k->obj);
    if (!r) {
        SaAddr peer;
        memset(&peer, 0, sizeof(peer));
        SaIoOp op;
        int cs = 0;
        r = sa_sock_xfer(k, 2, (char*)(intptr_t)buf, cap, &peer, &op, &cs);
        if (!r) {
            if (op.err) {
                r = sa_map_err(op.err, cs);
                if (r == SA_ETRUNC) sa_record_peer(&peer.ss);
            } else {
                sa_record_peer(&peer.ss);
                r = (int64_t)op.bytes;
            }
        }
    }
    sa_obj_unref(&k->obj);
    return r;
}

/* ---- files ------------------------------------------------------------------------------- */
typedef struct SaFile {
    SaObj obj;
    HANDLE h;
    sa_rwlock iolock;
    int32_t closed; /* atomic */
    int32_t append;
    int32_t busy; /* atomic: enforces one positional user at a time */
    int64_t pos;
} SaFile;

static void sa_file_destroy(SaObj* o) {
    SaFile* f = (SaFile*)o;
    if (!f->closed) CloseHandle(f->h);
    A_DEC(&o->rt->st_files);
    free(f);
}

static int64_t sa_open_job(SaJob* j) {
    const char* path = (const char*)j->q;
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (n <= 0) return SA_EINVAL;
    wchar_t* wp = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)n);
    if (!wp) return SA_EOS;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, n);
    DWORD acc, disp;
    switch (j->a) {
    case 0: acc = GENERIC_READ; disp = OPEN_EXISTING; break;
    case 1: acc = GENERIC_WRITE; disp = CREATE_ALWAYS; break;
    case 2: acc = FILE_APPEND_DATA | SYNCHRONIZE; disp = OPEN_ALWAYS; break;
    default: acc = GENERIC_READ | GENERIC_WRITE; disp = OPEN_ALWAYS; break;
    }
    HANDLE h = CreateFileW(wp, acc, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
    free(wp);
    if (h == INVALID_HANDLE_VALUE) {
        j->os_error = GetLastError(); /* ERROR_FILE_NOT_FOUND = 2, same as POSIX ENOENT */
        return SA_EOS;
    }
    j->p = h;
    return SA_OK;
}

static int64_t sa_flush_job(SaJob* j) {
    SaFile* f = (SaFile*)j->p;
    sa_rw_rdlock(&f->iolock);
    int64_t r = A_LOAD(&f->closed) ? SA_ECLOSED : (FlushFileBuffers(f->h) ? SA_OK : SA_EOS);
    if (r == SA_EOS) j->os_error = GetLastError();
    sa_rw_rdunlock(&f->iolock);
    return r;
}

static int64_t sa_file_close_job(SaJob* j) {
    SaFile* f = (SaFile*)j->p;
    sa_rw_wrlock(&f->iolock); /* waits for in-progress issues and flushes on this handle */
    A_STORE(&f->closed, 1);
    CancelIoEx(f->h, NULL);
    CloseHandle(f->h);
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
    HANDLE h = (HANDLE)j.p;
    if (!CreateIoCompletionPort(h, (HANDLE)rt->reactor, SA_KEY_IO, 0)) {
        DWORD e = GetLastError();
        CloseHandle(h);
        return sa_map_err(e, 0);
    }
    SaFile* f = (SaFile*)calloc(1, sizeof(SaFile));
    if (!f) {
        CloseHandle(h);
        return SA_EOS;
    }
    sa_obj_init(rt, &f->obj, OT_FILE, 1, sa_file_destroy);
    A_INC(&rt->st_files);
    f->h = h;
    f->append = mode == 2;
    sa_rw_init(&f->iolock);
    int64_t fh = sa_tab_insert(rt, &f->obj);
    if (!fh) {
        sa_obj_unref(&f->obj);
        return SA_EOS;
    }
    return fh;
}

static int64_t sa_file_xfer(SaFile* f, int write, int64_t buf, int64_t n, SaIoOp* op, int* cs) {
    sa_rw_rdlock(&f->iolock);
    if (A_LOAD(&f->closed)) {
        sa_rw_rdunlock(&f->iolock);
        return SA_ECLOSED;
    }
    sa_io_begin(f->obj.rt, op);
    if (write && f->append) {
        op->ov.Offset = 0xFFFFFFFF;
        op->ov.OffsetHigh = 0xFFFFFFFF;
    } else {
        op->ov.Offset = (DWORD)(f->pos & 0xFFFFFFFF);
        op->ov.OffsetHigh = (DWORD)(f->pos >> 32);
    }
    BOOL ok = write ? WriteFile(f->h, (const void*)(intptr_t)buf, (DWORD)n, NULL, &op->ov)
                    : ReadFile(f->h, (void*)(intptr_t)buf, (DWORD)n, NULL, &op->ov);
    DWORD e = ok ? 0 : GetLastError();
    sa_rw_rdunlock(&f->iolock);
    if (!ok && e != ERROR_IO_PENDING) {
        sa_io_abort(f->obj.rt, op);
        return e == ERROR_HANDLE_EOF ? SA_EEOF : sa_map_err(e, 0);
    }
    *cs = sa_io_wait(op, f->h, &f->iolock, &f->closed);
    return SA_OK;
}

static int64_t sa_file_read(SaFile* f, int64_t buf, int64_t cap) {
    SaIoOp op;
    int cs = 0;
    int64_t r = sa_file_xfer(f, 0, buf, cap, &op, &cs);
    if (r) return r;
    if (op.err) return op.err == ERROR_HANDLE_EOF ? SA_EEOF : sa_map_err(op.err, cs);
    if (op.bytes == 0) return SA_EEOF;
    f->pos += op.bytes;
    return op.bytes;
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
        SaIoOp op;
        int cs = 0;
        int64_t r = sa_file_xfer(f, 1, buf + total, chunk, &op, &cs);
        if (r) {
            sa_set_last(total);
            return r;
        }
        if (op.err) {
            r = sa_map_err(op.err, cs);
            sa_set_last(total + op.bytes);
            return r;
        }
        if (op.bytes == 0) {
            sa_set_last(total);
            return SA_EOS;
        }
        if (!f->append) f->pos += op.bytes;
        total += op.bytes;
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
        LARGE_INTEGER sz;
        sa_rw_rdlock(&f->iolock);
        BOOL ok = !A_LOAD(&f->closed) && GetFileSizeEx(f->h, &sz);
        sa_rw_rdunlock(&f->iolock);
        if (!ok) {
            A_STORE(&f->busy, 0);
            sa_obj_unref(&f->obj);
            return sa_map_err(GetLastError(), 0);
        }
        base = sz.QuadPart;
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

/* Gathered write of two buffers in one WSASend (used by the HTTP layer so a frame header and
 * its payload leave in one system call without copying the payload). A partial completion
 * finishes through sa_sock_write. Returns n1 + n2 or an error. */
static int64_t sa_sock_write2(SaSock* k, const char* p1, int64_t n1, const char* p2, int64_t n2) {
    SaTask* self = sa_current_task();
    if (self && A_LOAD(&self->cancel_req)) return sa_cancelled_here();
    if (n1 + n2 > (1 << 30)) {
        int64_t r = sa_sock_write(k, (int64_t)(intptr_t)p1, n1);
        return r < 0 ? r : sa_sock_write(k, (int64_t)(intptr_t)p2, n2) < 0 ? SA_EOS : n1 + n2;
    }
    WSABUF b[2];
    b[0].len = (ULONG)n1;
    b[0].buf = (char*)p1;
    b[1].len = (ULONG)n2;
    b[1].buf = (char*)p2;
    SaIoOp op;
    sa_rw_rdlock(&k->iolock);
    if (A_LOAD(&k->closed)) {
        sa_rw_rdunlock(&k->iolock);
        return SA_ECLOSED;
    }
    sa_io_begin(k->obj.rt, &op);
    int rc = WSASend(k->s, b, 2, NULL, 0, &op.ov, NULL);
    int e = rc == SOCKET_ERROR ? WSAGetLastError() : 0;
    sa_rw_rdunlock(&k->iolock);
    if (rc == SOCKET_ERROR && e != WSA_IO_PENDING) {
        sa_io_abort(k->obj.rt, &op);
        return sa_map_err(e, 0);
    }
    int cs = sa_io_wait(&op, (HANDLE)k->s, &k->iolock, &k->closed);
    if (op.err) return sa_map_err(op.err, cs);
    int64_t done = (int64_t)op.bytes, r;
    if (done < n1 && (r = sa_sock_write(k, (int64_t)(intptr_t)(p1 + done), n1 - done)) < 0) return r;
    if (done < n1) done = n1;
    if (done - n1 < n2 && (r = sa_sock_write(k, (int64_t)(intptr_t)(p2 + (done - n1)), n2 - (done - n1))) < 0) return r;
    return n1 + n2;
}

static int64_t sa_aio_write2(int64_t h, const void* p1, int64_t n1, const void* p2, int64_t n2) {
    SaObj* o = sa_lookup(h, OT_TCP_STREAM);
    if (!o) return SA_EHANDLE;
    int64_t r = sa_io_check(o);
    if (!r) r = sa_sock_write2((SaSock*)o, (const char*)p1, n1, (const char*)p2, n2);
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
    if (type == OT_TASK || type == OT_HTTP_CONN || type == OT_HTTP_EX) return SA_EINVAL; /* released / closed through their own calls */
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
        sa_rw_wrlock(&k->iolock);
        A_STORE(&k->closed, 1);
        closesocket(k->s); /* aborts pending operations; they complete with Closed */
        sa_rw_wrunlock(&k->iolock);
    }
    sa_obj_unref(o);
    return SA_OK;
}
