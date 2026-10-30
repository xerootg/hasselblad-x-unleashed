/* main.c — printer-service daemon entry point
 * Flow: parse arguments → initialize logging/config → connect to bus (own
 * com.hasselblad.printer) → register adaptor + backend → event loop
 * (read_write_dispatch + tick) → clean SIGTERM/SIGINT exit (save config)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <dbus/dbus.h>

#include "log.h"
#include "state.h"
#include "config.h"
#include "adaptor.h"
#include "backend.h"

#define PRN_NAME     "com.hasselblad.printer"
#define DEFAULT_CFG  "/data/x2print/config.ini"
#define DEFAULT_LOG  "/data/x2print/log/service.log"

static volatile sig_atomic_t g_run = 1;

static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;                       /* Event loop has a 100ms timeout, so this is noticed quickly */
}

static void usage(void)
{
    fputs(
        "printer-service — X2D II printing service (com.hasselblad.printer)\n"
        "Usage: printer-service [options]\n"
        "  --config <path>   Configuration file (default " DEFAULT_CFG ")\n"
        "  --mock            Force mock backend\n"
        "  --real            Force real backend (warn and fall back to mock if real symbols are absent)\n"
        "  --bus <address>   D-Bus address (default: environment/system bus)\n"
        "  --log <file>      Log file (default " DEFAULT_LOG ")\n"
        "  --loglevel <lvl>  debug|info|warn|error (default info)\n"
        "  --daemon          Fork into background\n"
        "  --version         Show version\n"
        "  --help            Show this help\n",
        stderr);
}

static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);        /* Do not kill process if peer disconnects */
}

/* Property-change notification: state → adaptor signal; also persist writable properties */
static const char *g_cfg_path;

static void on_prop_changed(int idx, void *user)
{
    (void)user;
    prn_adaptor_emit_changed(idx);
    if (prn_prop_info_get(idx)->writable)
        prn_config_save(g_cfg_path);
}

int main(int argc, char **argv)
{
    const char *cfg_path = DEFAULT_CFG, *bus_addr = NULL, *log_path = DEFAULT_LOG;
    int level = PRN_LOG_INFO, mock = 0, daemonize = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)      cfg_path = argv[++i];
        else if (!strcmp(argv[i], "--mock"))                   mock = 1;
        else if (!strcmp(argv[i], "--real"))                   mock = 0;   /* Explicit real backend */
        else if (!strcmp(argv[i], "--bus") && i + 1 < argc)    bus_addr = argv[++i];
        else if (!strcmp(argv[i], "--log") && i + 1 < argc)    log_path = argv[++i];
        else if (!strcmp(argv[i], "--loglevel") && i + 1 < argc) {
            level = prn_log_level_parse(argv[++i]);
            if (level < 0) { fprintf(stderr, "bad --loglevel\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--daemon"))                 daemonize = 1;
        else if (!strcmp(argv[i], "--version")) { puts("printer-service 1.1 (stage2)"); return 0; }
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else { fprintf(stderr, "Unknown option: %s (try --help)\n", argv[i]); return 2; }
    }

    prn_log_init(log_path, level);
    LOGI("printer-service starting (pid=%d, mock=%d, bus=%s)", (int)getpid(), mock,
         bus_addr ? bus_addr : "<env/default>");

    if (daemonize) {
        pid_t pid = fork();
        if (pid < 0) { LOGE("fork: %s", strerror(errno)); return 1; }
        if (pid > 0) return 0;                 /* Parent exits */
        setsid();
        if (fork() != 0) _exit(0);             /* Second fork detaches from session */
        close(0);
        /* Keep stderr (logging), close stdin */
    }

    install_signals();

    /* Backend selection: --mock > weak real-backend symbol > mock fallback */
    const prn_backend *be = NULL;
    if (!mock && prn_backend_real) {
        be = prn_backend_real();
        LOGI("using real backend");
    }
    if (!be) {
        if (!mock) LOGW("real backend not present, falling back to mock (--mock to silence)");
        be = prn_backend_mock();
    }

    /* Configuration (write defaults on first launch; persist each later writable-property change) */
    g_cfg_path = cfg_path;
    if (prn_config_load(cfg_path) != 0)
        LOGW("config load failed, continue with defaults (%s)", cfg_path);

    /* ---- Connect to bus ---- */
    DBusError e;
    dbus_error_init(&e);
    dbus_connection_set_change_sigpipe(FALSE);
    DBusConnection *conn;
    int is_private = 0;
    if (bus_addr) {
        /* Explicit address (camera: unix:path=/dev/socket/dbus) → private connection + manual registration */
        conn = dbus_connection_open_private(bus_addr, &e);
        is_private = 1;
    } else {
        /* Use libdbus defaults: DBUS_SESSION_BUS_ADDRESS / DBUS_SYSTEM_BUS_ADDRESS / system bus */
        conn = dbus_bus_get(DBUS_BUS_SYSTEM, &e);
    }
    if (!conn) {
        LOGE("bus connect failed: %s", e.message ? e.message : "?");
        return 1;
    }
    if (is_private && !dbus_bus_register(conn, &e)) {
        LOGE("bus register failed: %s", e.message ? e.message : "?");
        return 1;
    }

    /* Own name: DO_NOT_QUEUE — exit immediately on failure (do not queue for takeover) */
    int ret = dbus_bus_request_name(conn, PRN_NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, &e);
    if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        LOGE("request_name %s failed ret=%d: %s", PRN_NAME, ret,
             e.message ? e.message : "?");
        return 1;
    }
    LOGI("bus name %s acquired", PRN_NAME);

    /* Hook property notifications + register adaptor */
    prn_state_set_notify(on_prop_changed, NULL);
    if (prn_adaptor_start(conn, be) != 0)
        return 1;

    /* One-time backend startup hook (§19 crash recovery: clear stale wakelock/restore screen timeout); none for mock */
    if (be->boot_init) be->boot_init();

    /* ---- Event loop ---- */
    LOGI("service ready (backend=%s)", be->name);
    while (g_run) {
        /* 100ms timeout: balance SIGTERM responsiveness and tick granularity */
        if (!dbus_connection_read_write_dispatch(conn, 100)) {
            LOGE("bus disconnected");
            break;
        }
        if (be->tick) be->tick();
    }

    LOGI("shutting down, saving config");
    prn_config_save(cfg_path);
    dbus_connection_flush(conn);
    if (is_private) dbus_connection_close(conn);
    dbus_connection_unref(conn);
    prn_log_close();
    return 0;
}
