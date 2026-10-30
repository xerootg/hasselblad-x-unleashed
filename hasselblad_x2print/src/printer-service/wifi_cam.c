/* Implementation details. */
#include "wifi_cam.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#if defined(__ANDROID__)

#define CAM_SH      "/system/bin/sh"
#define X2P_ROOT    "/data/x2print"
#define BIN_DIR     X2P_ROOT "/bin"
#define WPA_SUP     BIN_DIR "/wpa_supplicant"     /* Implementation details. */
#define WPA_CLI     BIN_DIR "/wpa_cli"
#define WPA_CONF    X2P_ROOT "/wpa.conf"
#define WPA_SCAN_CONF X2P_ROOT "/wpa_scan.conf"  /* Implementation details. */
#define WPA_CTRL    X2P_ROOT "/wpasock"           /* Implementation details. */
#define WPA_PID     X2P_ROOT "/wpa_supplicant.pid"
#define WPA_LOG     X2P_ROOT "/log/wpa.log"
#define PSK_FILE    X2P_ROOT "/psk"               /* Implementation details. */
#define MOD_OWNED   X2P_ROOT "/module.owned"      /* Implementation details. */
#define STA_IF      "wlan0"
#define KMOD_PATH   "/system/lib/modules/bcmdhd.ko"
#define UDHCPC_SH   BIN_DIR "/udhcpc.sh"          /* Implementation details. */
#define OUT_TMP     X2P_ROOT "/tmp/cmd_out.txt"
#define WAKE_LOCK   "/sys/power/wake_lock"        /* Implementation details. */
#define WAKE_UNLOCK "/sys/power/wake_unlock"
#define X2P_WAKE    "x2print-session"

/* Implementation details. */
#define T_INSMOD    15000
#define T_IFUP      5000
#define T_WPA_KILL  3000
#define T_WPA_START 8000
#define T_WPA_POLL  4000     /* Implementation details. */
#define T_ASSOC_MS  45000    /* Implementation details. */
#define T_DHCP      25000
#define T_TERM      4000
#define T_FLUSH     4000
#define T_DOWN      4000
#define T_RMMOD     8000
#define T_SCAN      12000    /* Implementation details. */

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Implementation details. */
static int run_cmd(const char *cmd, int timeout_ms)
{
    long long t0 = now_ms();
    LOGD("wifi_cam: run(%dms): %s", timeout_ms, cmd);
    pid_t pid = fork();
    if (pid < 0) {
        LOGE("wifi_cam: fork: %s", strerror(errno));
        return -1;
    }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) { dup2(dn, STDOUT_FILENO); dup2(dn, STDERR_FILENO); }
        execl(CAM_SH, "sh", "-c", cmd, (char *)NULL);
        _exit(127);                                   /* Implementation details. */
    }
    int st = 0;
    for (;;) {
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) { LOGE("waitpid: %s", strerror(errno)); return -1; }
        if (now_ms() - t0 > timeout_ms) {
            LOGW("Operation timed out %d %s", timeout_ms, cmd);
            kill(pid, SIGKILL);
            waitpid(pid, &st, 0);
            return -2;
        }
        usleep(50 * 1000);
    }
    int rc = WIFEXITED(st) ? WEXITSTATUS(st) : -3;
    LOGD("wifi_cam: done rc=%d (%lldms): %s", rc, now_ms() - t0, cmd);
    return rc;
}

/* Implementation details. */
static int run_cmd_out(const char *cmd, int timeout_ms, char *out, size_t cap)
{
    remove(OUT_TMP);
    if (out && cap) out[0] = '\0';
    char full[600];
    snprintf(full, sizeof full, "%s > %s 2>/dev/null", cmd, OUT_TMP);
    int rc = run_cmd(full, timeout_ms);
    if (out && cap) {
        FILE *f = fopen(OUT_TMP, "r");
        if (f) {
            size_t n = fread(out, 1, cap - 1, f);
            out[n] = '\0';
            fclose(f);
        }
    }
    remove(OUT_TMP);
    return rc;
}

/* Implementation details. */
#define AC_RUNNING 1
#define AC_DONE    0

static struct {
    pid_t     pid;            /* Implementation details. */
    long long t0;
    int       timeout_ms;
    int       capture;
    int       rc;             /* Implementation details. */
    char      cmd[320];       /* Implementation details. */
} g_ac;

static int ac_busy(void) { return g_ac.pid > 0; }

static int ac_spawn(const char *cmd, int timeout_ms, int capture)
{
    if (ac_busy()) {
        LOGW("Operation failed %s", g_ac.cmd);
        return -1;
    }
    if (capture) remove(OUT_TMP);
    pid_t pid = fork();
    if (pid < 0) { LOGE("wifi_cam: fork: %s", strerror(errno)); return -1; }
    if (pid == 0) {
        int dn = open("/dev/null", O_WRONLY);
        if (dn < 0) _exit(126);
        dup2(dn, STDERR_FILENO);
        if (capture) {
            int out = open(OUT_TMP, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (out < 0) _exit(126);
            dup2(out, STDOUT_FILENO);
        } else {
            dup2(dn, STDOUT_FILENO);
        }
        execl(CAM_SH, "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    g_ac.pid = pid;
    g_ac.t0 = now_ms();
    g_ac.timeout_ms = timeout_ms;
    g_ac.capture = capture;
    g_ac.rc = 0;
    snprintf(g_ac.cmd, sizeof g_ac.cmd, "%.310s", cmd);
    LOGD("wifi_cam: spawn(%dms): %s", timeout_ms, cmd);
    return 0;
}

/* Implementation details. */
static int ac_poll(void)
{
    if (!ac_busy()) return AC_DONE;
    int st = 0;
    pid_t r = waitpid(g_ac.pid, &st, WNOHANG);
    if (r == g_ac.pid) {
        g_ac.rc = WIFEXITED(st) ? WEXITSTATUS(st) : -3;
        LOGD("wifi_cam: reap rc=%d (%lldms): %s", g_ac.rc, now_ms() - g_ac.t0, g_ac.cmd);
        g_ac.pid = 0;
        return AC_DONE;
    }
    if (r < 0 && errno != EINTR) {
        LOGE("wifi_cam: waitpid: %s", strerror(errno));
        g_ac.pid = 0;
        g_ac.rc = -4;
        return AC_DONE;
    }
    if (now_ms() - g_ac.t0 > g_ac.timeout_ms) {
        LOGW("Operation timed out %d %s", g_ac.timeout_ms, g_ac.cmd);
        kill(g_ac.pid, SIGKILL);
        waitpid(g_ac.pid, &st, 0);
        g_ac.pid = 0;
        g_ac.rc = -2;
        return AC_DONE;
    }
    return AC_RUNNING;
}

/* Implementation details. */
static void ac_kill(void)
{
    if (!ac_busy()) return;
    LOGW("Operation failed %s", g_ac.cmd);
    kill(g_ac.pid, SIGKILL);
    int st;
    waitpid(g_ac.pid, &st, 0);
    g_ac.pid = 0;
    g_ac.rc = -2;
}

/* Implementation details. */
static void ac_out(char *buf, size_t cap)
{
    buf[0] = 0;
    FILE *f = fopen(OUT_TMP, "r");
    if (!f) return;
    size_t n = fread(buf, 1, cap - 1, f);
    buf[n] = 0;
    fclose(f);
    remove(OUT_TMP);
}

static int exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static int iface_present(void)
{
    return exists("/sys/class/net/" STA_IF);
}

/* Implementation details. */
static int line_get(const char *out, const char *key, char *val, size_t cap)
{
    if (!out) return 0;
    size_t kl = strlen(key);
    const char *p = out;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t n = 0;
            while (v[n] && v[n] != '\n' && n < cap - 1) n++;
            memcpy(val, v, n);
            val[n] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p) p++;
    }
    return 0;
}

static void mkdirs(void)
{
    mkdir(X2P_ROOT, 0755);
    mkdir(X2P_ROOT "/bin", 0755);
    mkdir(X2P_ROOT "/log", 0755);
    mkdir(X2P_ROOT "/tmp", 0755);
}

/* Implementation details. */
static void ensure_udhcpc_script(void)
{
    if (exists(UDHCPC_SH)) return;
    FILE *f = fopen(UDHCPC_SH, "w");
    if (!f) { LOGW("Operation failed %s %s", UDHCPC_SH, strerror(errno)); return; }
    fprintf(f,
            "#!/system/bin/sh\n"
            "Operation failed"
            "case \"$1\" in\n"
            "  deconfig) busybox ifconfig \"$interface\" 0.0.0.0 ;;\n"
            "  bound|renew) busybox ifconfig \"$interface\" \"$ip\" netmask \"$subnet\";\n"
            "               busybox route add default gw \"$router\" 2>/dev/null ;;\n"
            "esac\n");
    fclose(f);
    chmod(UDHCPC_SH, 0755);
    LOGI("Operation failed %s", UDHCPC_SH);
}

/* Implementation details. */
void wifi_cam_wake_hold(void)
{
    FILE *f = fopen(WAKE_LOCK, "w");
    if (!f) {
        LOGW("Operation failed %s", strerror(errno));
        return;
    }
    fputs(X2P_WAKE, f);
    fclose(f);
    LOGI("Operation failed %s", X2P_WAKE);
}

void wifi_cam_wake_release(void)
{
    FILE *f = fopen(WAKE_UNLOCK, "w");
    if (!f)
        return;                          /* Implementation details. */
    fputs(X2P_WAKE, f);
    fclose(f);
    LOGI("Operation failed %s", X2P_WAKE);
}

/* Implementation details. */
static int g_mod_owned = 0;
static int g_join_fail_fast;   /* Implementation details. */      /* Implementation details. */

static void mod_owned_write(void)
{
    FILE *f = fopen(MOD_OWNED, "w");
    if (!f) { LOGW("Operation failed %s %s", MOD_OWNED,
                   strerror(errno)); return; }
    fputs("bcmdhd\n", f);
    fclose(f);
}

/* Implementation details. */
static int g_abort = 0;

/* Implementation details. */
enum { C_OFF = 0, C_BEGIN, C_INSMOD, C_GAP, C_IFWAIT, C_IFUP, C_IFUP_BB };
static struct {
    int       sub;
    int       armed;      /* Implementation details. */
    int       tries;
    long long t_sub;
} g_chip;

static void chip_begin(void)
{
    g_chip.sub = C_BEGIN;
    g_chip.armed = 0;
    g_chip.tries = 0;
}

static int chip_step(void)
{
    char cmd[160];
    switch (g_chip.sub) {
    case C_BEGIN:
        if (iface_present()) {
            LOGI("wifi_cam: " STA_IF "Operation failed");
            g_chip.sub = C_IFUP;
            g_chip.armed = 0;
            return chip_step();
        }
        if (!exists(KMOD_PATH)) {
            LOGE("Does not exist %s", KMOD_PATH);
            return -1;
        }
        LOGI("wifi_cam: " STA_IF "Operation failed %s", KMOD_PATH);
        g_chip.sub = C_INSMOD;
        g_chip.armed = 0;
        return chip_step();
    case C_INSMOD:
        if (!g_chip.armed) {
            snprintf(cmd, sizeof cmd, "insmod %s", KMOD_PATH);
            if (ac_spawn(cmd, T_INSMOD, 0) != 0) return -1;
            g_chip.armed = 1;
            return 1;
        }
        if (g_ac.rc == 0) {
            g_chip.sub = C_IFWAIT;
            g_chip.t_sub = now_ms();
            g_chip.armed = 0;
            return chip_step();
        }
        /* Implementation details. */
        if (g_chip.tries < 2) {
            g_chip.tries++;
            LOGW("Operation failed %d", g_chip.tries);
            g_chip.sub = C_GAP;
            g_chip.t_sub = now_ms();
            return 1;
        }
        LOGE("Operation failed");
        return -1;
    case C_GAP:
        if (now_ms() - g_chip.t_sub < 3000) return 1;
        g_chip.sub = C_INSMOD;
        g_chip.armed = 0;
        return chip_step();
    case C_IFWAIT:
        if (iface_present()) {                    /* Implementation details. */
            g_mod_owned = 1;
            mod_owned_write();
            LOGI("Operation failed");
            g_chip.sub = C_IFUP;
            g_chip.armed = 0;
            return chip_step();
        }
        if (now_ms() - g_chip.t_sub > 3000) {
            LOGE("Operation failed" STA_IF "Operation failed");
            return -1;
        }
        return 1;
    case C_IFUP:
    case C_IFUP_BB:
        if (!g_chip.armed) {
            snprintf(cmd, sizeof cmd, "%sifconfig " STA_IF " up",
                     g_chip.sub == C_IFUP_BB ? "busybox " : "");
            if (ac_spawn(cmd, T_IFUP, 0) != 0) return -1;
            g_chip.armed = 1;
            return 1;
        }
        if (g_ac.rc == 0) {
            g_chip.sub = C_OFF;
            return 0;                              /* Implementation details. */
        }
        if (g_chip.sub == C_IFUP) {                /* Implementation details. */
            LOGW("Operation failed");
            g_chip.sub = C_IFUP_BB;
            g_chip.armed = 0;
            return chip_step();
        }
        LOGE("wifi_cam: ifconfig " STA_IF "Operation failed");
        return -1;
    default:
        return -1;
    }
}

/* Implementation details. */

enum {
    J_IDLE = 0,   /* Implementation details. */
    J_PREP,       /* Implementation details. */
    J_SUPP,       /* Implementation details. */
    J_ASSOC,      /* Implementation details. */
    J_DHCP,       /* Implementation details. */
    J_DONE        /* joined */
};
enum { S_KILL = 0, S_START, S_TAIL };      /* Implementation details. */
enum { A_POLL = 0, A_TAIL };               /* Implementation details. */
enum { D_RUN = 0, D_IFCFG, D_IFCFG_FAIL }; /* Implementation details. */

static struct {
    int        phase, sub;
    int        armed;                 /* Implementation details. */
    long long  t_phase;               /* Implementation details. */
    long long  t_join;                /* Implementation details. */
    long long  last_poll;             /* Implementation details. */
    char       last_state[32];        /* Implementation details. */
} g_j;

static const char *phase_names[] = {
    "idle", "prep(insmod/ifup)", "start-supplicant",
    "assoc(wait COMPLETED)", "dhcp(udhcpc)", "joined",
};

const char *wifi_cam_join_phase(void)
{
    return phase_names[g_j.phase >= 0 && g_j.phase <= J_DONE ? g_j.phase : J_IDLE];
}

static void j_phase(int p)
{
    g_j.phase = p;
    g_j.sub = 0;
    g_j.armed = 0;
    g_j.t_phase = now_ms();
    LOGI("Operation failed %s", phase_names[p]);
}

static void j_sub(int s)
{
    g_j.sub = s;
    g_j.armed = 0;
}

/* Implementation details. */
static int write_wpa_conf(const char *ssid, const char *psk)
{
    FILE *f = fopen(WPA_CONF, "w");
    if (!f) {
        LOGE("Operation failed %s %s", WPA_CONF, strerror(errno));
        return -1;
    }
    if (psk[0]) {
        fprintf(f,
                "Operation failed"
                "ctrl_interface=%s\n"
                "ap_scan=1\n"
                "network={\n"
                "  ssid=\"%s\"\n"
                "  psk=\"%s\"\n"
                "  key_mgmt=WPA-PSK\n"
                "  proto=RSN\n"
                "  pairwise=CCMP\n"
                "  group=CCMP\n"
                "  priority=10\n"
                "}\n",
                WPA_CTRL, ssid, psk);
    } else {
        fprintf(f,
                "Operation failed"
                "ctrl_interface=%s\n"
                "ap_scan=1\n"
                "network={\n"
                "  ssid=\"%s\"\n"
                "  key_mgmt=NONE\n"
                "  priority=10\n"
                "}\n",
                WPA_CTRL, ssid);
    }
    fclose(f);
    chmod(WPA_CONF, 0600);

    if (!psk[0])
        return 0;                        /* Implementation details. */
    f = fopen(PSK_FILE, "w");            /* Implementation details. */
    if (f) {
        fprintf(f, "%s\n", psk);
        fclose(f);
        chmod(PSK_FILE, 0600);
    } else {
        LOGW("Operation failed %s %s", PSK_FILE, strerror(errno));
    }
    return 0;
}

int wifi_cam_join_start(const char *ssid, const char *psk)
{
    /* Implementation details. */
    if (!ssid || !ssid[0] || !psk || (psk[0] && strlen(psk) < 8)) {
        LOGE("Invalid value");
        return -1;
    }
    if (g_j.phase != J_IDLE && g_j.phase != J_DONE) {
        LOGW("Operation failed %s", wifi_cam_join_phase());
        return -1;
    }
    if (!exists(WPA_SUP) || !exists(WPA_CLI)) {
        LOGE("Operation failed %s %s", WPA_SUP, WPA_CLI);
        return -1;
    }
    if (strchr(ssid, '"') || strchr(psk, '"') || strchr(ssid, '\n') || strchr(psk, '\n')) {
        LOGE("Operation failed");
        return -1;
    }
    mkdirs();
    if (write_wpa_conf(ssid, psk) != 0)     /* Implementation details. */
        return -1;

    wifi_cam_wake_hold();                   /* Implementation details. */

    g_abort = 0;
    g_j.last_state[0] = '\0';
    g_j.t_join = now_ms();
    chip_begin();
    j_phase(J_PREP);
    LOGI("Operation failed %s %s", ssid, psk[0] ? "Operation failed" : "Operation failed");
    return 0;
}

static int step_supp(void)
{
    char cmd[320];
    switch (g_j.sub) {
    case S_KILL:
        /* Implementation details. */
        if (!g_j.armed) {
            snprintf(cmd, sizeof cmd,
                     "killall wpa_supplicant 2>/dev/null; rm -rf %s %s", WPA_CTRL, WPA_PID);
            if (ac_spawn(cmd, T_WPA_KILL, 0) != 0) return -1;
            g_j.armed = 1;
            return 1;
        }
        ensure_udhcpc_script();
        /* Implementation details. */
        remove(WPA_LOG);                     /* Implementation details. */
        snprintf(cmd, sizeof cmd, "%s -B -Dnl80211 -i " STA_IF " -c %s -P %s -f %s",
                 WPA_SUP, WPA_CONF, WPA_PID, WPA_LOG);
        if (ac_spawn(cmd, T_WPA_START, 0) != 0) return -1;
        g_j.sub = S_START;
        g_j.armed = 1;
        return 1;
    case S_START:
        if (g_ac.rc == 0) {
            g_j.last_poll = 0;
            j_phase(J_ASSOC);
            return 1;
        }
        LOGE("Operation failed %s", WPA_LOG);
        snprintf(cmd, sizeof cmd, "tail -n 15 %s", WPA_LOG);   /* Implementation details. */
        if (ac_spawn(cmd, 3000, 1) != 0) return -1;
        j_sub(S_TAIL);
        return 1;
    case S_TAIL:
        if (g_j.armed) {                       /* Implementation details. */
            char out[4096];
            ac_out(out, sizeof out);
            LOGW("Operation failed %.3800s", out);
        }
        return -1;
    default:
        return -1;
    }
}

static int step_assoc(void)
{
    long long now = now_ms();
    char cmd[512];                             /* Implementation details. */

    if (g_j.sub == A_TAIL) {                    /* Implementation details. */
        if (ac_busy()) return 1;
        char out[4096];
        ac_out(out, sizeof out);
        LOGW("Operation failed %.3800s", out);
        return -1;
    }
    if (now - g_j.t_phase > T_ASSOC_MS) {
        LOGE("Operation timed out %d %s", T_ASSOC_MS,
             g_j.last_state[0] ? g_j.last_state : "?");
        snprintf(cmd, sizeof cmd, "tail -n 15 %s", WPA_LOG);
        ac_spawn(cmd, 3000, 1);                /* Implementation details. */
        g_j.sub = A_TAIL;
        g_j.armed = 1;
        return 1;
    }
    if (g_j.last_poll && now - g_j.last_poll < 1000)
        return 1;                              /* Implementation details. */
    if (!g_j.armed) {
        /* Implementation details. */
        snprintf(cmd, sizeof cmd,
                 "%s -p %s status; echo __EV__; "
                 "grep -E \"reason=(15|2 |WRONG_KEY) |WRONG_KEY\" %s 2>/dev/null | tail -3",
                 WPA_CLI, WPA_CTRL, WPA_LOG);
        if (ac_spawn(cmd, T_WPA_POLL, 1) != 0) return -1;
        g_j.armed = 1;
        return 1;
    }
    /* Implementation details. */
    char out[2048], st[32] = "";
    ac_out(out, sizeof out);
    g_j.last_poll = now;
    g_j.armed = 0;
    line_get(out, "wpa_state", st, sizeof st);
    if (st[0] && strcmp(st, g_j.last_state) != 0) {
        LOGI("wifi_cam: wpa_state: %s → %s (%.1fs)",
             g_j.last_state[0] ? g_j.last_state : "(init)", st,
             (now - g_j.t_phase) / 1000.0);
        snprintf(g_j.last_state, sizeof g_j.last_state, "%s", st);
    }
    /* Implementation details. */
    const char *ev = strstr(out, "__EV__");
    if (ev && (strstr(ev, "reason=15") || strstr(ev, "reason=2 ")
               || strstr(ev, "WRONG_KEY"))) {    /* Implementation details. */
        LOGE("Operation failed %.1f",
             (now - g_j.t_phase) / 1000.0);
        g_join_fail_fast = 1;
        return -1;
    }
    if (strcmp(st, "COMPLETED") == 0) {
        char ssid_got[64] = "", bssid[32] = "";
        line_get(out, "ssid", ssid_got, sizeof ssid_got);
        line_get(out, "bssid", bssid, sizeof bssid);
        LOGI("Operation completed %s %s %.1f", ssid_got, bssid,
             (now - g_j.t_phase) / 1000.0);
        /* Implementation details. */
        remove(PSK_FILE);
        remove(WPA_CONF);
        LOGI("Operation failed");
        j_phase(J_DHCP);
    }
    return 1;
}

static int step_dhcp(void)
{
    char cmd[192];
    switch (g_j.sub) {
    case D_RUN:
        if (!g_j.armed) {
            snprintf(cmd, sizeof cmd, "busybox udhcpc -i " STA_IF " -s %s -n -q -t 4 -T 3",
                     UDHCPC_SH);
            if (ac_spawn(cmd, T_DHCP, 0) != 0) return -1;
            g_j.armed = 1;
            return 1;
        }
        j_sub(g_ac.rc == 0 ? D_IFCFG : D_IFCFG_FAIL);
        return step_dhcp();
    case D_IFCFG:
    case D_IFCFG_FAIL:
        if (!g_j.armed) {
            if (ac_spawn("busybox ifconfig " STA_IF, 4000, 1) != 0) {
                /* Implementation details. */
                if (g_j.sub == D_IFCFG_FAIL) return -1;
                j_phase(J_DONE);
                break;
            }
            g_j.armed = 1;
            return 1;
        }
        {
            char out[2048];
            ac_out(out, sizeof out);
            LOGI("wifi_cam: ifconfig: %.400s", out);
        }
        if (g_j.sub == D_IFCFG_FAIL) {
            LOGE("Operation failed");
            return -1;
        }
        j_phase(J_DONE);
        break;
    default:
        return -1;
    }
    LOGI("Operation completed %.1f",
         (now_ms() - g_j.t_join) / 1000.0);
    return 0;                                     /* 0=joined */
}

int wifi_cam_join_step(void)
{
    if (g_abort || g_j.phase == J_IDLE) return -1;
    if (g_j.phase == J_DONE) return 0;
    if (ac_busy() && ac_poll() == AC_RUNNING) return 1;   /* Implementation details. */

    switch (g_j.phase) {
    case J_PREP: {
        int r = chip_step();
        if (r < 0) return -1;
        if (r == 0) { j_phase(J_SUPP); return 1; }
        return 1;
    }
    case J_SUPP:  return step_supp();
    case J_ASSOC: return step_assoc();
    case J_DHCP:  return step_dhcp();
    default:      return -1;
    }
}

/* Implementation details. */

/* Implementation details. */
#define SCAN_MAX_NETS 32        /* Implementation details. */
typedef struct {
    char ssid[33];       /* Implementation details. */
    int  rssi;           /* dBm */
    int  wpa2;           /* 1=wpa2 0=open */
} scan_net_t;

/* Implementation details. */
static void scan_push(scan_net_t *list, int *n, const char *ssid, int rssi, int wpa2)
{
    if (!ssid[0]) return;                                /* Implementation details. */
    for (int i = 0; i < *n; i++) {
        if (!strcmp(list[i].ssid, ssid)) {
            if (rssi > list[i].rssi) { list[i].rssi = rssi; list[i].wpa2 = wpa2; }
            return;                                      /* Implementation details. */
        }
    }
    if (*n >= SCAN_MAX_NETS) return;                     /* Implementation details. */
    snprintf(list[*n].ssid, sizeof list[*n].ssid, "%s", ssid);
    list[*n].rssi = rssi;
    list[*n].wpa2 = wpa2;
    (*n)++;
}

static void scan_sort(scan_net_t *list, int n)
{
    for (int i = 1; i < n; i++) {                        /* Implementation details. */
        scan_net_t k = list[i];
        int j = i - 1;
        while (j >= 0 && list[j].rssi < k.rssi) { list[j + 1] = list[j]; j--; }
        list[j + 1] = k;
    }
}

/* Implementation details. */
static const char *trim_line(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Implementation details. */
static int parse_iw_scan(const char *out, scan_net_t *list, int *n)
{
    const char *p = out;
    char ssid[33] = "";
    int rssi = 0, wpa2 = 0, in_bss = 0, got = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[256];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        const char *t = trim_line(line);

        if (!strncmp(t, "BSS ", 4)) {
            if (in_bss) { scan_push(list, n, ssid, rssi, wpa2); got++; }
            in_bss = 1; ssid[0] = 0; rssi = -200; wpa2 = 0;
        } else if (in_bss && !strncmp(t, "SSID:", 5)) {
            snprintf(ssid, sizeof ssid, "%.32s", trim_line(t + 5));
        } else if (in_bss && !strncmp(t, "signal:", 7)) {
            rssi = (int)atof(t + 7);
        } else if (in_bss && !strncmp(t, "capability:", 11)) {
            if (strstr(t, "Privacy")) wpa2 = 1;           /* Implementation details. */
        } else if (in_bss && (!strncmp(t, "RSN:", 4) || !strncmp(t, "WPA:", 4))) {
            wpa2 = 1;                                    /* Implementation details. */
        }
        p = nl ? nl + 1 : NULL;
    }
    if (in_bss) { scan_push(list, n, ssid, rssi, wpa2); got++; }
    return got;
}

/* Implementation details. */
static int parse_wpa_scan(const char *out, scan_net_t *list, int *n)
{
    const char *p = out;
    int got = 0;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[512];
        if (ll >= sizeof line) ll = sizeof line - 1;
        memcpy(line, p, ll);
        line[ll] = 0;
        p = nl ? nl + 1 : NULL;
        if (!strncmp(line, "bssid", 5) || line[0] == 0) continue;  /* Implementation details. */

        char *f1 = strchr(line, '\t');                   /* bssid|freq */
        if (!f1) continue;
        char *f2 = strchr(f1 + 1, '\t');                 /* freq|signal */
        if (!f2) continue;
        char *f3 = strchr(f2 + 1, '\t');                 /* signal|flags */
        if (!f3) continue;
        char *f4 = strchr(f3 + 1, '\t');                 /* Implementation details. */
        if (!f4) continue;
        *f4 = 0;
        const char *ssid = f4 + 1;
        int rssi = atoi(f2 + 1);
        int wpa2 = (strstr(f3 + 1, "WPA2") || strstr(f3 + 1, "RSN") ||
                    strstr(f3 + 1, "WPA-PSK") || strstr(f3 + 1, "WPA-EAP")) ? 1 : 0;
        scan_push(list, n, ssid, rssi, wpa2);
        got++;
    }
    return got;
}

/* Implementation details. */
enum { SC_IDLE = 0, SC_CHIP, SC_IW, SC_WPA_KILL, SC_WPA_START, SC_WPA_SCAN,
       SC_WPA_POLL, SC_TAIL, SC_BUILD };
static struct {
    int        phase;
    int        armed;
    int        tries;                 /* Implementation details. */
    long long  t_poll;
    scan_net_t list[SCAN_MAX_NETS];
    int        cnt, via_iw;
    int        fail;
} g_sc;

int wifi_cam_scan_begin(void)
{
    if (g_j.phase != J_IDLE && g_j.phase != J_DONE) {
        LOGW("Operation failed %s", wifi_cam_join_phase());
        return -1;
    }
    if (g_sc.phase != SC_IDLE) {
        LOGW("Operation failed");
        return -1;
    }
    mkdirs();
    g_abort = 0;
    memset(g_sc.list, 0, sizeof g_sc.list);
    g_sc.cnt = 0;
    g_sc.via_iw = 0;
    g_sc.fail = 0;
    g_sc.armed = 0;
    chip_begin();
    g_sc.phase = SC_CHIP;
    LOGI("Operation failed");
    return 0;
}

static void scan_finish_list(char ***out, int *n)
{
    scan_sort(g_sc.list, g_sc.cnt);
    char **items = calloc((size_t)g_sc.cnt + 1, sizeof(char *));
    if (!items) { g_sc.phase = SC_IDLE; *out = NULL; *n = 0; g_sc.fail = 1; return; }
    for (int i = 0; i < g_sc.cnt; i++) {
        char el[64];
        snprintf(el, sizeof el, "%s\t%d\t%s", g_sc.list[i].ssid, g_sc.list[i].rssi,
                 g_sc.list[i].wpa2 ? "wpa2" : "open");
        items[i] = strdup(el);
    }
    *out = items;
    *n = g_sc.cnt;
    LOGI("Operation completed %s %d",
         g_sc.via_iw ? "iw" : "wpa_cli", g_sc.cnt);
    g_sc.phase = SC_IDLE;
}

int wifi_cam_scan_step(char ***out, int *n)
{
    char cmd[300];
    *out = NULL;
    *n = 0;
    if (g_abort || g_sc.phase == SC_IDLE) return -1;
    if (ac_busy() && ac_poll() == AC_RUNNING) return 1;

    switch (g_sc.phase) {
    case SC_CHIP: {
        int r = chip_step();
        if (r < 0) { g_sc.phase = SC_IDLE; return -1; }   /* Implementation details. */
        if (r == 0) {
            /* Implementation details. */
            g_sc.phase = SC_IW;
            g_sc.armed = 0;
        }
        return 1;
    }
    case SC_IW:
        if (!g_sc.armed) {
            if (ac_spawn("iw dev " STA_IF " scan", T_SCAN, 1) != 0) {
                g_sc.phase = SC_IDLE;
                return -1;
            }
            g_sc.armed = 1;
            return 1;
        }
        {
            static char buf[32768];
            ac_out(buf, sizeof buf);
            g_sc.armed = 0;
            if (g_ac.rc == 0 && strstr(buf, "BSS ")) {
                parse_iw_scan(buf, g_sc.list, &g_sc.cnt);
                g_sc.via_iw = 1;
                scan_finish_list(out, n);
                return g_sc.fail ? -1 : 0;
            }
            LOGW("Operation failed %d", g_ac.rc);
        }
        /* Implementation details. */
        if (!exists(WPA_SUP) || !exists(WPA_CLI)) {
            LOGE("Operation failed %s %s", WPA_SUP, WPA_CLI);
            g_sc.phase = SC_IDLE;
            return -1;
        }
        {
            FILE *f = fopen(WPA_SCAN_CONF, "w");         /* Implementation details. */
            if (!f) {
                LOGE("Operation failed %s %s", WPA_SCAN_CONF, strerror(errno));
                g_sc.phase = SC_IDLE;
                return -1;
            }
            fprintf(f, "Operation failed"
                       "ctrl_interface=%s\nap_scan=1\n", WPA_CTRL);
            fclose(f);
            chmod(WPA_SCAN_CONF, 0600);
        }
        g_sc.phase = SC_WPA_KILL;
        g_sc.armed = 0;
        return 1;
    case SC_WPA_KILL:
        if (!g_sc.armed) {
            snprintf(cmd, sizeof cmd, "killall wpa_supplicant 2>/dev/null; rm -rf %s %s",
                     WPA_CTRL, WPA_PID);
            if (ac_spawn(cmd, T_WPA_KILL, 0) != 0) { g_sc.phase = SC_IDLE; return -1; }
            g_sc.armed = 1;
            return 1;
        }
        snprintf(cmd, sizeof cmd, "%s -B -Dnl80211 -i " STA_IF " -c %s -P %s -f %s",
                 WPA_SUP, WPA_SCAN_CONF, WPA_PID, WPA_LOG);
        if (ac_spawn(cmd, T_WPA_START, 0) != 0) { g_sc.phase = SC_IDLE; return -1; }
        g_sc.phase = SC_WPA_START;
        g_sc.armed = 1;
        return 1;
    case SC_WPA_START:
        if (g_ac.rc != 0) {
            LOGE("Operation failed %s", WPA_LOG);
            snprintf(cmd, sizeof cmd, "tail -n 15 %s", WPA_LOG);
            ac_spawn(cmd, 3000, 1);
            g_sc.phase = SC_TAIL;
            g_sc.armed = 1;
            return 1;
        }
        snprintf(cmd, sizeof cmd, "%s -p %s scan", WPA_CLI, WPA_CTRL);
        if (ac_spawn(cmd, T_WPA_POLL, 0) != 0) { g_sc.phase = SC_IDLE; return -1; }
        g_sc.phase = SC_WPA_SCAN;
        g_sc.armed = 1;
        return 1;
    case SC_WPA_SCAN:
        if (g_ac.rc != 0)
            LOGW("Operation failed");
        g_sc.phase = SC_WPA_POLL;
        g_sc.tries = 0;
        g_sc.t_poll = 0;
        g_sc.armed = 0;
        return 1;
    case SC_WPA_POLL:
        /* Implementation details. */
        if (g_sc.t_poll && now_ms() - g_sc.t_poll < 500) return 1;
        if (!g_sc.armed) {
            snprintf(cmd, sizeof cmd, "%s -p %s scan_results", WPA_CLI, WPA_CTRL);
            if (ac_spawn(cmd, T_WPA_POLL, 1) != 0) { g_sc.phase = SC_IDLE; return -1; }
            g_sc.armed = 1;
            return 1;
        }
        {
            static char buf[16384];
            ac_out(buf, sizeof buf);
            g_sc.armed = 0;
            g_sc.t_poll = now_ms();
            g_sc.tries++;
            parse_wpa_scan(buf, g_sc.list, &g_sc.cnt);
            if (g_sc.cnt > 0 || g_sc.tries >= 16) {
                /* Implementation details. */
                scan_finish_list(out, n);
                return g_sc.fail ? -1 : 0;
            }
        }
        return 1;
    case SC_TAIL:
        if (!g_sc.armed) { g_sc.phase = SC_IDLE; return -1; }
        {
            char out2[4096];
            ac_out(out2, sizeof out2);
            LOGW("Operation failed %.3800s", out2);
        }
        g_sc.phase = SC_IDLE;
        return -1;
    default:
        g_sc.phase = SC_IDLE;
        return -1;
    }
}

/* Implementation details. */
void wifi_cam_psk_purge(void)
{
    if (remove(PSK_FILE) == 0) LOGI("Operation failed");
}

/* Implementation details. */
static const struct { const char *cmd; int to_ms; int need_if; } rs_ops[] = {
    { WPA_CLI " -p " WPA_CTRL " terminate 2>/dev/null", T_TERM,    0 },
    { "killall wpa_supplicant 2>/dev/null",             T_WPA_KILL, 0 },
    { "rm -rf " WPA_CTRL " " WPA_PID,                   2000,       0 },
    { "ip addr flush dev " STA_IF " 2>/dev/null",       T_FLUSH,    1 },
    { "busybox ip addr flush dev " STA_IF " 2>/dev/null", T_FLUSH,  1 },
    { "ifconfig " STA_IF " down",                       T_DOWN,     1 },
    { "busybox ifconfig " STA_IF " down",               T_DOWN,     1 },
};
#define RS_N ((int)(sizeof rs_ops / sizeof rs_ops[0]))
#define RS_IDLE (-1)
static struct { int op; int armed; int skip_net; } g_rs;

void wifi_cam_restore_begin(void)
{
    ac_kill();                       /* Implementation details. */
    if (g_j.phase != J_IDLE && g_j.phase != J_DONE)
        LOGW("Operation failed %s", wifi_cam_join_phase());
    g_j.phase = J_IDLE;              /* Implementation details. */
    g_sc.phase = SC_IDLE;            /* Implementation details. */
    g_chip.sub = C_OFF;
    g_abort = 0;
    g_rs.op = 0;
    g_rs.armed = 0;
    g_rs.skip_net = !iface_present();
    LOGI("Operation failed %s", g_rs.skip_net ? "Operation failed" : "Operation failed");
}

int wifi_cam_restore_step(void)
{
    if (g_rs.op == RS_IDLE) return 0;               /* Implementation details. */

    if (g_rs.armed && ac_busy() && ac_poll() == AC_RUNNING) return 1;
    if (g_rs.armed) {
        g_rs.armed = 0;
        g_rs.op++;                                  /* Implementation details. */
    }

    while (g_rs.op < RS_N && rs_ops[g_rs.op].need_if && g_rs.skip_net)
        g_rs.op++;                                  /* Implementation details. */

    if (g_rs.op >= RS_N) {
        /* Implementation details. */
        remove(MOD_OWNED);                          /* Implementation details. */
        g_mod_owned = 0;
        g_rs.op = RS_IDLE;
        LOGI("Operation completed");
        return 0;
    }
    if (ac_spawn(rs_ops[g_rs.op].cmd, rs_ops[g_rs.op].to_ms, 0) != 0) {
        g_rs.op++;                                  /* Implementation details. */
        return wifi_cam_restore_step();
    }
    g_rs.armed = 1;
    return 1;
}

/* Implementation details. */
void wifi_cam_abort(void)
{
    int active = (g_j.phase != J_IDLE && g_j.phase != J_DONE) ||
                 g_sc.phase != SC_IDLE || ac_busy();
    ac_kill();
    g_j.phase = J_IDLE;
    g_sc.phase = SC_IDLE;
    g_chip.sub = C_OFF;
    if (active) LOGW("Operation failed");
    g_abort = 1;                    /* Implementation details. */
}

/* Implementation details. */
int wifi_cam_boot_cleanup(void)
{
    int stale_supp = 0, owned = exists(MOD_OWNED);

    /* Implementation details. */
    FILE *f = fopen(WPA_PID, "r");
    if (f) {
        int pid = 0;
        int got = fscanf(f, "%d", &pid);
        fclose(f);
        remove(WPA_PID);
        if (got == 1 && pid > 1) {
            char fn[64], cl[160];
            snprintf(fn, sizeof fn, "/proc/%d/cmdline", pid);
            cl[0] = 0;
            FILE *cf = fopen(fn, "r");
            if (cf) {
                size_t n = fread(cl, 1, sizeof cl - 1, cf);
                cl[n] = 0;
                for (size_t i = 0; i < n; i++) if (cl[i] == 0) cl[i] = ' ';  /* Implementation details. */
                fclose(cf);
            }
            if (strstr(cl, WPA_SUP)) {              /* Implementation details. */
                kill(pid, SIGTERM);
                for (int i = 0; i < 10 && kill(pid, 0) == 0; i++)
                    usleep(100 * 1000);             /* Implementation details. */
                if (kill(pid, 0) == 0) {
                    kill(pid, SIGKILL);
                    LOGW("Operation failed %d", pid);
                }
                stale_supp = 1;
                LOGI("Operation failed %d", pid);
            }
        }
    }
    /* Implementation details. */
    {
        char out[256];
        if (run_cmd_out("pgrep -f " WPA_SUP, 3000, out, sizeof out) == 0 && out[0]) {
            int killed = 0;
            for (char *p = out; p && *p; ) {
                int pid = (int)strtol(p, &p, 10);
                if (pid > 1) { kill(pid, SIGKILL); killed++; }
                while (*p == ' ' || *p == '\n') p++;
            }
            if (killed) {
                stale_supp = 1;
                LOGI("Operation failed %d", killed);
            }
        }
    }
    run_cmd("rm -rf " WPA_CTRL " " WPA_PID, 2000);
    wifi_cam_psk_purge();                            /* Implementation details. */

    /* Implementation details. */
    if (owned) {
        if (iface_present())
            run_cmd("ifconfig " STA_IF " down 2>/dev/null", T_DOWN);
        remove(MOD_OWNED);
        LOGI("Operation failed");
    }

    /* Implementation details. */
    if (stale_supp || owned) {
        run_cmd("ip addr flush dev " STA_IF " 2>/dev/null", T_FLUSH);
        run_cmd("busybox ip addr flush dev " STA_IF " 2>/dev/null", T_FLUSH);
        run_cmd("ifconfig " STA_IF " down", T_DOWN);
        LOGI("Operation failed");
    } else {
        LOGI("Operation failed");
    }
    return 0;
}

#else /* Implementation details. */

static const char *g_offcam =
    "Operation failed";

int wifi_cam_join_start(const char *s, const char *p)
{
    (void)s; (void)p; LOGW("%s", g_offcam); return -1;
}
int wifi_cam_join_step(void)              { return -1; }
const char *wifi_cam_join_phase(void)     { return "stub"; }
int wifi_cam_scan_begin(void)             { LOGW("%s", g_offcam); return -1; }
int wifi_cam_scan_step(char ***out, int *n) { (void)out; (void)n; return -1; }
void wifi_cam_restore_begin(void)         { }
int wifi_cam_restore_step(void)           { return 0; }
void wifi_cam_abort(void)                 { }
int wifi_cam_boot_cleanup(void)           { return 0; }
void wifi_cam_wake_hold(void)             { }
void wifi_cam_wake_release(void)          { }
void wifi_cam_psk_purge(void)               { }   /* Implementation details. */

#endif /* __ANDROID__ */
