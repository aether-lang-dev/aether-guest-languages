// worker_support.c: one pointer the worker's @c_callback functions share
// (the std.guest.child Host), since a callback takes only the host function's call.
static void* g_kept = 0;
void ghl_keep(void* p) { g_kept = p; }
void* ghl_kept(void) { return g_kept; }
