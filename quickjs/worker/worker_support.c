// worker_support.c: how quickjs-worker hears a CANCEL while the script runs
// JavaScript and calls nothing (a loop).
//
// The other workers poll the socket from a hook the engine calls every so
// many steps (Lua's debug hook, Python's trace function, Tcl's limit
// handler). QuickJS calls no host code in a loop: contrib.quickjs offers a
// cancel cell instead, a std.sync cell its interrupt handler reads, which
// any thread may set. So a watcher thread waits on the socket while the
// script runs, and when the host has sent something the script did not ask
// for, sets the cell; the engine then stops the script ("interrupted:
// cancelled", which the script cannot catch), and the worker reads the frame
// (std.guest.child's poll) and reports the host's reason.
//
// The host speaks unasked only to cancel, or to set the log level (LOGLEVEL,
// which this worker has no use for: it stays on the socket for call_host or
// poll to apply). Anything else unasked breaks the protocol, which cancels
// too, and so does the host hanging up. While the script waits on a host call
// the reply is not the watcher's to judge: the worker marks the call
// (ghl_watch_call) under the same lock the watcher looks under, before the
// CALL goes out and after the reply is read, so a frame the watcher sees
// outside a call was sent before the call, unasked. The watcher only peeks
// (MSG_PEEK): every frame is still read by std.guest.child, on the worker's
// own thread.
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

extern void aether_sync_atomic_store(void* cell, int64_t value);

typedef struct {
    int fd;
    void* cell;
    pthread_mutex_t mu;
    int in_call;
    int stop;
    pthread_t th;
} GhlWatch;

static void nap_ms(int ms) {
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

// 1 when what waits on the socket is a frame other than LOGLEVEL (or the
// host has hung up); 0 when there is nothing, only LOGLEVEL frames, or not
// yet enough of a frame to tell.
static int unasked(int fd) {
    unsigned char buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, MSG_PEEK | MSG_DONTWAIT);
    if (n == 0) return 1;                 // the host hung up
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) ? 0 : 1;
    size_t off = 0;
    while (off + 4 <= (size_t)n) {
        uint32_t len = ((uint32_t)buf[off] << 24) | ((uint32_t)buf[off + 1] << 16) |
                       ((uint32_t)buf[off + 2] << 8) | (uint32_t)buf[off + 3];
        size_t have = (size_t)n - off - 4;
        const unsigned char* p = buf + off + 4;
        size_t need = len < 9 ? len : 9;
        if (have < need) return 0;
        int loglevel = len >= 8 && memcmp(p, "LOGLEVEL", 8) == 0 && (len == 8 || p[8] == ' ');
        if (!loglevel) return 1;
        if (have < len) return 0;
        off += 4 + len;
    }
    return 0;
}

static void* watch(void* arg) {
    GhlWatch* w = (GhlWatch*)arg;
    for (;;) {
        pthread_mutex_lock(&w->mu);
        int stop = w->stop;
        pthread_mutex_unlock(&w->mu);
        if (stop) return NULL;
        struct pollfd p = { w->fd, POLLIN, 0 };
        int r = poll(&p, 1, 20);
        if (r <= 0) continue;
        int fired = 0;
        pthread_mutex_lock(&w->mu);
        if (!w->stop && !w->in_call && unasked(w->fd)) {
            aether_sync_atomic_store(w->cell, 1);
            fired = 1;
        }
        pthread_mutex_unlock(&w->mu);
        if (fired) return NULL;
        nap_ms(5);   // a frame that is the call's, or a LOGLEVEL, is still there
    }
}

// Starts watching socket fd, setting cell when the host speaks unasked. NULL
// when the thread cannot be started (the worker then hears a CANCEL only in a
// call's reply).
void* ghl_watch_start(int fd, void* cell) {
    GhlWatch* w = (GhlWatch*)calloc(1, sizeof(GhlWatch));
    if (!w) return NULL;
    w->fd = fd;
    w->cell = cell;
    pthread_mutex_init(&w->mu, NULL);
    if (pthread_create(&w->th, NULL, watch, w) != 0) {
        pthread_mutex_destroy(&w->mu);
        free(w);
        return NULL;
    }
    return w;
}

// 1 before the worker sends a CALL, 0 once it has read the reply.
void ghl_watch_call(void* p, int on) {
    GhlWatch* w = (GhlWatch*)p;
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->in_call = on;
    pthread_mutex_unlock(&w->mu);
}

// Stops the watcher and waits for it, before the worker reads the socket on
// its own again.
void ghl_watch_stop(void* p) {
    GhlWatch* w = (GhlWatch*)p;
    if (!w) return;
    pthread_mutex_lock(&w->mu);
    w->stop = 1;
    pthread_mutex_unlock(&w->mu);
    pthread_join(w->th, NULL);
    pthread_mutex_destroy(&w->mu);
    free(w);
}
