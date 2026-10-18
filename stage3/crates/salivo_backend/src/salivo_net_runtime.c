#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>


typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket(s) close(s)
#define SD_BOTH SHUT_RDWR
#define WSAGetLastError() errno
#endif

/* Length argument type of send/recv: int on Winsock, size_t on POSIX. Callers pass n > 0;
   Winsock lengths clamp to INT_MAX (a partial transfer, reported by the return value). */
#ifdef _WIN32
typedef int salivo_io_len_t;
static salivo_io_len_t salivo_io_len(long long n) { return n > INT_MAX ? INT_MAX : (int)n; }
#else
typedef size_t salivo_io_len_t;
static salivo_io_len_t salivo_io_len(long long n) { return (size_t)n; }
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

/* Strings returned to Salivo code come from the program's string allocator (salivo_task_runtime.c),
   so Stage 3 reclaims them like any other string */
void* salivo_rt_smalloc(long long n);
static char* net_out_str(const char* s, size_t n) {
    char* r = (char*)salivo_rt_smalloc((long long)n + 1);
    if (!r) return 0;
    if (n) memcpy(r, s, n);
    r[n] = 0;
    return r;
}
static char* net_out_cstr(const char* s) { return net_out_str(s ? s : "", s ? strlen(s) : 0); }

#define MAX_SOCKET_HANDLES 1024

#ifdef SALIVO_DEBUG
#define SALIVO_NET_LOG(...) do { printf(__VA_ARGS__); fflush(stdout); } while(0)
#else
#define SALIVO_NET_LOG(...) do {} while(0)
#endif

typedef struct {
    SOCKET sock;
    int is_active;
    int is_listener;
    int port;
    char peer_ip[64];
    int peer_port;
} SalivoSocketSlot;

static SalivoSocketSlot g_socket_table[MAX_SOCKET_HANDLES + 1];
static int g_wsa_initialized = 0;

static void ensure_wsa_init(void) {
#ifdef _WIN32
    if (!g_wsa_initialized) {
        WSADATA wsa;
        int err = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (err != 0) {
            printf("[RT] WSAStartup failed: %d\n", err);
            fflush(stdout);
        } else {
            g_wsa_initialized = 1;
        }
    }
#else
    g_wsa_initialized = 1;
#endif
}

static long long alloc_socket_handle(SOCKET s, int is_listener, int port) {
    for (int i = 1; i <= MAX_SOCKET_HANDLES; i++) {
        if (!g_socket_table[i].is_active) {
            g_socket_table[i].sock = s;
            g_socket_table[i].is_active = 1;
            g_socket_table[i].is_listener = is_listener;
            g_socket_table[i].port = port;
            return (long long)i;
        }
    }
    return -1;
}

static SOCKET get_socket_by_handle(long long handle) {
    if (handle < 1 || handle > MAX_SOCKET_HANDLES) {
        return INVALID_SOCKET;
    }
    if (!g_socket_table[handle].is_active) {
        return INVALID_SOCKET;
    }
    return g_socket_table[handle].sock;
}

long long salivo_net_listen_addr(const char* bind_ip, long long port) {
    ensure_wsa_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        printf("[RT] socket creation failed: %d\n", WSAGetLastError());
        fflush(stdout);
        return -1;
    }
    int opt = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (bind_ip && strlen(bind_ip) > 0 && strcmp(bind_ip, "0.0.0.0") != 0) {
        addr.sin_addr.s_addr = inet_addr(bind_ip);
    } else {
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }

    int b_res = bind(s, (struct sockaddr*)&addr, sizeof(addr));
    if (b_res != 0) {
        printf("[RT] bind failed: %d\n", WSAGetLastError());
        fflush(stdout);
        closesocket(s);
        return -1;
    }
    int l_res = listen(s, 128);
    if (l_res != 0) {
        printf("[RT] listen failed: %d\n", WSAGetLastError());
        fflush(stdout);
        closesocket(s);
        return -1;
    }
    long long handle = alloc_socket_handle(s, 1, (int)port);
    if (handle < 0) {
        printf("[RT] socket handle allocation failed\n");
        fflush(stdout);
        closesocket(s);
        return -1;
    }
    SALIVO_NET_LOG("[RT] salivo_net_listen_addr success handle=%lld (os_socket=%llu)\n", handle, (unsigned long long)s);
    return handle;
}

long long salivo_net_listen(long long port) {
    return salivo_net_listen_addr("127.0.0.1", port);
}

long long salivo_net_connect_addr(const char* target_ip, long long port) {
    ensure_wsa_init();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        SALIVO_NET_LOG("[RT] socket creation failed: %d\n", WSAGetLastError());
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (target_ip && strlen(target_ip) > 0) {
        addr.sin_addr.s_addr = inet_addr(target_ip);
    } else {
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    }

    int c_res = connect(s, (struct sockaddr*)&addr, sizeof(addr));
    if (c_res != 0) {
        SALIVO_NET_LOG("[RT] connect failed: %d\n", WSAGetLastError());
        closesocket(s);
        return -1;
    }
    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    int bufsize = 65536;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));

    long long handle = alloc_socket_handle(s, 0, (int)port);
    if (handle < 0) {
        SALIVO_NET_LOG("[RT] socket handle allocation failed\n");
        closesocket(s);
        return -1;
    }
    SALIVO_NET_LOG("[RT] salivo_net_connect_addr success handle=%lld (os_socket=%llu)\n", handle, (unsigned long long)s);
    return handle;
}

long long salivo_net_connect(long long port) {
    return salivo_net_connect_addr("127.0.0.1", port);
}

long long salivo_net_accept(long long listener_handle) {
    SOCKET listener_s = get_socket_by_handle(listener_handle);
    if (listener_s == INVALID_SOCKET) {
        SALIVO_NET_LOG("[RT] salivo_net_accept error: invalid listener handle %lld\n", listener_handle);
        return -1;
    }

    struct sockaddr_in addr;
#ifdef _WIN32
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    SOCKET client_s = accept(listener_s, (struct sockaddr*)&addr, &len);
    if (client_s == INVALID_SOCKET) {
        SALIVO_NET_LOG("[RT] accept failed: %d\n", WSAGetLastError());
        return -1;
    }
    int nodelay = 1;
    setsockopt(client_s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    int bufsize = 65536;
    setsockopt(client_s, SOL_SOCKET, SO_RCVBUF, (const char*)&bufsize, sizeof(bufsize));
    setsockopt(client_s, SOL_SOCKET, SO_SNDBUF, (const char*)&bufsize, sizeof(bufsize));

    long long client_handle = alloc_socket_handle(client_s, 0, 0);
    if (client_handle < 0) {
        SALIVO_NET_LOG("[RT] client socket handle allocation failed\n");
        closesocket(client_s);
        return -1;
    }
    // Store real peer IP and port
    inet_ntop(AF_INET, &addr.sin_addr, g_socket_table[client_handle].peer_ip, sizeof(g_socket_table[client_handle].peer_ip));
    g_socket_table[client_handle].peer_port = ntohs(addr.sin_port);
    SALIVO_NET_LOG("[RT] salivo_net_accept success client_handle=%lld (os_socket=%llu) peer=%s:%d\n",
           client_handle, (unsigned long long)client_s,
           g_socket_table[client_handle].peer_ip, g_socket_table[client_handle].peer_port);
    return client_handle;
}

long long salivo_net_send(long long handle, const char* data) {
    SOCKET s = get_socket_by_handle(handle);
    if (s == INVALID_SOCKET) {
        SALIVO_NET_LOG("[RT] salivo_net_send error: invalid socket handle %lld\n", handle);
        return -1;
    }
    if (!data) return 0;
    size_t len = strlen(data);
    int sent = (int)send(s, data, salivo_io_len((long long)len), 0);
    SALIVO_NET_LOG("[RT] salivo_net_send handle=%lld sent=%d bytes\n", handle, sent);
    return (long long)sent;
}

/* What this thread's last salivo_net_recv got: bytes (> 0), 0 peer closed, -1 error */
#ifdef _WIN32
static __declspec(thread) long long g_net_last_recv = 0;
#else
static __thread long long g_net_last_recv = 0;
#endif

long long salivo_net_last_recv(void) { return g_net_last_recv; }

char* salivo_net_recv(long long handle, long long max_bytes) {
    g_net_last_recv = -1;
    SOCKET s = get_socket_by_handle(handle);
    if (s == INVALID_SOCKET) {
        SALIVO_NET_LOG("[RT] salivo_net_recv error: invalid socket handle %lld\n", handle);
        return net_out_cstr("");
    }
    if (max_bytes <= 0) max_bytes = 4096;
    char* buf = (char*)malloc((size_t)max_bytes + 1);
    if (!buf) return net_out_cstr("");
    int recvd = (int)recv(s, buf, salivo_io_len(max_bytes), 0);
    g_net_last_recv = recvd < 0 ? -1 : recvd;
    if (recvd < 0) recvd = 0;
    buf[recvd] = '\0';
    SALIVO_NET_LOG("[RT] salivo_net_recv handle=%lld recvd=%d bytes: '%s'\n", handle, recvd, buf);
    char* r = net_out_str(buf, (size_t)recvd);
    free(buf);
    return r;
}

long long salivo_net_close(long long handle) {
    if (handle < 1 || handle > MAX_SOCKET_HANDLES) {
        SALIVO_NET_LOG("[RT] salivo_net_close error: out of bounds handle %lld\n", handle);
        return -1;
    }
    if (!g_socket_table[handle].is_active) {
        SALIVO_NET_LOG("[RT] salivo_net_close warning: double-close or inactive handle %lld\n", handle);
        return 0; // Safe double-close handling
    }
    SOCKET s = g_socket_table[handle].sock;
    shutdown(s, SD_BOTH);
    closesocket(s);
    g_socket_table[handle].is_active = 0;
    g_socket_table[handle].sock = INVALID_SOCKET;
    SALIVO_NET_LOG("[RT] salivo_net_close success closed handle=%lld\n", handle);
    return 0;
}

/* First address of hostname as text: an IPv4 address when the host has one, else IPv6; "" if none */
char* salivo_net_resolve(const char* hostname) {
    ensure_wsa_init();
    if (!hostname || hostname[0] == '\0') return net_out_cstr("");
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(hostname, NULL, &hints, &res) != 0 || !res) return net_out_cstr("");
    const struct addrinfo* pick = NULL;
    for (const struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) { pick = ai; break; }
        if (ai->ai_family == AF_INET6 && !pick) pick = ai;
    }
    char ip[INET6_ADDRSTRLEN] = "";
    if (pick && pick->ai_family == AF_INET)
        inet_ntop(AF_INET, &((struct sockaddr_in*)pick->ai_addr)->sin_addr, ip, sizeof(ip));
    else if (pick)
        inet_ntop(AF_INET6, &((struct sockaddr_in6*)pick->ai_addr)->sin6_addr, ip, sizeof(ip));
    freeaddrinfo(res);
    return net_out_cstr(ip);
}

/* Host name of an IPv4 or IPv6 address; the address itself when it has no name or does not parse */
char* salivo_net_reverse_lookup(const char* ip_str) {
    ensure_wsa_init();
    if (!ip_str || ip_str[0] == '\0') return net_out_cstr("127.0.0.1");
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_NUMERICHOST;
    if (getaddrinfo(ip_str, NULL, &hints, &res) != 0 || !res) return net_out_cstr(ip_str);
    char host[NI_MAXHOST];
    int rc = getnameinfo(res->ai_addr, (socklen_t)res->ai_addrlen, host, sizeof(host), NULL, 0, NI_NAMEREQD);
    freeaddrinfo(res);
    return net_out_cstr(rc == 0 ? host : ip_str);
}

long long salivo_net_send_buf(long long handle, const char* buf_ptr, long long len_bytes) {
    SOCKET s = get_socket_by_handle(handle);
    if (s == INVALID_SOCKET) return -1;
    if (!buf_ptr || len_bytes <= 0) return 0;
    int sent = (int)send(s, buf_ptr, salivo_io_len(len_bytes), 0);
    return (long long)sent;
}

long long salivo_net_recv_buf(long long handle, char* buf_ptr, long long max_bytes) {
    SOCKET s = get_socket_by_handle(handle);
    if (s == INVALID_SOCKET) return -1;
    if (!buf_ptr || max_bytes <= 0) return 0;
    int recvd = (int)recv(s, buf_ptr, salivo_io_len(max_bytes), 0);
    return recvd < 0 ? -1 : (long long)recvd; /* -1 error, 0 peer closed */
}

/* Peer IPv4 address packed as a.b.c.d -> (a << 24) | (b << 16) | (c << 8) | d; -1 if unknown.
   Allocation-free counterpart of salivo_net_get_peer_ip. */
long long salivo_net_get_peer_ipv4(long long handle) {
    if (handle < 1 || handle > MAX_SOCKET_HANDLES || !g_socket_table[handle].is_active) return -1;
    unsigned a, b, c, d;
    if (sscanf(g_socket_table[handle].peer_ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
    return ((long long)a << 24) | ((long long)b << 16) | ((long long)c << 8) | (long long)d;
}

char* salivo_net_get_peer_ip(long long handle) {
    if (handle < 1 || handle > MAX_SOCKET_HANDLES || !g_socket_table[handle].is_active || g_socket_table[handle].peer_ip[0] == '\0')
        return net_out_cstr("0.0.0.0");
    return net_out_cstr(g_socket_table[handle].peer_ip);
}

long long salivo_net_get_peer_port(long long handle) {
    if (handle < 1 || handle > MAX_SOCKET_HANDLES || !g_socket_table[handle].is_active) {
        return 0;
    }
    return (long long)g_socket_table[handle].peer_port;
}


