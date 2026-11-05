// worker_support.c: for tinygo-worker. One pointer the worker shares with
// its helpers (the std.guest.child Host), and the thread a cancellable call
// of the Go library runs on.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef AETHER_HAS_SANDBOX
#define AETHER_HAS_SANDBOX 1
#endif
#include "aether_sandbox.h"

// The bridge's own (tinygo/aether_host_tinygo.c, compiled into this program
// by `import tinygo`): the borrowed-string call.
const char* tinygo_call_s_s_s(void* h, const char* s, const char* a, const char* b);

static void* g_kept = 0;
void ghl_keep(void* p) { g_kept = p; }
void* ghl_kept(void) { return g_kept; }

void ghl_sleep_ms(int ms) {
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

// One call of an exported `char* F(char*, char*)` on a thread of its own, so
// the worker's main thread can go on looking for the host's CANCEL while Go
// code runs. The result is copied out of the C.CString the library returned
// (which is freed here); a symbol that does not resolve gives "".
typedef struct {
    void* grants;
    void* handle;
    char* sym;
    char* a;
    char* b;
    char* result;
    volatile int done;
    pthread_t thread;
} Call;

static void* call_main(void* arg) {
    Call* c = (Call*)arg;
    // A thread's grant levels are its own: the worker's lockdown is the main
    // thread's, so this thread takes the same (empty) level before it runs
    // any Go.
    if (c->grants) aether_sandbox_push(c->grants);
    const char* r = tinygo_call_s_s_s(c->handle, c->sym, c->a, c->b);
    c->result = strdup(r ? r : "");
    // tinygo_call_s_s_s hands back the library's own pointer, or "" when the
    // symbol did not resolve (a literal, not to be freed); the library's is
    // a malloc, which the owned wrappers free. Tell the two apart by symbol.
    if (r && r[0] != '\0') free((void*)r);
    if (c->grants) aether_sandbox_pop();
    __sync_synchronize();
    c->done = 1;
    return NULL;
}

// Starts the call; NULL when the thread could not be started.
void* ghl_call_start(void* grants, void* handle, const char* sym, const char* a, const char* b) {
    Call* c = calloc(1, sizeof *c);
    c->grants = grants;
    c->handle = handle;
    c->sym = strdup(sym);
    c->a = strdup(a);
    c->b = strdup(b);
    if (pthread_create(&c->thread, NULL, call_main, c) != 0) {
        free(c->sym); free(c->a); free(c->b); free(c);
        return NULL;
    }
    pthread_detach(c->thread);
    return c;
}

int ghl_call_done(void* slot) {
    int d = ((Call*)slot)->done;
    __sync_synchronize();
    return d;
}

// The result of a finished call. Not freed: the worker ends soon after.
const char* ghl_call_result(void* slot) { return ((Call*)slot)->result; }
