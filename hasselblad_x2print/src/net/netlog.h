/* netlog.h — Minimal logging hook for the x2print networking layer (IPP/mDNS)
 *
 * Design: library code uses the LOGD/LOGI/LOGW/LOGE macros; by default, output goes to stderr.
 * The printer-service D-Bus daemon can register a hook using x2p_netlog_set_hook()
 * and redirect logs to syslog/journal or its own logging framework.
 *
 * Note: not thread-safe (sufficient for the current single-threaded service event loop).
 */
#ifndef X2P_NETLOG_H
#define X2P_NETLOG_H

#include <stdarg.h>

typedef enum {
    X2P_LOG_DEBUG = 0, /* Protocol message details */
    X2P_LOG_INFO  = 1, /* Normal workflow milestones */
    X2P_LOG_WARN  = 2, /* Recoverable issue (e.g. mDNS multicast join failed; falling back to unicast) */
    X2P_LOG_ERROR = 3, /* Operation failed */
} x2p_log_level_t;

/* Log-hook prototype: implemented by the service layer and registered via
 * x2p_netlog_set_hook(); the caller should not output again after the hook returns. */
typedef void (*x2p_log_fn)(x2p_log_level_t lvl, const char *fmt, va_list ap);

void x2p_netlog_set_hook(x2p_log_fn fn);       /* Pass NULL to restore default stderr output */
void x2p_netlog_set_level(x2p_log_level_t lvl); /* Set minimum output level (default INFO) */
void x2p_netlog(x2p_log_level_t lvl, const char *fmt, ...);

#define LOGD(...) x2p_netlog(X2P_LOG_DEBUG, __VA_ARGS__)
#define LOGI(...) x2p_netlog(X2P_LOG_INFO,  __VA_ARGS__)
#define LOGW(...) x2p_netlog(X2P_LOG_WARN,  __VA_ARGS__)
#define LOGE(...) x2p_netlog(X2P_LOG_ERROR, __VA_ARGS__)

#endif /* X2P_NETLOG_H */
