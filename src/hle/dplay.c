/*
 * joyride - DirectPlay (dplayx.dll) for GTA's network game.
 *
 * What the game uses (its network code is at 0x486800..0x487A00):
 *   DirectPlayEnumerateA (#2)   list service providers; the player picks one
 *   DirectPlayCreate (#1)       then QueryInterface(IID_IDirectPlay2A)
 *   IDirectPlay2A: EnumSessions, Open (create / join), CreatePlayer,
 *                  EnumPlayers (polled for the lobby), Send, Receive,
 *                  DestroyPlayer, Close, Release
 * Receive only keeps messages addressed to the local player from a player
 * the game knows; DirectPlay's system messages would be ignored, so none are
 * generated.
 *
 * The transport is joyride's own, so only joyride instances play together:
 * one service provider ("TCP/IP"), a star around the host. The host listens
 * on TCP, relays every message and keeps the roster of players; clients
 * connect to it. Games are found by UDP broadcast on the local network, and
 * at JOYRIDE_NET_HOST=<address> for one beyond it (the internet). Port 47624,
 * DirectPlay's own (JOYRIDE_NET_PORT to change it). Everything travels over
 * TCP, guaranteed or not: four players of 1997 need no more.
 *
 * A network thread does all the socket work into locked queues; the shims
 * touch guest memory only on the game's thread, and give the machine up
 * while they wait (enumerating, connecting, joining).
 *
 * POSIX sockets; on Windows, Winsock with the differences below.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
/* Winsock: sockets closed with closesocket, made non-blocking with
 * ioctlsocket, polled with WSAPoll, their errors in WSAGetLastError, their
 * options' values as char pointers; started once */
#include <winsock2.h>
#include <ws2tcpip.h>
#define close(fd) closesocket(fd)
#define poll WSAPoll
typedef ULONG nfds_t;
#define MSG_NOSIGNAL 0
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
static int sock_fcntl(int fd, int cmd, int flags) {
    if (cmd != F_SETFL) return 0;
    u_long on = (flags & O_NONBLOCK) != 0;
    return ioctlsocket((SOCKET)fd, FIONBIO, &on);
}
#define fcntl(fd, cmd, ...) sock_fcntl(fd, cmd, 0 __VA_OPT__(+) __VA_ARGS__)
#define setsockopt(s, l, o, v, n) setsockopt(s, l, o, (const char *)(v), n)
static int sock_errno(void) {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK ? EWOULDBLOCK : e == WSAEINTR ? EINTR : e == WSAEINPROGRESS ? EINPROGRESS : e;
}
__attribute__((constructor)) static void sock_start(void) { WSADATA w; WSAStartup(MAKEWORD(2, 2), &w); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#define sock_errno() errno
#endif

#include "win32hle.h"
#include "joyride_hle.h"

/* ---- DirectPlay constants ------------------------------------------------ */

#define DP_OK                 0x00000000u
#define DPERR_BUFFERTOOSMALL  0x8877001Eu
#define DPERR_CANTCREATEPLAYER 0x8877003Cu
#define DPERR_CANTCREATESESSION 0x88770046u
#define DPERR_INVALIDPARAMS   0x80070057u
#define DPERR_INVALIDPLAYER   0x88770096u
#define DPERR_NOCONNECTION    0x887700AAu
#define DPERR_NOMESSAGES      0x887700BEu
#define DPERR_NOSESSIONS      0x887700DCu
#define DPERR_TIMEOUT         0x887700F0u
#define DPERR_UNSUPPORTED     0x80004001u
#define DPERR_SESSIONLOST     0x8877012Cu

#define DPOPEN_JOIN    1u
#define DPOPEN_CREATE  2u
#define DPESC_TIMEDOUT 1u
#define DPPLAYERTYPE_PLAYER 1u
#define DPID_ALLPLAYERS 0u

/* DPSESSIONDESC2 (ANSI) */
#define SD_SIZE        0x50
#define SD_FLAGS       0x04
#define SD_INSTANCE    0x08
#define SD_APPLICATION 0x18
#define SD_MAXPLAYERS  0x28
#define SD_CURPLAYERS  0x2C
#define SD_NAME        0x30

static const uint8_t DPSPGUID_TCPIP[16] = {   /* {36E95EE0-8577-11cf-960C-0080C7534E82} */
    0xE0, 0x5E, 0xE9, 0x36, 0x77, 0x85, 0xCF, 0x11, 0x96, 0x0C, 0x00, 0x80, 0xC7, 0x53, 0x4E, 0x82 };

/* The rules of a network game that are joyride's, not the game's: the
 * host's, fixed when it creates the session and sent to each guest as it
 * joins, used by every machine until the session ends (each must run the
 * same game). -1: not in a network game. */
#define RULE_DRIVEBY 1u
static volatile int g_rules = -1;
int joyride_net_driveby(void) { int r = g_rules; return r < 0 ? -1 : (int)(r & RULE_DRIVEBY); }

/* ---- wire protocol --------------------------------------------------------
 * TCP frames: u32 length (of what follows), u8 type, payload. All little-endian.
 * UDP datagrams: "JOYR" u8 type, payload. */
enum {
    W_ENUM_REQ = 1,     /* UDP  guidApplication[16] */
    W_ENUM_REPLY,       /* UDP  instance[16] app[16] max u32 cur u32 port u16 name\0 */
    W_JOIN,             /* c>h  instance[16] */
    W_JOIN_OK,          /* h>c  max u32, joyride's rules u32 (bit 0: drive-bys always;
                           absent from older hosts: none), then roster as W_PLAYER_ADD frames */
    W_JOIN_REFUSED,     /* h>c  */
    W_CREATE_PLAYER,    /* c>h  token u32 name\0 */
    W_PLAYER_ID,        /* h>c  token u32 id u32 */
    W_PLAYER_ADD,       /* h>c  id u32 name\0 */
    W_PLAYER_DEL,       /* h>c  id u32 / c>h: destroy my player id */
    W_DATA,             /* both from u32 to u32 flags u32 data */
};
#define MAGIC "JOYR"
#define MAX_PLAYERS 16
#define MAX_CONNS 8
#define MAX_SESSIONS 16
#define MAX_MSG 4096

typedef struct msg { struct msg *next; uint32_t from, to, n; uint8_t data[]; } msg_t;

typedef struct {
    uint32_t id;
    char name[64];
    int conn;               /* host: the connection that owns it, -1: this machine */
    int local;              /* created by this machine's game */
} player_t;

typedef struct {
    int fd;
    uint8_t in[65536 + 8];
    size_t in_n;
} conn_t;

typedef struct {
    uint8_t instance[16], app[16];
    uint32_t max, cur;
    char name[64];
    struct sockaddr_in addr;     /* the host's TCP address */
} session_t;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    volatile int running, stop;
    int is_host, lost;
    int port;
    /* host */
    int listen_fd, udp_fd;
    conn_t conns[MAX_CONNS];
    uint8_t instance[16], app[16];
    uint32_t max_players;
    char session_name[64];
    uint32_t next_id;
    /* client */
    int server_fd;
    conn_t server;
    int joined, refused;
    uint32_t pending_token, pending_id;
    /* both */
    player_t players[MAX_PLAYERS];
    msg_t *q_head, *q_tail;      /* messages for this machine's players */
    session_t sessions[MAX_SESSIONS];
    int nsessions;
} N = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER,
        .listen_fd = -1, .udp_fd = -1, .server_fd = -1 };

static int net_port(void) {
    const char *e = getenv("JOYRIDE_NET_PORT");
    return e && atoi(e) > 0 ? atoi(e) : 47624;
}

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---- frames --------------------------------------------------------------- */

static int send_all(int fd, const void *buf, size_t n) {
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        int e = k < 0 ? sock_errno() : 0;
        if (k < 0 && e == EINTR) continue;
        if (k < 0 && (e == EAGAIN || e == EWOULDBLOCK)) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            if (poll(&pf, 1, 2000) <= 0) return -1;
            continue;
        }
        if (k <= 0) return -1;
        p += k, n -= (size_t)k;
    }
    return 0;
}

static int send_frame(int fd, int type, const void *payload, size_t n) {
    uint8_t hdr[5];
    put32(hdr, (uint32_t)n + 1), hdr[4] = (uint8_t)type;
    if (fd < 0 || send_all(fd, hdr, 5) || (n && send_all(fd, payload, n))) return -1;
    return 0;
}

/* ---- the roster and the local queue (N.lock held) ------------------------ */

static player_t *player_by_id(uint32_t id) {
    for (int i = 0; i < MAX_PLAYERS; i++) if (N.players[i].id == id) return &N.players[i];
    return NULL;
}
static player_t *player_add(uint32_t id, const char *name, int conn, int local) {
    player_t *p = player_by_id(id);
    for (int i = 0; !p && i < MAX_PLAYERS; i++) if (!N.players[i].id) p = &N.players[i];
    if (!p) return NULL;
    p->id = id, p->conn = conn, p->local = local;
    snprintf(p->name, sizeof p->name, "%s", name ? name : "");
    return p;
}
static void player_del(uint32_t id) {
    player_t *p = player_by_id(id);
    if (p) memset(p, 0, sizeof *p);
}
static int count_players(void) {
    int n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) n += N.players[i].id != 0;
    return n;
}

static void enqueue(uint32_t from, uint32_t to, const uint8_t *data, uint32_t n) {
    msg_t *m = (msg_t *)malloc(sizeof *m + n);
    if (!m) return;
    m->next = NULL, m->from = from, m->to = to, m->n = n;
    memcpy(m->data, data, n);
    if (N.q_tail) N.q_tail->next = m; else N.q_head = m;
    N.q_tail = m;
}

/* A message reached this machine: one copy for each local player it is for. */
static void deliver_local(uint32_t from, uint32_t to, const uint8_t *data, uint32_t n) {
    for (int i = 0; i < MAX_PLAYERS; i++) {
        player_t *p = &N.players[i];
        if (!p->id || !p->local || p->id == from) continue;
        if (to == DPID_ALLPLAYERS || to == p->id) enqueue(from, p->id, data, n);
    }
}

/* ---- host ------------------------------------------------------------------ */

static void host_broadcast(int except, int type, const void *payload, size_t n) {
    for (int c = 0; c < MAX_CONNS; c++)
        if (N.conns[c].fd >= 0 && c != except) send_frame(N.conns[c].fd, type, payload, n);
}

static void host_player_added(player_t *p) {
    uint8_t buf[4 + 64];
    put32(buf, p->id);
    size_t l = strlen(p->name) + 1;
    memcpy(buf + 4, p->name, l);
    host_broadcast(-1, W_PLAYER_ADD, buf, 4 + l);
}

static void host_player_removed(uint32_t id) {
    uint8_t buf[4];
    put32(buf, id);
    player_del(id);
    host_broadcast(-1, W_PLAYER_DEL, buf, 4);
}

/* A message from a player (local or a connection's): deliver and relay. */
static void host_route(int origin, uint32_t from, uint32_t to, uint32_t flags, const uint8_t *data, uint32_t n) {
    deliver_local(from, to, data, n);
    uint8_t *buf = (uint8_t *)malloc(12 + n);
    if (!buf) return;
    put32(buf, from), put32(buf + 4, to), put32(buf + 8, flags);
    memcpy(buf + 12, data, n);
    if (to == DPID_ALLPLAYERS) host_broadcast(origin, W_DATA, buf, 12 + n);
    else {
        player_t *p = player_by_id(to);
        if (p && p->conn >= 0 && p->conn != origin) send_frame(N.conns[p->conn].fd, W_DATA, buf, 12 + n);
    }
    free(buf);
}

static void host_drop(int c) {
    if (N.conns[c].fd < 0) return;
    close(N.conns[c].fd);
    N.conns[c].fd = -1, N.conns[c].in_n = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (N.players[i].id && N.players[i].conn == c) {
            fprintf(stderr, "[dplay] %s left\n", N.players[i].name);
            host_player_removed(N.players[i].id);
        }
}

static void host_frame(int c, int type, const uint8_t *p, size_t n) {
    int fd = N.conns[c].fd;
    switch (type) {
    case W_JOIN: {
        if (n < 16 || memcmp(p, N.instance, 16) || count_players() >= (int)N.max_players) {
            send_frame(fd, W_JOIN_REFUSED, NULL, 0);
            return;
        }
        uint8_t m[8];
        put32(m, N.max_players);
        put32(m + 4, (uint32_t)(g_rules < 0 ? 0 : g_rules));
        send_frame(fd, W_JOIN_OK, m, 8);
        for (int i = 0; i < MAX_PLAYERS; i++) {       /* the roster so far */
            player_t *q = &N.players[i];
            if (!q->id) continue;
            uint8_t buf[4 + 64];
            put32(buf, q->id);
            size_t l = strlen(q->name) + 1;
            memcpy(buf + 4, q->name, l);
            send_frame(fd, W_PLAYER_ADD, buf, 4 + l);
        }
        break;
    }
    case W_CREATE_PLAYER: {
        if (n < 5) return;
        uint32_t token = get32(p);
        char name[64];
        snprintf(name, sizeof name, "%.*s", (int)(n - 4), (const char *)p + 4);
        uint8_t r[8];
        put32(r, token);
        player_t *q = count_players() < (int)N.max_players ? player_add(N.next_id++, name, c, 0) : NULL;
        put32(r + 4, q ? q->id : 0);
        send_frame(fd, W_PLAYER_ID, r, 8);
        if (q) { fprintf(stderr, "[dplay] %s joined\n", q->name); host_player_added(q); }
        break;
    }
    case W_PLAYER_DEL:
        if (n >= 4) { player_t *q = player_by_id(get32(p)); if (q && q->conn == c) host_player_removed(q->id); }
        break;
    case W_DATA:
        if (n >= 12) host_route(c, get32(p), get32(p + 4), get32(p + 8), p + 12, (uint32_t)(n - 12));
        break;
    default: break;
    }
}

/* ---- client ---------------------------------------------------------------- */

static void client_frame(int type, const uint8_t *p, size_t n) {
    switch (type) {
    case W_JOIN_OK:
        N.joined = 1;
        if (n >= 4) N.max_players = get32(p);
        g_rules = n >= 8 ? (int)(get32(p + 4) & 0x7FFFFFFFu) : 0;   /* an older host: none of joyride's */
        if (g_rules & RULE_DRIVEBY) fprintf(stderr, "[dplay] the host's rules: drive-bys always\n");
        break;
    case W_JOIN_REFUSED: N.refused = 1; break;
    case W_PLAYER_ID:
        if (n >= 8 && get32(p) == N.pending_token) N.pending_id = get32(p + 4) ? get32(p + 4) : 0xFFFFFFFFu;
        break;
    case W_PLAYER_ADD:
        if (n >= 5) {
            char name[64];
            snprintf(name, sizeof name, "%.*s", (int)(n - 4), (const char *)p + 4);
            player_t *q = player_by_id(get32(p));
            player_add(get32(p), name, -1, q ? q->local : 0);
        }
        break;
    case W_PLAYER_DEL: if (n >= 4) player_del(get32(p)); break;
    case W_DATA: if (n >= 12) deliver_local(get32(p), get32(p + 4), p + 12, (uint32_t)(n - 12)); break;
    default: break;
    }
    pthread_cond_broadcast(&N.cond);
}

/* ---- the network thread ------------------------------------------------------ */

/* Read what a connection has; hand each whole frame to fn. 0: closed. */
static int pump_conn(conn_t *cn, void (*fn)(void *ctx, int type, const uint8_t *p, size_t n), void *ctx) {
    ssize_t k = recv(cn->fd, cn->in + cn->in_n, sizeof cn->in - cn->in_n, 0);
    int e = k < 0 ? sock_errno() : 0;
    if (k == 0 || (k < 0 && e != EAGAIN && e != EWOULDBLOCK && e != EINTR)) return 0;
    if (k > 0) cn->in_n += (size_t)k;
    size_t off = 0;
    while (cn->in_n - off >= 5) {
        uint32_t len = get32(cn->in + off);
        if (len == 0 || len > MAX_MSG + 64) return 0;               /* nonsense: drop it */
        if (cn->in_n - off < 4 + (size_t)len) break;
        fn(ctx, cn->in[off + 4], cn->in + off + 5, len - 1);
        off += 4 + (size_t)len;
    }
    memmove(cn->in, cn->in + off, cn->in_n - off);
    cn->in_n -= off;
    return 1;
}

static void host_cb(void *ctx, int type, const uint8_t *p, size_t n) { host_frame((int)(intptr_t)ctx, type, p, n); }
static void client_cb(void *ctx, int type, const uint8_t *p, size_t n) { (void)ctx; client_frame(type, p, n); }

static void host_udp(void) {
    uint8_t b[512];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t k = recvfrom(N.udp_fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
    if (k < 5 + 16 || memcmp(b, MAGIC, 4) || b[4] != W_ENUM_REQ) return;
    if (memcmp(b + 5, N.app, 16)) return;                           /* another game */
    uint8_t r[5 + 16 + 16 + 4 + 4 + 2 + 64];
    memcpy(r, MAGIC, 4), r[4] = W_ENUM_REPLY;
    memcpy(r + 5, N.instance, 16), memcpy(r + 21, N.app, 16);
    put32(r + 37, N.max_players), put32(r + 41, (uint32_t)count_players());
    r[45] = (uint8_t)N.port, r[46] = (uint8_t)(N.port >> 8);
    size_t l = strlen(N.session_name) + 1;
    memcpy(r + 47, N.session_name, l);
    sendto(N.udp_fd, r, 47 + l, 0, (struct sockaddr *)&from, fl);
}

static void *net_thread(void *arg) {
    (void)arg;
    while (!N.stop) {
        struct pollfd pf[2 + MAX_CONNS];
        int idx[2 + MAX_CONNS], n = 0;
        pthread_mutex_lock(&N.lock);
        if (N.is_host) {
            pf[n] = (struct pollfd){ N.listen_fd, POLLIN, 0 }, idx[n++] = -1;
            pf[n] = (struct pollfd){ N.udp_fd, POLLIN, 0 }, idx[n++] = -2;
            for (int c = 0; c < MAX_CONNS; c++) if (N.conns[c].fd >= 0) pf[n] = (struct pollfd){ N.conns[c].fd, POLLIN, 0 }, idx[n++] = c;
        } else if (N.server_fd >= 0) {
            pf[n] = (struct pollfd){ N.server_fd, POLLIN, 0 }, idx[n++] = -3;
        }
        pthread_mutex_unlock(&N.lock);
        if (!n) { usleep(10000); continue; }
        if (poll(pf, (nfds_t)n, 100) <= 0) continue;
        pthread_mutex_lock(&N.lock);
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            if (idx[i] == -1) {
                int fd = accept(N.listen_fd, NULL, NULL);
                if (fd < 0) continue;
                int c = 0;
                while (c < MAX_CONNS && N.conns[c].fd >= 0) c++;
                if (c == MAX_CONNS) { close(fd); continue; }
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                N.conns[c].fd = fd, N.conns[c].in_n = 0;
            } else if (idx[i] == -2) {
                host_udp();
            } else if (idx[i] == -3) {
                if (!pump_conn(&N.server, client_cb, NULL)) {
                    close(N.server_fd);
                    N.server_fd = N.server.fd = -1;
                    N.lost = 1;
                    fprintf(stderr, "[dplay] the host is gone: session lost\n");
                    pthread_cond_broadcast(&N.cond);
                }
            } else if (!pump_conn(&N.conns[idx[i]], host_cb, (void *)(intptr_t)idx[i])) {
                host_drop(idx[i]);
            }
        }
        pthread_mutex_unlock(&N.lock);
    }
    return NULL;
}

static void net_start(void) {
    if (N.running) return;
    N.stop = 0;
    if (pthread_create(&N.thread, NULL, net_thread, NULL) == 0) N.running = 1;
}

static uint32_t g_sent, g_received;  /* messages, for the log */

/* Everything back to the start: sockets closed, roster and queue empty. */
static void net_close(void) {
    if (g_sent || g_received) fprintf(stderr, "[dplay] session closed: %u messages sent, %u received\n", g_sent, g_received);
    g_sent = g_received = 0;
    if (N.running) { N.stop = 1; pthread_join(N.thread, NULL); N.running = 0; }
    pthread_mutex_lock(&N.lock);
    if (N.listen_fd >= 0) close(N.listen_fd);
    if (N.udp_fd >= 0) close(N.udp_fd);
    if (N.server_fd >= 0) close(N.server_fd);
    N.listen_fd = N.udp_fd = N.server_fd = N.server.fd = -1;
    for (int c = 0; c < MAX_CONNS; c++) { if (N.conns[c].fd >= 0) close(N.conns[c].fd); N.conns[c].fd = -1; N.conns[c].in_n = 0; }
    for (msg_t *m = N.q_head, *nx; m; m = nx) nx = m->next, free(m);
    N.q_head = N.q_tail = NULL;
    memset(N.players, 0, sizeof N.players);
    N.is_host = N.lost = N.joined = N.refused = 0;
    g_rules = -1;
    pthread_mutex_unlock(&N.lock);
}

/* Wait (machine given up) until cond() holds or ms pass. N.lock held. */
static int wait_for(int (*cond)(void), int ms) {
    uint64_t end = now_ms() + (uint64_t)ms;
    while (!cond()) {
        uint64_t t = now_ms();
        if (t >= end) return 0;
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) ts.tv_sec++, ts.tv_nsec -= 1000000000L;
        hle_block_begin();
        pthread_cond_timedwait(&N.cond, &N.lock, &ts);
        pthread_mutex_unlock(&N.lock);
        hle_block_end();
        pthread_mutex_lock(&N.lock);
    }
    return 1;
}

/* ---- finding games ---------------------------------------------------------- */

static void add_target(struct sockaddr_in *list, int *n, const char *host, int port) {
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_DGRAM }, *res = NULL;
    char h[256];
    snprintf(h, sizeof h, "%s", host);
    char *colon = strchr(h, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }
    if (getaddrinfo(h, NULL, &hints, &res) == 0 && res) {
        list[*n] = *(struct sockaddr_in *)res->ai_addr;
        list[*n].sin_port = htons((uint16_t)port);
        (*n)++;
        freeaddrinfo(res);
    } else {
        fprintf(stderr, "[dplay] JOYRIDE_NET_HOST %s: cannot resolve\n", host);
    }
}

/* Ask the local network (and JOYRIDE_NET_HOST) for games of this
 * application; fills N.sessions. Runs on the game's thread, machine given up. */
static void discover(const uint8_t *app, int ms) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in to[4];
    int nt = 0, port = net_port();
    to[nt] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_BROADCAST) };
    nt++;
    to[nt] = (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    nt++;
    const char *h = getenv("JOYRIDE_NET_HOST");
    if (h && *h) add_target(to, &nt, h, port);
    uint8_t req[5 + 16];
    memcpy(req, MAGIC, 4), req[4] = W_ENUM_REQ, memcpy(req + 5, app, 16);

    session_t found[MAX_SESSIONS];
    int nf = 0;
    hle_block_begin();
    uint64_t end = now_ms() + (uint64_t)ms, next_send = 0;
    while (now_ms() < end) {
        if (now_ms() >= next_send) {
            for (int i = 0; i < nt; i++) sendto(fd, req, sizeof req, 0, (struct sockaddr *)&to[i], sizeof to[i]);
            next_send = now_ms() + 250;
        }
        struct pollfd pf = { fd, POLLIN, 0 };
        if (poll(&pf, 1, 50) <= 0) continue;
        uint8_t b[512];
        struct sockaddr_in from;
        socklen_t fl = sizeof from;
        ssize_t k = recvfrom(fd, b, sizeof b - 1, 0, (struct sockaddr *)&from, &fl);
        if (k < 48 || memcmp(b, MAGIC, 4) || b[4] != W_ENUM_REPLY) continue;
        b[k] = 0;
        int dup = 0;
        for (int i = 0; i < nf; i++) dup |= !memcmp(found[i].instance, b + 5, 16);
        if (dup || nf == MAX_SESSIONS) continue;
        session_t *s = &found[nf++];
        memcpy(s->instance, b + 5, 16), memcpy(s->app, b + 21, 16);
        s->max = get32(b + 37), s->cur = get32(b + 41);
        s->addr = from;
        s->addr.sin_port = htons((uint16_t)(b[45] | b[46] << 8));
        snprintf(s->name, sizeof s->name, "%s", (const char *)b + 47);
    }
    hle_block_end();
    close(fd);
    pthread_mutex_lock(&N.lock);
    memcpy(N.sessions, found, sizeof found);
    N.nsessions = nf;
    pthread_mutex_unlock(&N.lock);
}

/* ---- the COM object ----------------------------------------------------------- */

static uint32_t g_vt;               /* IDirectPlay2A vtable */
static uint32_t g_obj;              /* the one object the game has */

static void dp_Release(void) {
    uint32_t self = A32(0), n = MEM32(self + 4) ? --MEM32(self + 4) : 0;
    if (!n) { net_close(); if (self == g_obj) g_obj = 0; hle_com_free(self); }
    RET(n, 1);
}

#define UNSUPPORTED(name, argc) static void name(void) { RET(DPERR_UNSUPPORTED, argc); }
UNSUPPORTED(dp_AddPlayerToGroup, 3)
UNSUPPORTED(dp_CreateGroup, 6)
UNSUPPORTED(dp_DeletePlayerFromGroup, 3)
UNSUPPORTED(dp_DestroyGroup, 2)
UNSUPPORTED(dp_EnumGroupPlayers, 6)
UNSUPPORTED(dp_EnumGroups, 5)
UNSUPPORTED(dp_GetCaps, 3)
UNSUPPORTED(dp_GetGroupData, 5)
UNSUPPORTED(dp_GetGroupName, 4)
UNSUPPORTED(dp_GetPlayerAddress, 4)
UNSUPPORTED(dp_GetPlayerCaps, 4)
UNSUPPORTED(dp_GetPlayerData, 5)
UNSUPPORTED(dp_Initialize, 2)
UNSUPPORTED(dp_SetGroupData, 5)
UNSUPPORTED(dp_SetGroupName, 4)
UNSUPPORTED(dp_SetPlayerData, 5)
UNSUPPORTED(dp_SetPlayerName, 4)
UNSUPPORTED(dp_SetSessionDesc, 3)

/* (this) */
static void dp_Close(void) {
    net_close();
    RET(DP_OK, 1);
}

/* (this, lpsd, dwTimeout, callback, context, flags) */
static void dp_EnumSessions(void) {
    uint32_t sd = A32(1), cb = A32(3), ctx = A32(4);
    if (!sd || !cb) RET(DPERR_INVALIDPARAMS, 6);
    uint8_t app[16];
    memcpy(app, hle_ptr(sd + SD_APPLICATION), 16);
    discover(app, 700);
    uint32_t mark = hle_scratch_mark();
    uint32_t timeout = hle_scratch(4);
    MEM32(timeout) = 700;
    pthread_mutex_lock(&N.lock);
    session_t list[MAX_SESSIONS];
    int n = N.nsessions;
    memcpy(list, N.sessions, sizeof list);
    pthread_mutex_unlock(&N.lock);
    int go = 1;
    for (int i = 0; i < n && go; i++) {
        uint32_t d = hle_scratch(SD_SIZE), name = hle_scratch_copy(list[i].name, (uint32_t)strlen(list[i].name) + 1);
        MEM32(d) = SD_SIZE;
        memcpy(hle_ptr(d + SD_INSTANCE), list[i].instance, 16);
        memcpy(hle_ptr(d + SD_APPLICATION), list[i].app, 16);
        MEM32(d + SD_MAXPLAYERS) = list[i].max;
        MEM32(d + SD_CURPLAYERS) = list[i].cur;
        MEM32(d + SD_NAME) = name;
        uint32_t a[4] = { d, timeout, 0, ctx };
        go = hle_call_guest(cb, 4, a) != 0;
    }
    if (go) {
        uint32_t a[4] = { 0, timeout, DPESC_TIMEDOUT, ctx };
        hle_call_guest(cb, 4, a);
    }
    hle_scratch_release(mark);
    RET(DP_OK, 6);
}

static int joined_or_refused(void) { return N.joined || N.refused || N.lost; }

/* (this, lpsd, flags) */
static void dp_Open(void) {
    uint32_t sd = A32(1), flags = A32(2);
    if (!sd) RET(DPERR_INVALIDPARAMS, 3);
    net_close();
    N.port = net_port();
    if (flags & DPOPEN_CREATE) {
        int lf = socket(AF_INET, SOCK_STREAM, 0), uf = socket(AF_INET, SOCK_DGRAM, 0), one = 1;
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)N.port), .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (lf >= 0) setsockopt(lf, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (uf >= 0) setsockopt(uf, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        if (lf < 0 || uf < 0 || bind(lf, (struct sockaddr *)&a, sizeof a) || listen(lf, 8)
            || bind(uf, (struct sockaddr *)&a, sizeof a)) {
            fprintf(stderr, "[dplay] cannot host on port %d: %s\n", N.port, strerror(errno));
            if (lf >= 0) close(lf);
            if (uf >= 0) close(uf);
            RET(DPERR_CANTCREATESESSION, 3);
        }
        pthread_mutex_lock(&N.lock);
        N.listen_fd = lf, N.udp_fd = uf, N.is_host = 1, N.next_id = 0x00010001u;
        g_rules = joyride_cfg.driveby ? (int)RULE_DRIVEBY : 0;    /* this host's rules, for the whole session */
        for (int i = 0; i < 16; i++) N.instance[i] = (uint8_t)(rand() ^ (int)now_ms() >> i);
        memcpy(N.app, hle_ptr(sd + SD_APPLICATION), 16);
        N.max_players = MEM32(sd + SD_MAXPLAYERS) ? MEM32(sd + SD_MAXPLAYERS) : 4;
        snprintf(N.session_name, sizeof N.session_name, "%s", MEM32(sd + SD_NAME) ? (const char *)hle_ptr(MEM32(sd + SD_NAME)) : "GTA");
        memcpy(hle_ptr(sd + SD_INSTANCE), N.instance, 16);
        pthread_mutex_unlock(&N.lock);
        net_start();
        fprintf(stderr, "[dplay] hosting \"%s\" on port %d (up to %u players)\n", N.session_name, N.port, N.max_players);
        RET(DP_OK, 3);
    }
    if (flags & DPOPEN_JOIN) {
        pthread_mutex_lock(&N.lock);
        session_t s;
        int found = 0;
        for (int i = 0; i < N.nsessions && !found; i++)
            if (!memcmp(N.sessions[i].instance, hle_ptr(sd + SD_INSTANCE), 16)) s = N.sessions[i], found = 1;
        pthread_mutex_unlock(&N.lock);
        if (!found) RET(DPERR_NOSESSIONS, 3);
        int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        hle_block_begin();
        int ok = fd >= 0 && connect(fd, (struct sockaddr *)&s.addr, sizeof s.addr) == 0;
        hle_block_end();
        if (!ok) {
            fprintf(stderr, "[dplay] cannot reach %s:%d: %s\n", inet_ntoa(s.addr.sin_addr), ntohs(s.addr.sin_port), strerror(errno));
            if (fd >= 0) close(fd);
            RET(DPERR_NOCONNECTION, 3);
        }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        pthread_mutex_lock(&N.lock);
        N.server_fd = N.server.fd = fd, N.server.in_n = 0;
        memcpy(N.instance, s.instance, 16);
        send_frame(fd, W_JOIN, s.instance, 16);
        pthread_mutex_unlock(&N.lock);
        net_start();
        pthread_mutex_lock(&N.lock);
        int done = wait_for(joined_or_refused, 5000) && N.joined;
        pthread_mutex_unlock(&N.lock);
        if (!done) { net_close(); RET(DPERR_NOSESSIONS, 3); }
        fprintf(stderr, "[dplay] joined \"%s\" at %s:%d\n", s.name, inet_ntoa(s.addr.sin_addr), ntohs(s.addr.sin_port));
        RET(DP_OK, 3);
    }
    RET(DPERR_INVALIDPARAMS, 3);
}

static int player_answered(void) { return N.pending_id != 0 || N.lost; }

/* (this, &dpid, lpPlayerName (DPNAME), hEvent, data, size, flags) */
static void dp_CreatePlayer(void) {
    uint32_t out = A32(1), dpname = A32(2);
    char name[64] = "Player";
    if (dpname && MEM32(dpname + 8)) snprintf(name, sizeof name, "%s", (const char *)hle_ptr(MEM32(dpname + 8)));
    pthread_mutex_lock(&N.lock);
    uint32_t id = 0;
    if (N.is_host) {
        player_t *p = count_players() < (int)N.max_players ? player_add(N.next_id++, name, -1, 1) : NULL;
        if (p) { id = p->id; host_player_added(p); }
    } else if (N.server_fd >= 0) {
        N.pending_token = (uint32_t)now_ms() | 1u, N.pending_id = 0;
        uint8_t buf[4 + 64];
        put32(buf, N.pending_token);
        size_t l = strlen(name) + 1;
        memcpy(buf + 4, name, l);
        send_frame(N.server_fd, W_CREATE_PLAYER, buf, 4 + l);
        if (wait_for(player_answered, 5000) && N.pending_id && N.pending_id != 0xFFFFFFFFu) {
            id = N.pending_id;
            player_add(id, name, -1, 1);
        }
    }
    pthread_mutex_unlock(&N.lock);
    if (!id) RET(DPERR_CANTCREATEPLAYER, 7);
    if (out) MEM32(out) = id;
    fprintf(stderr, "[dplay] player \"%s\" is 0x%08X\n", name, id);
    RET(DP_OK, 7);
}

/* (this, dpid) */
static void dp_DestroyPlayer(void) {
    uint32_t id = A32(1);
    pthread_mutex_lock(&N.lock);
    player_t *p = player_by_id(id);
    if (p && p->local) {
        if (N.is_host) host_player_removed(id);
        else { uint8_t b[4]; put32(b, id); send_frame(N.server_fd, W_PLAYER_DEL, b, 4); player_del(id); }
    }
    pthread_mutex_unlock(&N.lock);
    RET(p ? DP_OK : DPERR_INVALIDPLAYER, 2);
}

/* (this, lpguidInstance, callback, context, flags): callback(dpid, type, DPNAME*, flags, ctx) */
static void dp_EnumPlayers(void) {
    uint32_t cb = A32(2), ctx = A32(3);
    if (!cb) RET(DPERR_INVALIDPARAMS, 5);
    player_t list[MAX_PLAYERS];
    pthread_mutex_lock(&N.lock);
    memcpy(list, N.players, sizeof list);
    pthread_mutex_unlock(&N.lock);
    uint32_t mark = hle_scratch_mark();
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!list[i].id) continue;
        uint32_t nm = hle_scratch(16), s = hle_scratch_copy(list[i].name, (uint32_t)strlen(list[i].name) + 1);
        MEM32(nm) = 16, MEM32(nm + 4) = 0, MEM32(nm + 8) = s, MEM32(nm + 12) = s;
        uint32_t a[5] = { list[i].id, DPPLAYERTYPE_PLAYER, nm, 0, ctx };
        if (!hle_call_guest(cb, 5, a)) break;
    }
    hle_scratch_release(mark);
    RET(DP_OK, 5);
}

/* (this, idFrom, idTo, flags, data, size) */
static void dp_Send(void) {
    uint32_t from = A32(1), to = A32(2), flags = A32(3), data = A32(4), n = A32(5);
    if (n > MAX_MSG || (n && !data)) RET(DPERR_INVALIDPARAMS, 6);
    pthread_mutex_lock(&N.lock);
    uint32_t hr = DP_OK;
    if (N.lost) hr = DPERR_SESSIONLOST;
    else if (to != DPID_ALLPLAYERS && !player_by_id(to)) hr = DPERR_INVALIDPLAYER;   /* it left: how the game notices */
    else if (N.is_host) host_route(-1, from, to, flags, (const uint8_t *)hle_ptr(data), n);
    else if (N.server_fd >= 0) {
        uint8_t *buf = (uint8_t *)malloc(12 + n);
        if (buf) {
            put32(buf, from), put32(buf + 4, to), put32(buf + 8, flags);
            memcpy(buf + 12, hle_ptr(data), n);
            if (send_frame(N.server_fd, W_DATA, buf, 12 + n)) hr = DPERR_SESSIONLOST;
            free(buf);
        }
        deliver_local(from, to, (const uint8_t *)hle_ptr(data), n);   /* another local player */
    } else hr = DPERR_NOCONNECTION;
    if (hr == DP_OK) g_sent++;
    pthread_mutex_unlock(&N.lock);
    RET(hr, 6);
}

/* (this, &idFrom, &idTo, flags, data, &size) */
static void dp_Receive(void) {
    uint32_t pfrom = A32(1), pto = A32(2), data = A32(4), psize = A32(5);
    if (!psize) RET(DPERR_INVALIDPARAMS, 6);
    pthread_mutex_lock(&N.lock);
    msg_t *m = N.q_head;
    if (!m) {
        int lost = N.lost;
        pthread_mutex_unlock(&N.lock);
        RET(lost ? DPERR_SESSIONLOST : DPERR_NOMESSAGES, 6);
    }
    if (!data || MEM32(psize) < m->n) {
        MEM32(psize) = m->n;
        pthread_mutex_unlock(&N.lock);
        RET(DPERR_BUFFERTOOSMALL, 6);
    }
    N.q_head = m->next;
    if (!N.q_head) N.q_tail = NULL;
    pthread_mutex_unlock(&N.lock);
    if (pfrom) MEM32(pfrom) = m->from;
    if (pto) MEM32(pto) = m->to;
    memcpy(hle_ptr(data), m->data, m->n);
    MEM32(psize) = m->n;
    free(m);
    g_received++;
    RET(DP_OK, 6);
}

/* (this, &DPSESSIONDESC2-in-buffer, &size) */
static void dp_GetSessionDesc(void) { RET(DPERR_UNSUPPORTED, 3); }
/* (this, idPlayer, &count) */
static void dp_GetMessageCount(void) {
    uint32_t n = 0;
    pthread_mutex_lock(&N.lock);
    for (msg_t *m = N.q_head; m; m = m->next) n++;
    pthread_mutex_unlock(&N.lock);
    if (A32(2)) MEM32(A32(2)) = n;
    RET(DP_OK, 3);
}
/* (this, idPlayer, data, &size) */
static void dp_GetPlayerName(void) { RET(DPERR_UNSUPPORTED, 4); }

/* IDirectPlay2A, in vtable order */
static const win32hle_shim k_dp2[] = {
    { "IDirectPlay2A::QueryInterface", hle_com_QueryInterface },
    { "IDirectPlay2A::AddRef", hle_com_AddRef },
    { "IDirectPlay2A::Release", dp_Release },
    { "IDirectPlay2A::AddPlayerToGroup", dp_AddPlayerToGroup },
    { "IDirectPlay2A::Close", dp_Close },
    { "IDirectPlay2A::CreateGroup", dp_CreateGroup },
    { "IDirectPlay2A::CreatePlayer", dp_CreatePlayer },
    { "IDirectPlay2A::DeletePlayerFromGroup", dp_DeletePlayerFromGroup },
    { "IDirectPlay2A::DestroyGroup", dp_DestroyGroup },
    { "IDirectPlay2A::DestroyPlayer", dp_DestroyPlayer },
    { "IDirectPlay2A::EnumGroupPlayers", dp_EnumGroupPlayers },
    { "IDirectPlay2A::EnumGroups", dp_EnumGroups },
    { "IDirectPlay2A::EnumPlayers", dp_EnumPlayers },
    { "IDirectPlay2A::EnumSessions", dp_EnumSessions },
    { "IDirectPlay2A::GetCaps", dp_GetCaps },
    { "IDirectPlay2A::GetGroupData", dp_GetGroupData },
    { "IDirectPlay2A::GetGroupName", dp_GetGroupName },
    { "IDirectPlay2A::GetMessageCount", dp_GetMessageCount },
    { "IDirectPlay2A::GetPlayerAddress", dp_GetPlayerAddress },
    { "IDirectPlay2A::GetPlayerCaps", dp_GetPlayerCaps },
    { "IDirectPlay2A::GetPlayerData", dp_GetPlayerData },
    { "IDirectPlay2A::GetPlayerName", dp_GetPlayerName },
    { "IDirectPlay2A::GetSessionDesc", dp_GetSessionDesc },
    { "IDirectPlay2A::Initialize", dp_Initialize },
    { "IDirectPlay2A::Open", dp_Open },
    { "IDirectPlay2A::Receive", dp_Receive },
    { "IDirectPlay2A::Send", dp_Send },
    { "IDirectPlay2A::SetGroupData", dp_SetGroupData },
    { "IDirectPlay2A::SetGroupName", dp_SetGroupName },
    { "IDirectPlay2A::SetPlayerData", dp_SetPlayerData },
    { "IDirectPlay2A::SetPlayerName", dp_SetPlayerName },
    { "IDirectPlay2A::SetSessionDesc", dp_SetSessionDesc },
    { NULL, NULL },
};

/* ---- dplayx.dll exports ---------------------------------------------------------- */

/* #2 DirectPlayEnumerateA(callback, context): callback(&guidSP, name, major, minor, ctx) */
static void x_DirectPlayEnumerateA(void) {
    static uint32_t guid, name;
    if (!guid) {
        guid = hle_va(hle_alloc(16));
        memcpy(hle_ptr(guid), DPSPGUID_TCPIP, 16);
        name = hle_guest_strdup("Internet TCP/IP Connection (joyride)");
    }
    uint32_t a[5] = { guid, name, 6, 0, A32(1) };
    if (A32(0)) hle_call_guest(A32(0), 5, a);
    RET(DP_OK, 2);
}

/* #1 DirectPlayCreate(&guidSP, &lpDP, outer) */
static void x_DirectPlayCreate(void) {
    if (!A32(1)) RET(DPERR_INVALIDPARAMS, 3);
    if (!g_vt) g_vt = hle_com_vtable(k_dp2);
    uint32_t o = hle_com_new(g_vt, 16);
    g_obj = o;
    MEM32(A32(1)) = o;
    srand((unsigned)now_ms() ^ (unsigned)getpid());
    RET(DP_OK, 3);
}

void joyride_dplay_shutdown(void) { net_close(); }

const win32hle_shim joyride_dplay[] = {
    { "dplayx.dll#1", x_DirectPlayCreate },
    { "dplayx.dll#2", x_DirectPlayEnumerateA },
    { "DirectPlayCreate", x_DirectPlayCreate },
    { "DirectPlayEnumerateA", x_DirectPlayEnumerateA },
    { NULL, NULL },
};
