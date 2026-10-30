/* client.h — libdbus client wrapper (wire protocol: analysis/dbus/REPORT_odindb_conventions.md)
 * Templated triplet: destination=interface=com.hasselblad.<mod>, path=/<mod>
 */
#ifndef ODIN_CLIENT_H
#define ODIN_CLIENT_H

#include <dbus/dbus.h>
#include <stddef.h>

#define ODIN_IFACE_PROPERTIES "org.freedesktop.DBus.Properties"
#define ODIN_IFACE_INTROSPECT "org.freedesktop.DBus.Introspectable"
#define ODIN_BUS_DAEMON       "org.freedesktop.DBus"
#define ODIN_BUS_PATH         "/org/freedesktop/DBus"
/* Property-change signal member — lowercase p, exactly as in original QtDBus;
 * using standard capitalization will not receive it */
#define ODIN_SIG_PROPCHANGED  "propertiesChanged"
/* LinkstatusProxy base class always fetches this interface on connect (for wire-level matching) */
#define ODIN_IFACE_LINKSTATUS "com.hasselblad.linkstatus"

#define ODIN_DEFAULT_TIMEOUT_MS 25000   /* Original default timeout: 25 seconds */

/* com.hasselblad.error.* → E_ReturnStatus numeric values (also process exit codes) */
int odin_error_exitcode(const char *error_name);

/* ---- Introspection parse model (this module's interface only) ---- */
typedef struct {
    char name[64];
    char type[12];
} odin_arg;

typedef struct {
    char     name[64];
    odin_arg in[8];
    int      n_in;
    odin_arg out[8];
    int      n_out;
} odin_method;

typedef struct {
    char name[64];
    char type[12];
    int  writable;                    /* introspection access includes write */
} odin_prop;

typedef struct {
    char     name[64];
    odin_arg args[8];
    int      n_args;
} odin_signal;

typedef struct {
    odin_method methods[48];
    int         n_methods;
    odin_prop   props[64];
    int         n_props;
    odin_signal signals[48];
    int         n_signals;
} odin_iface;

/* Find by name in model; return NULL if not found */
const odin_method *odin_iface_find_method(const odin_iface *ifc, const char *name);
const odin_prop   *odin_iface_find_prop(const odin_iface *ifc, const char *name);
const odin_signal *odin_iface_find_signal(const odin_iface *ifc, const char *name);

typedef struct {
    DBusConnection *conn;
    char module[48];                  /* Bare module name */
    char dest[96];                    /* com.hasselblad.<mod> */
    char path[64];                    /* /<mod> */
} odin_client;

/* Connect to bus (bus: "system"/"session"/NULL=system; "none"=do not connect) +
 * templated triplet */
int  odin_client_init(odin_client *oc, const char *module, const char *bus, DBusError *e);
void odin_client_free(odin_client *oc);

/* QtDBus-equivalent startup sequence (AddMatch propertiesChanged / NameOwnerChanged ×2,
 * GetNameOwner, NameHasOwner, GetAll(this module), GetAll(linkstatus)).
 * Returns 0=name is on bus; -1=not present; -2=bus error (e is populated).
 * *getall_reply (may be NULL) receives this module's GetAll reply (borrowed; caller unrefs). */
int odin_client_start(odin_client *oc, DBusMessage **getall_reply, DBusError *e);

/* Generic synchronous call (25s default); returns reply on success (caller unrefs), NULL on
 * failure with reason in e/errno */
DBusMessage *odin_call(odin_client *oc, const char *dest, const char *path,
                       const char *iface, const char *member, int timeout_ms,
                       DBusError *e, DBusMessage *req);

DBusMessage *odin_getall(odin_client *oc, const char *iface, DBusError *e);
DBusMessage *odin_get(odin_client *oc, const char *prop, DBusError *e);
DBusMessage *odin_introspect(odin_client *oc, DBusError *e);

/* Set(ssv) builder: begin opens variant container (caller appends value to var); end closes
 * and sends */
typedef struct {
    DBusMessage    *msg;
    DBusMessageIter parent;
    DBusMessageIter var;
} odin_set_builder;

int odin_set_begin(odin_client *oc, const char *prop, const char *value_sig,
                   odin_set_builder *b);
DBusMessage *odin_set_end(odin_client *oc, odin_set_builder *b, DBusError *e);

/* Bus-name lookup / match-rule subscription (fail silently, matching original startup tolerance) */
int  odin_name_has_owner(odin_client *oc, const char *name);
int  odin_add_match(odin_client *oc, const char *rule);
int  odin_remove_match(odin_client *oc, const char *rule);

/* Fetch and parse introspection XML into ifc (retain this module's interface only); 0=success */
int odin_introspect_parse(odin_client *oc, odin_iface *ifc, DBusError *e);

/* Filter ListNames for com.hasselblad.* and write to names (each item malloc'd, caller frees);
 * return count/-1 */
int odin_list_modules(odin_client *oc, char ***names, DBusError *e);

#endif
