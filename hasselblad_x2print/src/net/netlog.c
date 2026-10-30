/* netlog.c — Logging implementation: stderr by default; can be taken over by a service-layer hook */
#include "netlog.h"

#include <stdio.h>
#include <time.h>

static x2p_log_fn g_hook = NULL;
static x2p_log_level_t g_level = X2P_LOG_INFO;

static const char *lvl_tag[] = {"[D]", "[I]", "[W]", "[E]"};

void x2p_netlog_set_hook(x2p_log_fn fn) { g_hook = fn; }

void x2p_netlog_set_level(x2p_log_level_t lvl) { g_level = lvl; }

void x2p_netlog(x2p_log_level_t lvl, const char *fmt, ...)
{
    va_list ap;
    if (lvl < g_level) return;
    if (g_hook) { /* Hand all output to the service layer when a hook is registered */
        va_start(ap, fmt);
        g_hook(lvl, fmt, ap);
        va_end(ap);
        return;
    }
    /* Default: [level] message → stderr */
    va_start(ap, fmt);
    fprintf(stderr, "%s ", lvl_tag[lvl & 3]);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    (void)time(NULL); /* Reserved for adding timestamps if needed */
}
