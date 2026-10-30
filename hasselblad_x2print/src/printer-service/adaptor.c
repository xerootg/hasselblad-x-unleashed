/* Implementation details. */
#include "adaptor.h"
#include "state.h"
#include "log.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#define PRN_DEST        "com.hasselblad.printer"
#define PRN_PATH        "/printer"
#define PRN_IFACE       PRN_DEST
#define IFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#define IFACE_INTROSPECT "org.freedesktop.DBus.Introspectable"
#define IFACE_PEER       "org.freedesktop.DBus.Peer"
#define SIG_PROPCHANGED  "propertiesChanged"            /* Implementation details. */

#define ERR_FAILED "com.hasselblad.error.failed"
#define ERR_PARAM  "com.hasselblad.error.parametererror"
#define ERR_BUSY   "com.hasselblad.error.busy"

static DBusConnection *g_conn;
static const prn_backend *g_be;

/* Implementation details. */
static long long now_ms_a(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static long long g_last_activity;        /* Implementation details. */

void prn_adaptor_touch(void)             { g_last_activity = now_ms_a(); }
long long prn_adaptor_idle_ms(void)      { return now_ms_a() - g_last_activity; }

/* Implementation details. */
int prn_adaptor_system_wifi_power(int on)
{
    if (!g_conn) return -1;
    dbus_bool_t v = on ? TRUE : FALSE;
    DBusMessage *req = dbus_message_new_method_call(
        "com.hasselblad.system", "/system", "com.hasselblad.system", "power_on_wifi");
    if (!req) return -1;
    if (!dbus_message_append_args(req, DBUS_TYPE_BOOLEAN, &v, DBUS_TYPE_INVALID)) {
        dbus_message_unref(req);
        return -1;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 2000, &e);
    dbus_message_unref(req);
    if (!rep) {
        LOGW("Operation failed %d %s", on, e.message ? e.message : "?");
        dbus_error_free(&e);
        return -1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        LOGW("Operation failed %d %s", on,
             dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return -1;
    }
    dbus_message_unref(rep);
    LOGI("Operation failed %d", on);
    return 0;
}

/* Implementation details. */
int prn_adaptor_screen_idle_set(int sec)
{
    if (!g_conn) return -1;
    DBusMessage *req = dbus_message_new_method_call(
        "com.hasselblad.system", "/system",
        "org.freedesktop.DBus.Properties", "Set");
    if (!req) return -1;
    const char *iface = "com.hasselblad.system", *name = "screen_idle_timeout";
    dbus_int32_t v = (dbus_int32_t)sec;
    DBusMessageIter it, var;
    if (!dbus_message_append_args(req, DBUS_TYPE_STRING, &iface,
                                  DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
        dbus_message_unref(req);
        return -1;
    }
    dbus_message_iter_init_append(req, &it);
    if (!dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, "i", &var) ||
        !dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &v) ||
        !dbus_message_iter_close_container(&it, &var)) {
        dbus_message_unref(req);
        return -1;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 2000, &e);
    dbus_message_unref(req);
    if (!rep) {
        LOGW("Operation failed %d %s", sec, e.message ? e.message : "?");
        dbus_error_free(&e);
        return -1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        LOGW("Operation failed %d %s", sec,
             dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return -1;
    }
    dbus_message_unref(rep);
    return 0;
}

int prn_adaptor_screen_idle_get(int *sec)
{
    if (!g_conn || !sec) return -1;
    DBusMessage *req = dbus_message_new_method_call(
        "com.hasselblad.system", "/system",
        "org.freedesktop.DBus.Properties", "Get");
    if (!req) return -1;
    const char *iface = "com.hasselblad.system", *name = "screen_idle_timeout";
    if (!dbus_message_append_args(req, DBUS_TYPE_STRING, &iface,
                                  DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
        dbus_message_unref(req);
        return -1;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(g_conn, req, 2000, &e);
    dbus_message_unref(req);
    if (!rep) {
        LOGW("Operation failed %s", e.message ? e.message : "?");
        dbus_error_free(&e);
        return -1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        LOGW("Operation failed %s",
             dbus_message_get_error_name(rep) ? dbus_message_get_error_name(rep) : "?");
        dbus_message_unref(rep);
        return -1;
    }
    /* Implementation details. */
    DBusMessageIter it, sub;
    dbus_int32_t v = -1;
    int ok = 0;
    if (dbus_message_iter_init(rep, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
        dbus_message_iter_recurse(&it, &sub);
        if (dbus_message_iter_get_arg_type(&sub) == DBUS_TYPE_INT32) {
            dbus_message_iter_get_basic(&sub, &v);
            ok = 1;
        }
    }
    dbus_message_unref(rep);
    if (!ok) {
        LOGW("Operation failed");
        return -1;
    }
    *sec = (int)v;
    return 0;
}

/* Implementation details. */
static DBusHandlerResult reply_err(DBusConnection *c, DBusMessage *req,
                                   const char *name, const char *msg)
{
    DBusMessage *r = dbus_message_new_error(req, name, msg);
    if (!r) { LOGE("oom: error reply"); return DBUS_HANDLER_RESULT_NEED_MEMORY; }
    dbus_connection_send(c, r, NULL);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult reply_empty(DBusConnection *c, DBusMessage *req)
{
    DBusMessage *r = dbus_message_new_method_return(req);
    if (!r) return DBUS_HANDLER_RESULT_NEED_MEMORY;
    dbus_connection_send(c, r, NULL);
    dbus_message_unref(r);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */
static const char *prop_signature(int idx)
{
    switch (prn_prop_info_get(idx)->type) {
    case 's': return DBUS_TYPE_STRING_AS_STRING;
    case 'i': return DBUS_TYPE_INT32_AS_STRING;
    case 'b': return DBUS_TYPE_BOOLEAN_AS_STRING;
    default:  return DBUS_TYPE_ARRAY_AS_STRING DBUS_TYPE_STRING_AS_STRING; /* as */
    }
}

/* Implementation details. */
static const char *safe_str(const char *s)
{
    return s ? s : "";
}

static void append_prop_value(DBusMessageIter *iter, int idx)
{
    if (prn_prop_info_get(idx)->type == 's') {
        const char *v = safe_str(prn_get_string(idx));
        dbus_message_iter_append_basic(iter, DBUS_TYPE_STRING, &v);
    } else if (prn_prop_info_get(idx)->type == 'i') {
        dbus_int32_t v = prn_get_int(idx);
        dbus_message_iter_append_basic(iter, DBUS_TYPE_INT32, &v);
    } else if (prn_prop_info_get(idx)->type == 'b') {       /* synced/tx_active/caps_ready(§7b/§2b) */
        dbus_bool_t v = prn_get_int(idx) ? TRUE : FALSE;
        dbus_message_iter_append_basic(iter, DBUS_TYPE_BOOLEAN, &v);
    } else {                                             /* Implementation details. */
        DBusMessageIter arr;
        dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "s", &arr);
        if (idx == PRN_DISCOVERED_PRINTERS) {
            prn_state *st = prn_state_get();
            for (int i = 0; i < st->discovered_n; i++) {
                if (!st->discovered[i]) {
                    LOGW("marshal: discovered[%d] NULL, skip", i);
                    continue;
                }
                dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &st->discovered[i]);
            }
        } else {
            /* Implementation details. */
            const prn_strlist_t *l = prn_get_list(idx);
            if (!l) {
                LOGW("marshal: list prop #%d NULL store, emit empty", idx);
            } else {
                for (int i = 0; i < l->n; i++) {
                    const char *s = l->items[i];
                    dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &s);
                }
            }
        }
        dbus_message_iter_close_container(iter, &arr);
    }
}

/* Implementation details. */
void prn_adaptor_emit_changed(int prop_idx)
{
    if (!g_conn) return;
    const prn_prop_info *pi = prn_prop_info_get(prop_idx);
    if (!pi) return;

    DBusMessage *sig = dbus_message_new_signal(PRN_PATH, IFACE_PROPERTIES, SIG_PROPCHANGED);
    if (!sig) { LOGE("oom: signal"); return; }
    DBusMessageIter it, arr, entry, v, inv;
    const char *iface_name = PRN_IFACE;
    dbus_message_iter_init_append(sig, &it);
    dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface_name);  /* a0: s */
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);/* a1: a{sv} */
    dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &pi->name);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, prop_signature(prop_idx), &v);
    append_prop_value(&v, prop_idx);
    dbus_message_iter_close_container(&entry, &v);
    dbus_message_iter_close_container(&arr, &entry);
    dbus_message_iter_close_container(&it, &arr);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &inv);   /* Implementation details. */
    dbus_message_iter_close_container(&it, &inv);
    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
    LOGD("signal propertiesChanged: %s", pi->name);
}

/* Implementation details. */
static void emit_yay(const char *member, unsigned char len, const unsigned char *data, int n)
{
    if (!g_conn) return;
    DBusMessage *sig = dbus_message_new_signal(PRN_PATH, PRN_IFACE, member);
    if (!sig) { LOGE("oom: signal"); return; }
    const unsigned char *p = data;
    if (!dbus_message_append_args(sig, DBUS_TYPE_BYTE, &len,
                                       DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
                                       &p, n, DBUS_TYPE_INVALID)) {
        LOGE("append rx payload failed");
        dbus_message_unref(sig);
        return;
    }
    dbus_connection_send(g_conn, sig, NULL);
    dbus_message_unref(sig);
    LOGD("signal %s: len=%u n=%d", member, (unsigned)len, n);
}

void prn_adaptor_emit_rx(unsigned char len, const unsigned char *data, int n)
{
    emit_yay("rx", len, data, n);
}

void prn_adaptor_emit_testrx(unsigned char len, const unsigned char *data, int n)
{
    emit_yay("testrx", len, data, n);
}

/* ---------- org.freedesktop.DBus.Properties ---------- */
static DBusHandlerResult handle_get_all(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    char *iface_arg = NULL;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &iface_arg, DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", e.message);
        dbus_error_free(&e);
        return r;
    }
    /* Implementation details. */
    /* Implementation details. */
    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    if (!iface_arg || !*iface_arg || !strcmp(iface_arg, PRN_IFACE)) {
        for (int idx = 0; idx < PRN_PROP_COUNT; idx++) {
            DBusMessageIter entry, v;
            dbus_message_iter_open_container(&arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &prn_prop_info_get(idx)->name);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, prop_signature(idx), &v);
            append_prop_value(&v, idx);
            dbus_message_iter_close_container(&entry, &v);
            dbus_message_iter_close_container(&arr, &entry);
        }
    }
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult handle_get(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter args;
    char *iface_arg = NULL, *name = NULL;
    if (!dbus_message_iter_init(m, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Get args");
    dbus_message_iter_get_basic(&args, &iface_arg);            /* Implementation details. */
    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Get args");
    dbus_message_iter_get_basic(&args, &name);
    (void)iface_arg;                                           /* Implementation details. */

    int idx = prn_prop_index(name);
    if (idx < 0) {
        char msg[128];
        snprintf(msg, sizeof msg, "No such property '%s'", name);
        return reply_err(c, m, ERR_PARAM, msg);
    }
    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, v;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_VARIANT, prop_signature(idx), &v);
    append_prop_value(&v, idx);
    dbus_message_iter_close_container(&it, &v);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */
static DBusHandlerResult handle_set(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter args, sub;
    char *iface_arg = NULL, *name = NULL;
    if (!dbus_message_iter_init(m, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Set args");
    dbus_message_iter_get_basic(&args, &iface_arg);            /* Implementation details. */
    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Set args");
    dbus_message_iter_get_basic(&args, &name);
    dbus_message_iter_next(&args);
    if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_VARIANT)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "bad Set args");
    dbus_message_iter_recurse(&args, &sub);

    prn_adaptor_touch();                                 /* Implementation details. */
    int idx = prn_prop_index(name);
    const prn_prop_info *pi = prn_prop_info_get(idx);
    if (!pi) {
        char msg[128];
        snprintf(msg, sizeof msg, "No such property '%s'", name);
        return reply_err(c, m, ERR_PARAM, msg);
    }
    if (!pi->writable) {
        char msg[128];
        snprintf(msg, sizeof msg, "Property '%s' is read-only", pi->name);
        return reply_err(c, m, ERR_PARAM, msg);
    }
    /* Implementation details. */
    char want = pi->type;
    int got = dbus_message_iter_get_arg_type(&sub);
    if ((want == 'i' && got != DBUS_TYPE_INT32) ||
        (want == 's' && got != DBUS_TYPE_STRING))
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
                         "value type mismatch");

    DBusHandlerResult hr;
    if (want == 'i') {
        dbus_int32_t val;
        dbus_message_iter_get_basic(&sub, &val);
        int rc = prn_set_int(idx, (int)val);                  /* Implementation details. */
        if (rc == -2) {
            char msg[128];
            snprintf(msg, sizeof msg, "Value %d out of range for '%s'", (int)val, pi->name);
            hr = reply_err(c, m, ERR_PARAM, msg);
        } else if (rc != 0) {
            hr = reply_err(c, m, ERR_PARAM, "set rejected");
        } else {
            LOGI("Set %s = %d", pi->name, (int)val);
            hr = reply_empty(c, m);
        }
    } else {
        const char *val;
        dbus_message_iter_get_basic(&sub, &val);
        int rc = prn_set_string(idx, val);
        if (rc != 0)
            hr = reply_err(c, m, ERR_PARAM, "set rejected");
        else {
            LOGI("Set %s = \"%s\"", pi->name, val);
            hr = reply_empty(c, m);
        }
    }
    return hr;
}

/* Implementation details. */
static const char *be_err_name(int code)
{
    switch (code) {
    case PRN_BE_PARAM: return ERR_PARAM;
    case PRN_BE_BUSY:  return ERR_BUSY;
    default:           return ERR_FAILED;
    }
}
static DBusHandlerResult reply_be_err(DBusConnection *c, DBusMessage *m, int code, const char *what)
{
    /* Implementation details. */
    const char *detail = prn_be_last_error();
    char msg[192];
    if (detail && detail[0]) snprintf(msg, sizeof msg, "%s: %s", what, detail);
    else snprintf(msg, sizeof msg, "%s", what);
    LOGW("method %s failed: %s (%d)%s%s", what, be_err_name(code), code,
         (detail && detail[0]) ? " " : "", (detail && detail[0]) ? detail : "");
    return reply_err(c, m, be_err_name(code), msg);
}

/* Implementation details. */
static DBusHandlerResult reply_str_array(DBusConnection *c, DBusMessage *m, char **items, int n)
{
    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &arr);
    for (int i = 0; i < n; i++)
        dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &items[i]);
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}
static void free_str_array(char **items, int n)
{
    if (!items) return;
    for (int i = 0; i < n; i++) free(items[i]);
    free(items);
}

/* Connect_to_ap(s ssid, s psk) → b */
static DBusHandlerResult m_connect_to_ap(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    const char *ssid = NULL, *psk = NULL;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &ssid, DBUS_TYPE_STRING, &psk,
                               DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", e.message);
        dbus_error_free(&e);
        return r;
    }
    int rc = g_be->connect_to_ap(ssid, psk);
    if (rc < 0) return reply_be_err(c, m, rc, "connect_to_ap");
    dbus_bool_t ok = TRUE;
    DBusMessage *rep = dbus_message_new_method_return(m);
    dbus_message_append_args(rep, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */
static DBusMessage *g_pending_scan;      /* Implementation details. */

static DBusHandlerResult m_scan_wifi(DBusConnection *c, DBusMessage *m)
{
    char **items = NULL;
    int n = 0;
    int rc = g_be->scan_wifi(&items, &n);
    if (rc < 0) return reply_be_err(c, m, rc, "scan_wifi");
    if (rc == 1) {                       /* Implementation details. */
        g_pending_scan = dbus_message_ref(m);
        LOGI("Operation completed");
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    DBusHandlerResult hr = reply_str_array(c, m, items, n);
    free_str_array(items, n);
    return hr;
}

void prn_adaptor_scan_complete(int ok)
{
    if (!g_pending_scan) return;
    DBusMessage *m = g_pending_scan;
    g_pending_scan = NULL;
    if (ok) {
        prn_state *st = prn_state_get();
        char **items = calloc((size_t)st->scanned.n + 1, sizeof(char *));
        if (items) {
            int n = 0;
            for (int i = 0; i < st->scanned.n; i++)
                items[n++] = strdup(st->scanned.items[i]);
            reply_str_array(g_conn, m, items, n);
            free_str_array(items, n);
        } else {
            reply_err(g_conn, m, ERR_FAILED, "oom");
        }
        LOGI("Operation failed %d", prn_state_get()->scanned.n);
    } else {
        const char *detail = prn_be_last_error();
        char msg[192];
        if (detail && detail[0]) snprintf(msg, sizeof msg, "scan_wifi: %s", detail);
        else snprintf(msg, sizeof msg, "%s", "scan_wifi failed");
        LOGW("Operation failed %s", msg);
        reply_err(g_conn, m, ERR_FAILED, msg);
    }
    dbus_message_unref(m);
}

/* Disconnect_and_restore() → () */
static DBusHandlerResult m_disconnect(DBusConnection *c, DBusMessage *m)
{
    int rc = g_be->disconnect_restore();
    if (rc < 0) return reply_be_err(c, m, rc, "disconnect_and_restore");
    return reply_empty(c, m);
}

/* Start_discovery() → as */
static DBusHandlerResult m_start_discovery(DBusConnection *c, DBusMessage *m)
{
    char **items = NULL;
    int n = 0;
    int rc = g_be->start_discovery(&items, &n);
    if (rc < 0) return reply_be_err(c, m, rc, "start_discovery");
    DBusHandlerResult hr = reply_str_array(c, m, items, n);
    free_str_array(items, n);
    return hr;
}

/* Select_printer(s name) → b */
static DBusHandlerResult m_select_printer(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    const char *name = NULL;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", e.message);
        dbus_error_free(&e);
        return r;
    }
    int rc = g_be->select_printer(name);
    if (rc < 0) return reply_be_err(c, m, rc, "select_printer");
    dbus_bool_t ok = TRUE;
    DBusMessage *rep = dbus_message_new_method_return(m);
    dbus_message_append_args(rep, DBUS_TYPE_BOOLEAN, &ok, DBUS_TYPE_INVALID);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Get_photo_list(s path) → as */
static DBusHandlerResult m_get_photo_list(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    const char *path = NULL;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &path, DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", e.message);
        dbus_error_free(&e);
        return r;
    }
    char **items = NULL;
    int n = 0;
    int rc = g_be->photo_list(path, &items, &n);
    if (rc < 0) return reply_be_err(c, m, rc, "get_photo_list");
    DBusHandlerResult hr = reply_str_array(c, m, items, n);
    free_str_array(items, n);
    return hr;
}

/* Implementation details. */

/* Implementation details. */
static const char *edit_opts_check(const prn_job_opts_t *o)
{
    if (isnan(o->crop_x) || isnan(o->crop_y) || isnan(o->crop_w) || isnan(o->crop_h) ||
        isinf(o->crop_x) || isinf(o->crop_y) || isinf(o->crop_w) || isinf(o->crop_h))
        return "Operation failed";
    if (o->crop_x < 0.0 || o->crop_y < 0.0 || o->crop_x > 1.0 || o->crop_y > 1.0 ||
        o->crop_w <= 0.0 || o->crop_h <= 0.0 ||
        o->crop_x + o->crop_w > 1.0 + 1e-6 ||
        o->crop_y + o->crop_h > 1.0 + 1e-6)
        return "Operation failed";
    if (o->rotation != -1 && o->rotation != 0 &&
        o->rotation != 90 && o->rotation != 180 && o->rotation != 270)
        return "Operation failed";
    return NULL;
}

/* Implementation details. */
static void opts_init(prn_job_opts_t *opts)
{
    memset(opts, 0, sizeof *opts);
    opts->crop_x = 0; opts->crop_y = 0;
    opts->crop_w = 1; opts->crop_h = 1;             /* Implementation details. */
    opts->rotation = -1;                            /* Implementation details. */
}

static int parse_opts_dict(DBusMessageIter *args, prn_job_opts_t *opts,
                           int copies_given, const char **badkey,
                           const char **errmsg)
{
    *badkey = NULL;
    *errmsg = NULL;
    if (dbus_message_iter_get_arg_type(args) != DBUS_TYPE_ARRAY)
        return 0;                                    /* Implementation details. */
    DBusMessageIter dict;
    dbus_message_iter_recurse(args, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry, v;
        const char *key = NULL;
        dbus_message_iter_recurse(&dict, &entry);
        dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (dbus_message_iter_get_arg_type(&entry) != DBUS_TYPE_VARIANT) {
            dbus_message_iter_next(&dict);
            continue;
        }
        dbus_message_iter_recurse(&entry, &v);
        int vt = dbus_message_iter_get_arg_type(&v);
        if (key && !strcmp(key, "copies")) {
            if (vt != DBUS_TYPE_INT32) { *badkey = key; return -1; }
            dbus_int32_t cv;
            dbus_message_iter_get_basic(&v, &cv);
            opts->copies = (int)cv;
            copies_given = 1;
        } else if (key && !strcmp(key, "media")) {
            if (vt != DBUS_TYPE_STRING) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->media);
        } else if (key && !strcmp(key, "print-quality")) {
            if (vt != DBUS_TYPE_STRING) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->print_quality);
        } else if (key && !strcmp(key, "print-color-mode")) {
            if (vt != DBUS_TYPE_STRING) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->color_mode);
        } else if (key && !strcmp(key, "print-scaling")) {
            if (vt != DBUS_TYPE_STRING) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->print_scaling);
        } else if (key && !strcmp(key, "crop_x")) {   /* §18 */
            if (vt != DBUS_TYPE_DOUBLE) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->crop_x);
        } else if (key && !strcmp(key, "crop_y")) {
            if (vt != DBUS_TYPE_DOUBLE) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->crop_y);
        } else if (key && !strcmp(key, "crop_w")) {
            if (vt != DBUS_TYPE_DOUBLE) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->crop_w);
        } else if (key && !strcmp(key, "crop_h")) {
            if (vt != DBUS_TYPE_DOUBLE) { *badkey = key; return -1; }
            dbus_message_iter_get_basic(&v, &opts->crop_h);
        } else if (key && !strcmp(key, "rotation")) {
            if (vt != DBUS_TYPE_INT32) { *badkey = key; return -1; }
            dbus_int32_t rv;
            dbus_message_iter_get_basic(&v, &rv);
            opts->rotation = (int)rv;
        } /* Implementation details. */
        dbus_message_iter_next(&dict);
    }
    if (!copies_given) opts->copies = 0;              /* Implementation details. */
    *errmsg = edit_opts_check(opts);
    if (*errmsg) return -2;
    return 0;
}

/* Implementation details. */
static DBusHandlerResult m_start_print_job(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter args;
    if (!dbus_message_iter_init(m, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "expect (as a{sv})");

    /* arg0: as paths */
    char *paths[64];
    int npaths = 0;
    DBusMessageIter arr;
    dbus_message_iter_recurse(&args, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *p = NULL;
        dbus_message_iter_get_basic(&arr, &p);
        if (npaths < 64) paths[npaths++] = strdup(p ? p : "");
        else { LOGW("start_print_job: too many paths, truncated at 64"); break; }
        dbus_message_iter_next(&arr);
    }
    if (npaths == 0) {
        for (int i = 0; i < npaths; i++) free(paths[i]);
        return reply_err(c, m, ERR_PARAM, "empty paths");
    }

    /* Implementation details. */
    prn_job_opts_t opts;
    opts_init(&opts);
    if (dbus_message_iter_next(&args)) {
        const char *badkey = NULL, *errmsg = NULL;
        int prc = parse_opts_dict(&args, &opts, 0, &badkey, &errmsg);
        if (prc != 0) {
            for (int i = 0; i < npaths; i++) free(paths[i]);
            char msg[160];
            if (prc == -1)
                snprintf(msg, sizeof msg, "opts.%s value type mismatch",
                         badkey ? badkey : "?");
            else
                snprintf(msg, sizeof msg, "%s", errmsg ? errmsg : "?");
            return reply_err(c, m, ERR_PARAM, msg);
        }
    }

    char *job_id = NULL;
    int rc = g_be->start_print_job(paths, npaths, &opts, &job_id);
    for (int i = 0; i < npaths; i++) free(paths[i]);
    if (rc < 0) return reply_be_err(c, m, rc, "start_print_job");

    DBusMessage *rep = dbus_message_new_method_return(m);
    dbus_message_append_args(rep, DBUS_TYPE_STRING, &job_id, DBUS_TYPE_INVALID);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    free(job_id);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */

/* Implementation details. */
static DBusHandlerResult m_get_photo_preview(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    const char *path = NULL;
    dbus_int32_t mw = 0, mh = 0;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &path,
                               DBUS_TYPE_INT32, &mw, DBUS_TYPE_INT32, &mh,
                               DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
                                        e.message);
        dbus_error_free(&e);
        return r;
    }
    char *out = NULL;
    int rc = g_be->photo_preview(path, (int)mw, (int)mh, &out);
    if (rc < 0) return reply_be_err(c, m, rc, "get_photo_preview");
    DBusMessage *rep = dbus_message_new_method_return(m);
    dbus_message_append_args(rep, DBUS_TYPE_STRING, &out, DBUS_TYPE_INVALID);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    free(out);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */
static void dict_i32(DBusMessageIter *arr, const char *k, dbus_int32_t v)
{
    DBusMessageIter entry, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_INT32_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_INT32, &v);
    dbus_message_iter_close_container(&entry, &w);
    dbus_message_iter_close_container(arr, &entry);
}
static void dict_bool(DBusMessageIter *arr, const char *k, dbus_bool_t v)
{
    DBusMessageIter entry, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_BOOLEAN_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_BOOLEAN, &v);
    dbus_message_iter_close_container(&entry, &w);
    dbus_message_iter_close_container(arr, &entry);
}
static void dict_i64(DBusMessageIter *arr, const char *k, dbus_int64_t v)
{
    DBusMessageIter entry, w;
    dbus_message_iter_open_container(arr, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
    dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &k);
    dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
                                     DBUS_TYPE_INT64_AS_STRING, &w);
    dbus_message_iter_append_basic(&w, DBUS_TYPE_INT64, &v);
    dbus_message_iter_close_container(&entry, &w);
    dbus_message_iter_close_container(arr, &entry);
}

/* Implementation details. */
static DBusHandlerResult m_get_photo_info(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    const char *path = NULL;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_STRING, &path, DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs",
                                        e.message);
        dbus_error_free(&e);
        return r;
    }
    prn_photo_info_t pi;
    int rc = g_be->photo_info(path, &pi);
    if (rc < 0) return reply_be_err(c, m, rc, "get_photo_info");

    DBusMessage *rep = dbus_message_new_method_return(m);
    DBusMessageIter it, arr;
    dbus_message_iter_init_append(rep, &it);
    dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "{sv}", &arr);
    dict_i32(&arr, "width", pi.width);
    dict_i32(&arr, "height", pi.height);
    dict_i32(&arr, "orientation", pi.orientation);
    dict_i32(&arr, "rotated_width", pi.rotated_width);
    dict_i32(&arr, "rotated_height", pi.rotated_height);
    dict_bool(&arr, "has_jpeg", pi.has_jpeg ? TRUE : FALSE);
    dict_bool(&arr, "is_3fr", pi.is_3fr ? TRUE : FALSE);
    dict_i64(&arr, "file_size", (dbus_int64_t)pi.file_size);
    dbus_message_iter_close_container(&it, &arr);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */
static DBusHandlerResult m_preview_print(DBusConnection *c, DBusMessage *m)
{
    DBusMessageIter args;
    if (!dbus_message_iter_init(m, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING)
        return reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", "expect (s a{sv})");
    const char *path = NULL;
    dbus_message_iter_get_basic(&args, &path);

    prn_job_opts_t opts;
    opts_init(&opts);
    if (dbus_message_iter_next(&args)) {
        const char *badkey = NULL, *errmsg = NULL;
        int prc = parse_opts_dict(&args, &opts, 1 /* Implementation details. */, &badkey, &errmsg);
        if (prc != 0) {
            char msg[160];
            if (prc == -1)
                snprintf(msg, sizeof msg, "opts.%s value type mismatch",
                         badkey ? badkey : "?");
            else
                snprintf(msg, sizeof msg, "%s", errmsg ? errmsg : "?");
            return reply_err(c, m, ERR_PARAM, msg);
        }
    }
    char *out = NULL;
    int rc = g_be->preview_print(path, &opts, &out);
    if (rc < 0) return reply_be_err(c, m, rc, "preview_print");
    DBusMessage *rep = dbus_message_new_method_return(m);
    dbus_message_append_args(rep, DBUS_TYPE_STRING, &out, DBUS_TYPE_INVALID);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    free(out);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Cancel_job() → b */
static DBusHandlerResult m_cancel_job(DBusConnection *c, DBusMessage *m)
{
    dbus_bool_t canceled = g_be->cancel_job() ? TRUE : FALSE;
    DBusMessage *rep = dbus_message_new_method_return(m);
    dbus_message_append_args(rep, DBUS_TYPE_BOOLEAN, &canceled, DBUS_TYPE_INVALID);
    dbus_connection_send(c, rep, NULL);
    dbus_message_unref(rep);
    return DBUS_HANDLER_RESULT_HANDLED;
}

/* Implementation details. */
static DBusHandlerResult m_tx(DBusConnection *c, DBusMessage *m)
{
    DBusError e;
    dbus_error_init(&e);
    unsigned char len = 0;
    const unsigned char *payload = NULL;                 /* Implementation details. */
    int plen = 0;
    if (!dbus_message_get_args(m, &e, DBUS_TYPE_BYTE, &len,
                               DBUS_TYPE_ARRAY, DBUS_TYPE_BYTE,
                               &payload, &plen, DBUS_TYPE_INVALID)) {
        DBusHandlerResult r = reply_err(c, m, "org.freedesktop.DBus.Error.InvalidArgs", e.message);
        dbus_error_free(&e);
        return r;
    }

    /* Implementation details. */
    char hex[64 * 3 + 1];                                /* "41 42 ..." + NUL */
    int hi = 0;
    static const char HEXD[] = "0123456789ABCDEF";
    for (int i = 0; i < plen && i < 64; i++) {
        if (i) hex[hi++] = ' ';
        hex[hi++] = HEXD[payload[i] >> 4];
        hex[hi++] = HEXD[payload[i] & 0xf];
    }
    hex[hi] = 0;
    LOGD("tx: len=%u payload=%d B [%s]%s", (unsigned)len, plen, hex,
         plen > 64 ? "Operation failed" : "");

    prn_set_tx_active(1);                                /* Implementation details. */
    if (plen > 0) prn_adaptor_emit_rx(len, payload, plen); /* Implementation details. */
    prn_set_tx_active(0);                                /* Implementation details. */
    return reply_empty(c, m);
}

/* Implementation details. */
static DBusHandlerResult handle_methods(DBusConnection *c, DBusMessage *m)
{
    prn_adaptor_touch();                   /* Implementation details. */
    if (dbus_message_is_method_call(m, PRN_IFACE, "Connect_to_ap"))          return m_connect_to_ap(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Scan_wifi"))              return m_scan_wifi(c, m);   /* §20 */
    if (dbus_message_is_method_call(m, PRN_IFACE, "Disconnect_and_restore")) return m_disconnect(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Start_discovery"))        return m_start_discovery(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Select_printer"))         return m_select_printer(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Get_photo_list"))         return m_get_photo_list(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Start_print_job"))        return m_start_print_job(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Cancel_job"))            return m_cancel_job(c, m);
    if (dbus_message_is_method_call(m, PRN_IFACE, "Get_photo_preview"))     return m_get_photo_preview(c, m); /* §18 */
    if (dbus_message_is_method_call(m, PRN_IFACE, "Get_photo_info"))        return m_get_photo_info(c, m);    /* §18 */
    if (dbus_message_is_method_call(m, PRN_IFACE, "Preview_print"))         return m_preview_print(c, m);    /* §18 */
    if (dbus_message_is_method_call(m, PRN_IFACE, "tx"))                    return m_tx(c, m); /* §7b */
    if (dbus_message_has_destination(m, PRN_DEST))   /* Implementation details. */
        return reply_err(c, m, ERR_FAILED, "No such method");
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* Implementation details. */
static const char INTROSPECTION_XML[] =
    "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
    "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
    "<node name=\"printer\">\n"
    "  <interface name=\"com.hasselblad.printer\">\n"
    "    <property name=\"connection_state\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"ssid\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"printer_state\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"printer_name\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"printer_ip\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"job_progress\" type=\"i\" access=\"read\"/>\n"
    "    <property name=\"job_status\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"copies\" type=\"i\" access=\"readwrite\"/>\n"
    "    <property name=\"media\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"print_quality\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"color_mode\" type=\"s\" access=\"readwrite\"/>\n"
    "    <property name=\"preset\" type=\"i\" access=\"readwrite\"/>\n"
    "    <property name=\"document_format\" type=\"s\" access=\"read\"/>\n"
    "    <property name=\"caps_ready\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"copies_max\" type=\"i\" access=\"read\"/>\n"
    "    <property name=\"supported_media\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"supported_qualities\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"supported_color_modes\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"supported_formats\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"media_size_info\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"discovered_printers\" type=\"as\" access=\"read\"/>\n"
    "    <property name=\"scanned_networks\" type=\"as\" access=\"read\"/>\n"     /* §20 */
    "    <property name=\"synced\" type=\"b\" access=\"read\"/>\n"
    "    <property name=\"tx_active\" type=\"b\" access=\"read\"/>\n"
    "    <method name=\"Connect_to_ap\">\n"
    "      <arg name=\"ssid\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"psk\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"accepted\" direction=\"out\" type=\"b\"/>\n"
    "    </method>\n"
    "    <method name=\"Disconnect_and_restore\"/>\n"
    "    <method name=\"Scan_wifi\">\n"                                          /* §20 */
    "      <arg name=\"networks\" direction=\"out\" type=\"as\"/>\n"
    "    </method>\n"
    "    <method name=\"Start_discovery\">\n"
    "      <arg name=\"printers\" direction=\"out\" type=\"as\"/>\n"
    "    </method>\n"
    "    <method name=\"Select_printer\">\n"
    "      <arg name=\"name\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"selected\" direction=\"out\" type=\"b\"/>\n"
    "    </method>\n"
    "    <method name=\"Get_photo_list\">\n"
    "      <arg name=\"path\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"files\" direction=\"out\" type=\"as\"/>\n"
    "    </method>\n"
    "    <method name=\"Start_print_job\">\n"
    "      <arg name=\"paths\" direction=\"in\" type=\"as\"/>\n"
    "      <arg name=\"opts\" direction=\"in\" type=\"a{sv}\"/>\n"
    "      <arg name=\"job_id\" direction=\"out\" type=\"s\"/>\n"
    "    </method>\n"
    "    <method name=\"Cancel_job\">\n"
    "      <arg name=\"canceled\" direction=\"out\" type=\"b\"/>\n"
    "    </method>\n"
    "    <method name=\"Get_photo_preview\">\n"                  /* §18 */
    "      <arg name=\"path\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"max_w\" direction=\"in\" type=\"i\"/>\n"
    "      <arg name=\"max_h\" direction=\"in\" type=\"i\"/>\n"
    "      <arg name=\"jpeg_path\" direction=\"out\" type=\"s\"/>\n"
    "    </method>\n"
    "    <method name=\"Get_photo_info\">\n"
    "      <arg name=\"path\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"info\" direction=\"out\" type=\"a{sv}\"/>\n"
    "    </method>\n"
    "    <method name=\"Preview_print\">\n"
    "      <arg name=\"path\" direction=\"in\" type=\"s\"/>\n"
    "      <arg name=\"opts\" direction=\"in\" type=\"a{sv}\"/>\n"
    "      <arg name=\"jpeg_path\" direction=\"out\" type=\"s\"/>\n"
    "    </method>\n"
    "    <method name=\"tx\">\n"
    "      <arg name=\"message_len\" direction=\"in\" type=\"y\"/>\n"
    "      <arg name=\"message\" direction=\"in\" type=\"ay\"/>\n"
    "    </method>\n"
    "    <signal name=\"rx\">\n"
    "      <arg name=\"message_len\" type=\"y\"/>\n"
    "      <arg name=\"message\" type=\"ay\"/>\n"
    "    </signal>\n"
    "    <signal name=\"testrx\">\n"
    "      <arg name=\"message_len\" type=\"y\"/>\n"
    "      <arg name=\"message\" type=\"ay\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Properties\">\n"
    "    <method name=\"Get\"><arg name=\"iface\" direction=\"in\" type=\"s\"/><arg name=\"name\" direction=\"in\" type=\"s\"/><arg name=\"value\" direction=\"out\" type=\"v\"/></method>\n"
    "    <method name=\"Set\"><arg name=\"iface\" direction=\"in\" type=\"s\"/><arg name=\"name\" direction=\"in\" type=\"s\"/><arg name=\"value\" direction=\"in\" type=\"v\"/></method>\n"
    "    <method name=\"GetAll\"><arg name=\"iface\" direction=\"in\" type=\"s\"/><arg name=\"props\" direction=\"out\" type=\"a{sv}\"/></method>\n"
    "    <signal name=\"propertiesChanged\">\n"
    "      <arg name=\"interface\" type=\"s\"/>\n"
    "      <arg name=\"changed\" type=\"a{sv}\"/>\n"
    "      <arg name=\"invalidated\" type=\"as\"/>\n"
    "    </signal>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
    "    <method name=\"Introspect\"><arg name=\"xml\" direction=\"out\" type=\"s\"/></method>\n"
    "  </interface>\n"
    "  <interface name=\"org.freedesktop.DBus.Peer\">\n"
    "    <method name=\"Ping\"/>\n"
    "  </interface>\n"
    "</node>\n";

/* Implementation details. */
static DBusHandlerResult obj_filter(DBusConnection *c, DBusMessage *m, void *data)
{
    (void)data;
    if (dbus_message_is_method_call(m, IFACE_INTROSPECT, "Introspect")) {
        const char *xml = INTROSPECTION_XML;
        DBusMessage *rep = dbus_message_new_method_return(m);
        dbus_message_append_args(rep, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
        dbus_connection_send(c, rep, NULL);
        dbus_message_unref(rep);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (dbus_message_is_method_call(m, IFACE_PEER, "Ping"))
        return reply_empty(c, m);

    if (dbus_message_is_method_call(m, IFACE_PROPERTIES, "GetAll")) return handle_get_all(c, m);
    if (dbus_message_is_method_call(m, IFACE_PROPERTIES, "Get"))    return handle_get(c, m);
    if (dbus_message_is_method_call(m, IFACE_PROPERTIES, "Set"))    return handle_set(c, m);

    return handle_methods(c, m);
}

int prn_adaptor_start(DBusConnection *c, const prn_backend *be)
{
    g_conn = c;
    g_be = be;
    g_last_activity = now_ms_a();           /* Implementation details. */
    /* {unregister, message, pad1..pad4} */
    static DBusObjectPathVTable vt = { NULL, obj_filter, NULL, NULL, NULL, NULL };
    if (!dbus_connection_register_object_path(c, PRN_PATH, &vt, NULL)) {
        LOGE("register_object_path %s failed", PRN_PATH);
        return -1;
    }
    LOGI("adaptor: object %s registered (backend=%s)", PRN_PATH, be->name);
    return 0;
}
