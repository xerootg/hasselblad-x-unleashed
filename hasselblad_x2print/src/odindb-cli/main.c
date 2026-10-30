/* Implementation details. */
#include "argspec.h"
#include "client.h"
#include "dumpfmt.h"
#include "faces.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define CLI_VERSION "x2print-1.0.0"

/* Implementation details. */
static void print_usage(const char *argv0)
{
    printf("Usage: %s [options]\n", argv0);
    printf("Tool for accessing properties, methods and signals defined in OdinDB\n");
    printf("\n");
    printf("Options:\n");
    printf("  -h, --help                         Displays help on commandline options.\n");
    printf("  --help-all                         Displays help, including generic Qt\n");
    printf("                                     options.\n");
    printf("  -v, --version                      Displays version information.\n");
    printf("  -b, --bus <bus>                    Use specific D-Bus (system/session/none)\n");
    printf("  -a, --appsmessaging                Show appsmessaging information.\n");
    printf("  -e, --enum <enum name>             Show all members of the enum\n");
    printf("  -i, --info                         Show extra information like (argument info\n");
    printf("                                     etc)\n");
    printf("  -l                                 Can be added as argument to -s, -m, -p, -e\n");
    printf("                                     and will list the request\n");
    printf("  -m, --method <method>              Method\n");
    printf("  -p, --property <property>          Get/set property\n");
    printf("  -s, --service <service>            Service name. Must be set for all\n");
    printf("                                     arguments except -e, -w, -r and\n");
    printf("                                     list-services\n");
    printf("  -r, --release-wakelock             Release wakelock acquired by odindb-send\n");
    printf("  -w, --acquire-wakelock             Acquires a wakelock which assures that\n");
    printf("                                     system does not enter kernel suspend\n");
    printf("  --list-services                    List all services\n");
    printf("  --list-methods                     List methods of a service\n");
    printf("  --list-properties                  List properties of a service\n");
    printf("  --get-all                          List properties and their current values\n");
    printf("                                     of a service\n");
    printf("  --list-signals                     List signals of a service\n");
    printf("  --list-enums                       List enums\n");
    printf("  --list-all                         List methods, properties and signals of a\n");
    printf("                                     service\n");
    printf("  --apps-messaging-version           Show apps-messaging version\n");
    printf("  --show-usage                       Shows example usage of the application\n");
    printf("  --signal <signal>                  Monitor signals emitted from the service\n");
    printf("  --signal-oneshot <signal-oneshot>  Monitor signals emitted from the service\n");
    printf("                                     oneshot\n");
    printf("  --trigger-memunreachable           'libmemunreachable' is an AOSP-tool that\n");
    printf("                                     can look for unreachable memory\n");
    printf("  --print-cmdline                    Print cmd line to log\n");
}

static void print_show_usage(void)
{
    /* Implementation details. */
    printf("# List all services\n");
    printf("odindb-send -s -l\n");
    printf("odindb-send --list-services\n");
    printf("\n");
    printf("# List methods on a service\n");
    printf("odindb-send -s camera -m -l\n");
    printf("\n");
    printf("# List properties on a service\n");
    printf("odindb-send -s camera -p -l\n");
    printf("\n");
    printf("# List signals on a service\n");
    printf("odindb-send -s camera --list-signals\n");
    printf("\n");
    printf("# List all enums in the system\n");
    printf("odindb-send --list-enums\n");
    printf("\n");
    printf("# List all properties, signals and methods from a service\n");
    printf("odindb-send --service camera --list-all\n");
    printf("\n");
    printf("# Get extra information about all properties a service\n");
    printf("odindb-send -s camera --list-properties -i\n");
    printf("\n");
    printf("# Get extra information about a property\n");
    printf("odindb-send -s camera -p exp_mode -i\n");
    printf("\n");
    printf("# Get extra information about all methods on a service\n");
    printf("odindb-send -s camera --list-methods -i\n");
    printf("\n");
    printf("# Get extra information about a method\n");
    printf("odindb-send -s camera -m set_live_view -i\n");
    printf("\n");
    printf("# Get extra information about an enum\n");
    printf("odindb-send -e E_ExposureStatus -i\n");
    printf("\n");
    printf("# Monitor a signal from the camera-service\n");
    printf("odindb-send -s camera --signal live_view_stateChanged\n");
    printf("\n");
    printf("# Monitor multiple signals from the camera-service\n");
    printf("# Just keep adding signal names separated by a comma: signal1,signal2,...,signalX\n");
    printf("\n");
    printf("odindb-send -s camera --signal live_view_stateChanged,stream_groupChanged,exp_modeChanged\n");
    printf("\n");
    printf("# Monitor all signals sent out from a service\n");
    printf("odindb-send -s camera --signal all\n");
    printf("\n");
    printf("# Monitor a signal and exit after it has been received\n");
    printf("odindb-send -s camera --signal-oneshot exp_modeChanged\n");
    printf("\n");
    printf("# Call method with list of strings argument\n");
    printf("odindb-send -s camera -m foo \"[ \\\"One\\\", \\\"Two\\\", \\\"Three\\\" ]\"\n");
    printf("\n");
    printf("# Call method with QVariantMap argument\n");
    printf("odindb-send -s error -m report E_ErrorCode_LensNotConnected E_ErrorCategory_System E_ErrorAction_Ackable \"{\\\"key1\\\":\\\"value1\\\", \\\"key2\\\":42}\" foo bar 231\n");
    printf("\n");
    printf("# Trigger memunreachable\n");
    printf("odindb-send -s camera --trigger-memunreachable# Run a method\n");
    printf("odindb-send -s camera -m set_live_view true\n");
    printf("\n");
    printf("# Run a method with (QString, QVariantMap) as arguments\n");
    printf("odindb-send -s storage -m set_metadata /cfe/939HASBL/B0000881 \"{\\\"lensModel\\\":\\\"CF 38\\\",\\\"av\\\":60}\"\n");
    printf("\n");
    printf("# Run a method with a bitset enum as argument\n");
    printf("odindb-send -s camera -m do_exposure_extended2 1 \"E_SessionOptions_KeepSession|E_SessionOptions_SimpleDriveMode|E_SessionOptions_ForceSingleExposure\" 0\n");
    printf("# Get a property\n");
    printf("odindb-send -s camera -p tv_selected\n");
    printf("# Set a property\n");
    printf("odindb-send -s camera -p tv_selected 60\n");
    printf("# Set a property with negative argument\n");
    printf("odindb-send -s camera -p tv_selected -- -60\n");
    printf("# Get a property bit (0 or 1)\n");
    printf("odindb-send -s camera -p status:E_CameraStatus_Initialised\n");
    printf("# Ensure system does not enter suspend\n");
    printf("odindb-send --acquire-wakelock\n");
    printf("# Release a previously acquired wakelock\n");
    printf("odindb-send --release-wakelock\n");
}

/* Implementation details. */
static const char *o_service, *o_prop, *o_enum, *o_bus;
static const char *o_signal, *o_signal1;
static const char *o_prop_value;
static const char *o_meth_name;       /* Implementation details. */
static char *o_margs[16];
static int o_nmargs;
static int f_info, f_w, f_r, f_listsvc, f_listm, f_listp, f_listsig,
           f_liste, f_listall, f_getall, f_memunreach, f_amver, f_showusage,
           f_printcmd, f_help, f_version, f_appmsg, f_any;
static volatile sig_atomic_t g_stop;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Implementation details. */
static int is_flaglike(const char *s)
{
    return !strcmp(s, "-l") || !strcmp(s, "-i") || !strcmp(s, "-w") ||
           !strcmp(s, "-r") || !strcmp(s, "-a") || !strcmp(s, "-v") ||
           !strcmp(s, "-h") || !strncmp(s, "--", 2);
}

static int parse_options(int argc, char **argv)
{
    int dashdash = 0;
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        if (dashdash) goto positional;
        if (!strcmp(a, "--")) { dashdash = 1; continue; }

        if (!strncmp(a, "--", 2)) {
            const char *eq = strchr(a, '=');
            char key[48];
            size_t kl = eq ? (size_t)(eq - a) : strlen(a);
            if (kl >= sizeof key) kl = sizeof key - 1;
            memcpy(key, a, kl);
            key[kl] = 0;
            const char *val = eq ? eq + 1 : NULL;
#define NEED_VAL()                                                      \
    do {                                                                \
        if (!val && i + 1 < argc && !is_flaglike(argv[i + 1]))          \
            val = argv[++i];                                            \
    } while (0)
            if (!strcmp(key, "--service")) { NEED_VAL(); o_service = val; }
            else if (!strcmp(key, "--method")) {
                NEED_VAL();
                if (val) o_meth_name = val; else f_listm = 1;   /* -m -l */
            }
            else if (!strcmp(key, "--property")) {
                NEED_VAL();
                if (val) o_prop = val; else f_listp = 1;        /* -p -l */
            }
            else if (!strcmp(key, "--enum")) { NEED_VAL(); o_enum = val; }
            else if (!strcmp(key, "--bus")) { NEED_VAL(); o_bus = val; }
            else if (!strcmp(key, "--signal")) { NEED_VAL(); o_signal = val; }
            else if (!strcmp(key, "--signal-oneshot")) { NEED_VAL(); o_signal1 = val; }
            else if (!strcmp(key, "--info")) f_info = 1;
            else if (!strcmp(key, "--list-services")) f_listsvc = 1;
            else if (!strcmp(key, "--list-methods")) f_listm = 1;
            else if (!strcmp(key, "--list-properties")) f_listp = 1;
            else if (!strcmp(key, "--list-signals")) f_listsig = 1;
            else if (!strcmp(key, "--list-enums")) f_liste = 1;
            else if (!strcmp(key, "--list-all")) f_listall = 1;
            else if (!strcmp(key, "--get-all")) f_getall = 1;
            else if (!strcmp(key, "--trigger-memunreachable")) f_memunreach = 1;
            else if (!strcmp(key, "--apps-messaging-version")) f_amver = 1;
            else if (!strcmp(key, "--show-usage")) f_showusage = 1;
            else if (!strcmp(key, "--print-cmdline")) f_printcmd = 1;
            else if (!strcmp(key, "--acquire-wakelock")) f_w = 1;
            else if (!strcmp(key, "--release-wakelock")) f_r = 1;
            else if (!strcmp(key, "--appsmessaging")) f_appmsg = 1;
            else if (!strcmp(key, "--help-all") || !strcmp(key, "--help")) f_help = 1;
            else if (!strcmp(key, "--version")) f_version = 1;
            else if (!strcmp(key, "--any")) f_any = 1;   /* Implementation details. */
            else { printf("Unknown option: %s\n", a); return -1; }
            continue;
        }
        if (a[0] == '-' && a[1] && !a[2]) {
            const char *val = NULL;
            if (strchr("smpeb", a[1]) && i + 1 < argc &&
                !is_flaglike(argv[i + 1]))
                val = argv[++i];
            switch (a[1]) {
            case 's': o_service = val; break;
            case 'e': o_enum = val; break;
            case 'b': o_bus = val; break;
            case 'm':
                if (val) o_meth_name = val;
                else f_listm = 1;             /* Implementation details. */
                break;
            case 'p':
                if (val) o_prop = val;
                else f_listp = 1;             /* Implementation details. */
                break;
            case 'i': f_info = 1; break;
            case 'l': f_listsvc = 1; break;   /* Implementation details. */
            case 'w': f_w = 1; break;
            case 'r': f_r = 1; break;
            case 'a': f_appmsg = 1; break;
            case 'v': f_version = 1; break;
            case 'h': f_help = 1; break;
            default: printf("Unknown option: %s\n", a); return -1;
            }
            continue;
        }
positional:
        /* Implementation details. */
        if (o_prop && !o_prop_value && !o_meth_name) {
            o_prop_value = a;
        } else if (o_meth_name || o_signal || o_signal1) {
            if (o_nmargs < 16) o_margs[o_nmargs++] = a;
        } else {
            printf("Unexpected argument: %s\n", a);
            return -1;
        }
    }
    return 0;
}

/* Implementation details. */
/* Implementation details. */
static int split_sig(const char *sig, char out[][8], int max)
{
    int n = 0;
    while (*sig && n < max) {
        int len = 1;
        if (sig[0] == 'a') {
            if (sig[1] == '{') {
                int depth = 1, j = 2;
                while (sig[j] && depth) {
                    if (sig[j] == '{') depth++;
                    else if (sig[j] == '}') depth--;
                    j++;
                }
                len = j;
            } else {
                len = 1 + (strchr("ynqiuxtdsogvb", sig[1]) ? 1 : 0);
            }
        }
        if (len > 7) len = 7;
        memcpy(out[n], sig, (size_t)len);
        out[n][len] = 0;
        sig += len;
        n++;
    }
    return n;
}

/* Implementation details. */
static const face_prop *face_prop_find(const odin_face *f, const char *name)
{
    for (int i = 0; i < f->n_props; i++)
        if (!strcmp(f->props[i].name, name)) return &f->props[i];
    return NULL;
}

static const face_method *face_method_find(const odin_face *f, const char *name)
{
    for (int i = 0; i < f->n_methods; i++) {
        if (!strcmp(f->methods[i].name, name)) return &f->methods[i];
    }
    if (name[0] == 'd' && name[1] == 'o')          /* Implementation details. */
        for (int i = 0; i < f->n_methods; i++)
            if (!strcmp(f->methods[i].name, name + 2)) return &f->methods[i];
    for (int i = 0; i < f->n_methods; i++)         /* Implementation details. */
        if (!strcasecmp(f->methods[i].name, name)) return &f->methods[i];
    return NULL;
}

static const face_signal *face_signal_find(const odin_face *f, const char *name)
{
    for (int i = 0; i < f->n_signals; i++)
        if (!strcmp(f->signals[i].name, name)) return &f->signals[i];
    return NULL;
}

/* Implementation details. */
static long face_block_number(const char *block)
{
    const char *lp = strrchr(block, '(');
    if (lp) {
        const char *rp = strchr(lp, ')');
        if (rp) return strtol(lp + 1, NULL, 0);
    }
    const char *eq = strchr(block, '=');
    if (eq) return strtol(eq + 1, NULL, 0);
    return 0;
}

/* Implementation details. */
static int dict_find(DBusMessage *reply, const char *name, DBusMessageIter *val)
{
    DBusMessageIter arr, ent;
    if (!reply || dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
        return 0;
    if (!dbus_message_iter_init(reply, &arr) ||
        dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_ARRAY)
        return 0;
    dbus_message_iter_recurse(&arr, &ent);
    do {
        if (dbus_message_iter_get_arg_type(&ent) != DBUS_TYPE_DICT_ENTRY) continue;
        DBusMessageIter kv;
        dbus_message_iter_recurse(&ent, &kv);
        const char *k = NULL;
        dbus_message_iter_get_basic(&kv, &k);
        if (k && !strcmp(k, name)) {
            dbus_message_iter_next(&kv);
            *val = kv;
            return 1;
        }
    } while (dbus_message_iter_next(&ent));
    return 0;
}

static int report_error_reply(DBusMessage *rep)
{
    const char *name = dbus_message_get_error_name(rep);
    const char *msg = "";
    DBusError e;
    dbus_error_init(&e);
    dbus_message_get_args(rep, &e, DBUS_TYPE_STRING, &msg, DBUS_TYPE_INVALID);
    dbus_error_free(&e);
    printf("Error %s: %s\n", name ? name : "?", msg ? msg : "");
    return odin_error_exitcode(name);
}

static void print_props_block(const odin_iface *ifc, int info)
{
    printf("Properties\n");           /* Implementation details. */
    printf("============\n");
    if (!info)
        for (int i = 0; i < ifc->n_props; i++)
            printf("Property: %s(%s)\n", ifc->props[i].name,
                   dumpfmt_qttype(ifc->props[i].type));
    else
        for (int i = 0; i < ifc->n_props; i++)
            printf("Name: %s\nType: %s\n\n", ifc->props[i].name,
                   dumpfmt_qttype(ifc->props[i].type));
    printf("============\n");
}

static void print_methods_block(const odin_iface *ifc, int info)
{
    printf("Methods:\n");
    printf("============\n");
    for (int m = 0; m < ifc->n_methods; m++) {
        const odin_method *mm = &ifc->methods[m];
        printf("Method: %s(", mm->name);
        for (int i = 0; i < mm->n_in; i++)
            printf("%s%s%s%s", i ? ", " : "", dumpfmt_qttype(mm->in[i].type),
                   mm->in[i].name[0] ? " " : "", mm->in[i].name);
        printf(")\n");
        if (info) {
            printf("In arguments:\n");
            for (int i = 0; i < mm->n_in; i++)
                printf("- %s%s%s\n", dumpfmt_qttype(mm->in[i].type),
                       mm->in[i].name[0] ? " " : "", mm->in[i].name);
            printf("- [optional] timeout (default 25 seconds)\n");
            printf("Out arguments:\n");
            if (mm->n_out == 0)
                printf("- No out arguments\n");
            for (int i = 0; i < mm->n_out; i++)
                printf("- %s%s%s\n", dumpfmt_qttype(mm->out[i].type),
                       mm->out[i].name[0] ? " " : "", mm->out[i].name);
            printf("\n");
        }
    }
    printf("============\n");
}

static void print_signals_block(const odin_iface *ifc)
{
    printf("Signals:\n");
    printf("============\n");
    for (int s = 0; s < ifc->n_signals; s++) {
        const odin_signal *sg = &ifc->signals[s];
        printf("Signal: %s(", sg->name);
        for (int i = 0; i < sg->n_args; i++)
            printf("%s%s", i ? "," : "", dumpfmt_qttype(sg->args[i].type));
        printf(")\n");
    }
    printf("============\n");
}

/* Implementation details. */
static int cmd_prop_get_face(const odin_face *f, DBusMessage *ga, const char *spec)
{
    char name[128], enumspec[128] = "";
    snprintf(name, sizeof name, "%s", spec);
    char *colon = strchr(name, ':');
    if (colon) {
        *colon = 0;
        snprintf(enumspec, sizeof enumspec, "%s", colon + 1);
    }
    const face_prop *fp = face_prop_find(f, name);
    if (!fp) {
        printf("\"Failed to find properties: %s \"\n", name);   /* Implementation details. */
        return 1;
    }
    DBusMessageIter val;
    if (dict_find(ga, name, &val)) {
        if (enumspec[0]) {
            DBusMessageIter sub;
            dbus_message_iter_recurse(&val, &sub);
            dbus_int32_t v = 0;
            int t = dbus_message_iter_get_arg_type(&sub);
            if (t == DBUS_TYPE_INT32) dbus_message_iter_get_basic(&sub, &v);
            else if (t == DBUS_TYPE_BOOLEAN) {
                dbus_bool_t b;
                dbus_message_iter_get_basic(&sub, &b);
                v = b ? 1 : 0;
            } else if (t == DBUS_TYPE_UINT32) {
                dbus_uint32_t u;
                dbus_message_iter_get_basic(&sub, &u);
                v = (dbus_int32_t)u;
            }
            if (!isdigit((unsigned char)enumspec[0])) {
                printf("\"Failed to resolve enum bit: %s \"\n", enumspec);
                return 1;
            }
            dbus_int32_t mask = (dbus_int32_t)strtol(enumspec, NULL, 0);
            printf("%s = %d\n", name, (v & mask) ? 1 : 0);
            return 0;
        }
        dumpfmt_prop_line(stdout, name, &val);
        return 0;
    }
    if (enumspec[0]) {                /* Implementation details. */
        if (!isdigit((unsigned char)enumspec[0])) {
            printf("\"Failed to resolve enum bit: %s \"\n", enumspec);
            return 1;
        }
        long v = face_block_number(fp->block ? fp->block : "");
        long mask = strtol(enumspec, NULL, 0);
        printf("%s = %d\n", name, (int)((v & mask) ? 1 : 0));
        return 0;
    }
    fputs(fp->block ? fp->block : "", stdout);    /* Implementation details. */
    fputc('\n', stdout);
    return 0;
}

/* Implementation details. */
static int cmd_prop_get(DBusMessage *ga, const char *spec)
{
    char name[128], enumspec[128] = "";
    snprintf(name, sizeof name, "%s", spec);
    char *colon = strchr(name, ':');
    if (colon) {
        *colon = 0;
        snprintf(enumspec, sizeof enumspec, "%s", colon + 1);
    }
    DBusMessageIter val;
    if (!dict_find(ga, name, &val)) {
        printf("\"Failed to find properties: %s \"\n", name);   /* Implementation details. */
        return 1;
    }
    if (enumspec[0]) {
        DBusMessageIter sub;
        dbus_message_iter_recurse(&val, &sub);
        if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_INT32 ||
            !isdigit((unsigned char)enumspec[0])) {
            printf("\"Failed to resolve enum bit: %s \"\n", enumspec);
            return 1;
        }
        dbus_int32_t v;
        dbus_message_iter_get_basic(&sub, &v);
        dbus_int32_t mask = (dbus_int32_t)strtol(enumspec, NULL, 0);
        printf("%s = %d\n", name, (v & mask) ? 1 : 0);
        return 0;
    }
    dumpfmt_prop_line(stdout, name, &val);
    return 0;
}

/* Implementation details. */
static int report_dbus_error_face(const DBusError *e, const char *member);

static int cmd_prop_set(odin_client *oc, odin_iface *ifc, const char *name,
                        const char *value)
{
    const odin_prop *pp = odin_iface_find_prop(ifc, name);
    if (!pp) {
        printf("No such property \"%s\" could be found.\n", name);  /* Implementation details. */
        return 1;
    }
    if (!pp->writable) {
        printf("Property is Read-only!\n");     /* Implementation details. */
        return 1;
    }
    odin_set_builder b;
    if (odin_set_begin(oc, pp->name, pp->type, &b)) return 1;
    char err[256];
    if (argspec_encode(&b.var, pp->type, value, err, sizeof err)) {
        printf("Invalid value for property!\n"); /* Implementation details. */
        dbus_message_unref(b.msg);
        return 1;
    }
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = odin_set_end(oc, &b, &e);
    if (!rep) {
        if (dbus_error_is_set(&e)) {   /* Implementation details. */
            int rc = report_dbus_error_face(&e, pp->name);
            dbus_error_free(&e);
            return rc;
        }
        printf("Error: send failed\n");
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        int rc = report_error_reply(rep);
        dbus_message_unref(rep);
        return rc;
    }
    dbus_message_unref(rep);
    return 0;                          /* Implementation details. */
}

/* Implementation details. */
static int report_dbus_error_face(const DBusError *e, const char *member)
{
    const char *msg = e->message ? e->message : "";
    printf("\"%s\" : \"%s\"\n", member, msg);
    printf("%s\n", msg);
    printf("Error\n");
    return odin_error_exitcode(e->name);
}

/* Implementation details. */
static int cmd_method_face(odin_client *oc, const odin_face *f, const char *mname)
{
    const face_method *fm = face_method_find(f, mname);
    if (!fm) {
        printf("No such method (%s) in service (%s)\n", mname, oc->module);
        return 1;
    }
    char types[12][8];
    int n_in = split_sig(fm->sig, types, 12);
    if (o_nmargs < n_in) {
        printf("Not enough arguments provided, expecting at least %d.\n", n_in);
        return 1;
    }
    DBusMessage *req = dbus_message_new_method_call(
        oc->dest, oc->path, oc->dest, fm->name);
    DBusMessageIter it;
    dbus_message_iter_init_append(req, &it);
    for (int i = 0; i < n_in; i++) {
        char err[256];
        if (argspec_encode(&it, types[i], o_margs[i], err, sizeof err)) {
            printf("Invalid argument %d: %s\n", i + 1, err);
            dbus_message_unref(req);
            return 1;
        }
    }
    int timeout_ms = ODIN_DEFAULT_TIMEOUT_MS;
    dbus_int32_t timeout_arg = -1;     /* Implementation details. */
    if (o_nmargs > n_in) {
        char *end = NULL;
        long secs = strtol(o_margs[n_in], &end, 0);
        if (!end || *end) {
            printf("Input parse error. Expected %d arg to be timeout of int type!\n",
                   n_in + 1);
            dbus_message_unref(req);
            return 1;
        }
        if (secs < 1) secs = 1;
        if (secs > 3600) secs = 3600;
        timeout_ms = (int)secs * 1000;
        timeout_arg = (dbus_int32_t)secs;
    }
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout_arg);
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        oc->conn, req, timeout_ms, &e);
    dbus_message_unref(req);
    if (!rep) {
        if (dbus_error_is_set(&e)) {   /* Implementation details. */
            int rc = report_dbus_error_face(&e, fm->name);
            dbus_error_free(&e);
            return rc;
        }
        printf("Error: call failed\n");
        return 1;
    }
    dbus_message_unref(rep);
    printf("Ok\n");                    /* Implementation details. */
    return 0;
}

/* Implementation details. */
static int cmd_method(odin_client *oc, odin_iface *ifc, const char *mname)
{
    const odin_method *mm = odin_iface_find_method(ifc, mname);
    if (!mm) {
        printf("No such method (%s) in service (%s)\n", mname, oc->module);
        return 1;
    }
    if (o_nmargs < mm->n_in) {
        printf("Not enough arguments provided, expecting at least %d.\n", mm->n_in);
        return 1;
    }
    DBusMessage *req = dbus_message_new_method_call(
        oc->dest, oc->path, oc->dest, mm->name);
    DBusMessageIter it;
    dbus_message_iter_init_append(req, &it);
    for (int i = 0; i < mm->n_in; i++) {
        char err[256];
        if (argspec_encode(&it, mm->in[i].type, o_margs[i], err, sizeof err)) {
            printf("Invalid argument %d: %s\n", i + 1, err);
            dbus_message_unref(req);
            return 1;
        }
    }
    int timeout_ms = ODIN_DEFAULT_TIMEOUT_MS;
    dbus_int32_t timeout_arg = -1;     /* Implementation details. */
    if (o_nmargs > mm->n_in) {
        char *end = NULL;
        long secs = strtol(o_margs[mm->n_in], &end, 0);
        if (!end || *end) {
            printf("Input parse error. Expected %d arg to be timeout of int type!\n",
                   mm->n_in + 1);
            dbus_message_unref(req);
            return 1;
        }
        if (secs < 1) secs = 1;
        if (secs > 3600) secs = 3600;
        timeout_ms = (int)secs * 1000;
        timeout_arg = (dbus_int32_t)secs;
    }
    dbus_message_iter_append_basic(&it, DBUS_TYPE_INT32, &timeout_arg);
    DBusError e;
    dbus_error_init(&e);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        oc->conn, req, timeout_ms, &e);
    dbus_message_unref(req);
    if (!rep) {
        if (dbus_error_is_set(&e)) {   /* Implementation details. */
            int rc = report_dbus_error_face(&e, mm->name);
            dbus_error_free(&e);
            return rc;
        }
        printf("Error: call failed\n");
        return 1;
    }
    if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        int rc = report_error_reply(rep);
        dbus_message_unref(rep);
        return rc;
    }
    DBusMessageIter rit;
    if (!dbus_message_iter_init(rep, &rit)) {
        printf("Ok\n");                /* Implementation details. */
        dbus_message_unref(rep);
        return 0;
    }
    /* Implementation details. */
    int i = 0;
    do {
        fputs("     ", stdout);
        if (i < mm->n_out && mm->out[i].name[0])
            printf("%s = ", mm->out[i].name);
        dumpfmt_value(stdout, &rit);
        fputc('\n', stdout);
        i++;
    } while (dbus_message_iter_next(&rit));
    dbus_message_unref(rep);
    return 0;
}

/* ---- --signal / --signal-oneshot ---- */
#define MAX_MON 32
static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t l = strlen(src);
    if (l >= cap) l = cap - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

static int mon_member(const char *member, char names[][64], int n)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], member)) return 1;
    return 0;
}

/* Implementation details. */
static int cmd_monitor_face(odin_client *oc, const odin_face *f, const char *spec,
                            int oneshot)
{
    char names[MAX_MON][64];
    int n_names = 0, all = 0;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n_names < MAX_MON;
         tok = strtok_r(NULL, ",", &save)) {
        if (!strcmp(tok, "all")) { all = 1; continue; }
        snprintf(names[n_names], sizeof names[n_names], "%s", tok);
        n_names++;
    }
    const face_signal *mon[MAX_MON];
    int n_mon = 0;
    if (all) {
        for (int i = 0; i < f->n_signals && n_mon < MAX_MON; i++)
            mon[n_mon++] = &f->signals[i];
    } else {
        for (int i = 0; i < n_names; i++) {
            const face_signal *sg = face_signal_find(f, names[i]);
            if (!sg) {
                printf("Failed to monitor signal \"%s\"\n", names[i]); /* Implementation details. */
                return 1;
            }
            mon[n_mon++] = sg;
        }
    }
    int count = all ? f->n_signals : n_names;
    if (count == 0) count = 1;

    for (int i = 0; i < n_mon; i++) {
        if (!mon[i]->wire) continue;   /* Implementation details. */
        char rule[512];
        snprintf(rule, sizeof rule,
                 "type='signal',sender='%s',path='%s',interface='%s',member='%s'",
                 oc->dest, oc->path, oc->dest, mon[i]->name);
        odin_add_match(oc, rule);
    }
    printf("Signal monitor is up and running (monitoring %d signals). "
           "Press Ctrl^C to exit application.\n", count);
    fflush(stdout);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    int got = 0;
    while (!g_stop) {
        dbus_connection_read_write(oc->conn, 300);
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(oc->conn)) != NULL) {
            if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
                const char *iface = dbus_message_get_interface(msg);
                const char *member = dbus_message_get_member(msg);
                if (iface && member && !strcmp(iface, oc->dest)) {
                    for (int i = 0; i < n_mon; i++) {
                        if (!mon[i]->wire || strcmp(mon[i]->name, member))
                            continue;
                        /* Implementation details. */
                        char anbuf[256];
                        const char *an[8] = { "", "", "", "", "", "", "", "" };
                        int n_an = 0;
                        snprintf(anbuf, sizeof anbuf, "%s", mon[i]->argnames);
                        char *sv = NULL;
                        for (char *t = strtok_r(anbuf, ",", &sv);
                             t && n_an < 8; t = strtok_r(NULL, ",", &sv))
                            an[n_an++] = t;
                        dumpfmt_signal_frame(stdout, member, an, n_an, msg);
                        got = 1;
                    }
                }
                /* Implementation details. */
            }
            dbus_message_unref(msg);
        }
        if (oneshot && got) {
            fflush(stdout);
            return 0;
        }
    }
    return 0;
}

static int cmd_monitor(odin_client *oc, odin_iface *ifc, const char *spec,
                       int oneshot)
{
    char names[MAX_MON][64];
    int n_names = 0, all = 0, monitor_props = 0;
    char propfilter[MAX_MON][64];
    int n_filter = 0;

    char buf[1024];
    snprintf(buf, sizeof buf, "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok && n_names < MAX_MON;
         tok = strtok_r(NULL, ",", &save)) {
        if (!strcmp(tok, "all")) { all = 1; continue; }
        snprintf(names[n_names], sizeof names[n_names], "%s", tok);
        n_names++;
    }

    /* Implementation details. */
    char mon[MAX_MON][64];
    int n_mon = 0;
    if (all) {
        for (int i = 0; i < ifc->n_signals && n_mon < MAX_MON; i++)
            copy_bounded(mon[n_mon++], 64, ifc->signals[i].name);
        monitor_props = 1;
    } else {
        for (int i = 0; i < n_names; i++) {
            size_t nl = strlen(names[i]);
            if (nl > 7 && !strcmp(names[i] + nl - 7, "Changed")) {
                monitor_props = 1;     /* Implementation details. */
                if (n_filter < MAX_MON) {
                    copy_bounded(propfilter[n_filter], 64, names[i]);
                    propfilter[n_filter][strlen(propfilter[n_filter]) - 7] = 0;
                    n_filter++;
                }
            } else if (n_mon < MAX_MON) {
                copy_bounded(mon[n_mon++], 64, names[i]);
            }
        }
    }
    int count = n_mon + (monitor_props ? 1 : 0);
    if (count == 0) count = 1;

    for (int i = 0; i < n_mon; i++) {  /* Implementation details. */
        char rule[512];
        snprintf(rule, sizeof rule,
                 "type='signal',sender='%s',path='%s',interface='%s',member='%s'",
                 oc->dest, oc->path, oc->dest, mon[i]);
        odin_add_match(oc, rule);
    }

    printf("Signal monitor is up and running (monitoring %d signals). "
           "Press Ctrl^C to exit application.\n", count);
    fflush(stdout);

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    int got = 0;
    while (!g_stop) {
        dbus_connection_read_write(oc->conn, 300);
        DBusMessage *msg;
        while ((msg = dbus_connection_pop_message(oc->conn)) != NULL) {
            if (dbus_message_get_type(msg) == DBUS_MESSAGE_TYPE_SIGNAL) {
                const char *iface = dbus_message_get_interface(msg);
                const char *member = dbus_message_get_member(msg);
                if (iface && member && !strcmp(iface, oc->dest) &&
                    mon_member(member, mon, n_mon)) {
                    const odin_signal *sg = odin_iface_find_signal(ifc, member);
                    const char *an[8];
                    for (int i = 0; i < 8; i++)
                        an[i] = (sg && i < sg->n_args) ? sg->args[i].name : "";
                    dumpfmt_signal_frame(stdout, member, an,
                                         sg ? sg->n_args : 0, msg);
                    got = 1;
                } else if (iface && member && monitor_props &&
                           !strcmp(iface, ODIN_IFACE_PROPERTIES) &&
                           !strcmp(member, ODIN_SIG_PROPCHANGED)) {
                    /* Implementation details. */
                    DBusMessageIter it, arr;
                    if (dbus_message_iter_init(msg, &it) &&
                        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_STRING &&
                        dbus_message_iter_next(&it) &&
                        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                        dbus_message_iter_recurse(&it, &arr);
                        int ok = dbus_message_iter_get_arg_type(&arr) ==
                                 DBUS_TYPE_DICT_ENTRY;
                        while (ok) {
                            DBusMessageIter ent;
                            dbus_message_iter_recurse(&arr, &ent);
                            const char *key = NULL;
                            dbus_message_iter_get_basic(&ent, &key);
                            dbus_message_iter_next(&ent);   /* Implementation details. */
                            int take = 1;
                            if (n_filter) {
                                take = 0;
                                for (int i = 0; i < n_filter; i++)
                                    if (key && !strcmp(propfilter[i], key)) {
                                        take = 1;
                                        break;
                                    }
                            }
                            if (take && key) {
                                printf("%sChanged:\n", key);
                                printf("     %s = ", key);
                                dumpfmt_value(stdout, &ent);
                                printf("\n\n");
                                got = 1;
                            }
                            ok = dbus_message_iter_next(&arr);
                        }
                    }
                }
            }
            dbus_message_unref(msg);
        }
        if (oneshot && got) {
            fflush(stdout);
            return 0;
        }
    }
    return 0;
}

/* Implementation details. */
static int cmd_acquire_wakelock(const char *bus)
{
    odin_client sys;
    DBusError e;
    dbus_error_init(&e);
    if (odin_client_init(&sys, "system", bus, &e) || !sys.conn ||
        !odin_name_has_owner(&sys, sys.dest)) {
        printf("system is not available!\n");     /* Implementation details. */
        dbus_error_free(&e);
        odin_client_free(&sys);
        return 1;
    }
    DBusMessage *m = dbus_message_new_method_call(
        sys.dest, sys.path, sys.dest, "acquire_wake_lock");
    const char *name = "odindb-send";
    dbus_message_append_args(m, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
    DBusMessage *rep = dbus_connection_send_with_reply_and_block(
        sys.conn, m, ODIN_DEFAULT_TIMEOUT_MS, &e);
    dbus_message_unref(m);
    int rc = 0;
    if (!rep) {
        printf("Error: %s\n", e.message ? e.message : "call failed");
        dbus_error_free(&e);
        rc = 1;
    } else if (dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR) {
        rc = report_error_reply(rep);
        dbus_message_unref(rep);
    } else {
        dbus_message_unref(rep);
        printf("Wakelock acquired!\n");
        printf("Press Ctrl^C to release (or run odindb-send -r)\n");
        fflush(stdout);
        signal(SIGINT, on_sigint);
        signal(SIGTERM, on_sigint);
        while (!g_stop) sleep(1);      /* Implementation details. */
    }
    odin_client_free(&sys);
    return rc;
}

static int cmd_release_wakelock(void)
{
    /* Implementation details. */
    DIR *d = opendir("/proc");
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!isdigit((unsigned char)de->d_name[0])) continue;
        char path[300];
        snprintf(path, sizeof path, "/proc/%s/cmdline", de->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        char cmd[4096];
        size_t n = fread(cmd, 1, sizeof cmd - 1, f);
        fclose(f);
        cmd[n] = 0;
        int pid = atoi(de->d_name);
        if (pid == getpid()) continue;
        int hit = 0;
        for (size_t i = 0; i < n;) {
            if (!strcmp(cmd + i, "--acquire-wakelock") ||
                !strcmp(cmd + i, "-w")) { hit = 1; break; }
            i += strlen(cmd + i) + 1;
        }
        if (hit) {
            if (kill(pid, SIGINT) == 0)
                printf("Successfully sent kill signal to pid %d\n", pid);
            else
                printf("Failed to send kill signal to pid %d\nreason: %s\n",
                       pid, strerror(errno));
        }
    }
    closedir(d);
    return 0;                          /* Implementation details. */
}

static int cmd_list_services(const char *bus)
{
    odin_client oc;
    DBusError e;
    dbus_error_init(&e);
    int ok = odin_client_init(&oc, "", bus, &e) == 0 && oc.conn;
    printf("Services:\n");
    printf("============\n");
    if (ok) {
        char **names = NULL;
        int n = odin_list_modules(&oc, &names, &e);
        for (int i = 0; i < n; i++) {
            printf("%s\n", names[i]);
            free(names[i]);
        }
        free(names);
        odin_client_free(&oc);
    }
    printf("============\n");
    dbus_error_free(&e);
    return 0;
}

/* ================ main ================ */
int main(int argc, char **argv)
{
    if (parse_options(argc, argv)) {
        print_usage(argv[0]);
        return 1;
    }

    if (f_help) { print_usage(argv[0]); return 0; }
    if (f_version) { printf("odindb-cli %s\n", CLI_VERSION); return 0; }
    if (f_showusage) { print_show_usage(); return 0; }
    if (f_amver) { printf("apps_messaging_version = %s\n", CLI_VERSION); return 0; }
    if (f_printcmd) {
        for (int i = 0; i < argc; i++)
            printf("%s%s", i ? " " : "", argv[i]);
        printf("\n");
        print_usage(argv[0]);
        return 1;
    }
    if (f_appmsg) {
        printf("Apps messaging version: %s\n", CLI_VERSION);
        print_usage(argv[0]);
        return 1;
    }

    if (f_liste || o_enum) {
        /* Implementation details. */
        if (o_enum)
            printf("Enum information is not available in the generic client "
                   "(no compiled-in HblmTypes tables): %s\n", o_enum);
        else
            printf("Enums are not available in the generic client "
                   "(no compiled-in HblmTypes tables).\n");
        return 1;
    }

    if (f_listsvc && !o_service) return cmd_list_services(o_bus);
    if (f_w) return cmd_acquire_wakelock(o_bus);
    if (f_r) return cmd_release_wakelock();

    /* Implementation details. */
    if (!o_service) {
        print_usage(argv[0]);
        return 1;
    }

    /* Implementation details. */
    odin_client oc;
    DBusError e;
    dbus_error_init(&e);
    if (odin_client_init(&oc, o_service, o_bus, &e)) {
        printf("Error: cannot connect to bus: %s\n",
               e.message ? e.message : "?");
        dbus_error_free(&e);
        return 1;
    }
    const odin_face *face = NULL;
    if (!f_any) {
        face = odin_face_find(oc.module);
        if (!face) {
            printf("No such service: %s\n", oc.module);
            odin_client_free(&oc);
            return 1;
        }
    }

    DBusMessage *ga = NULL;
    int sr = odin_client_start(&oc, &ga, &e);
    if (sr == -1) {
        /* Implementation details. */
        printf("No such service: %s\n", oc.module);
        odin_client_free(&oc);
        return 1;
    }
    if (sr == -2) {
        if (ga && dbus_message_get_type(ga) == DBUS_MESSAGE_TYPE_ERROR) {
            report_error_reply(ga);
            dbus_message_unref(ga);
        } else {
            printf("Error: %s\n", e.message ? e.message : "start failed");
        }
        dbus_error_free(&e);
        odin_client_free(&oc);
        return 1;
    }

    int rc = 0;
    odin_iface ifc;
    int have_ifc = 0;

    if (!face &&
        (f_listall || f_listm || f_listp || f_listsig || o_prop_value ||
         o_meth_name || f_memunreach || o_signal || o_signal1)) {
        /* Implementation details. */
        if (odin_introspect_parse(&oc, &ifc, &e) == 0)
            have_ifc = 1;
        dbus_error_free(&e);
    }

    if (f_listall) {
        if (face) {                     /* Implementation details. */
            fputs(face->pl, stdout);
            fputs(face->ml, stdout);
            fputs(face->lsig, stdout);
        } else {
            print_props_block(&ifc, 0);
            print_methods_block(&ifc, 0);
            print_signals_block(&ifc);
        }
    } else if (f_listp) {
        if (face) fputs(f_info ? face->pli : face->pl, stdout);
        else print_props_block(&ifc, f_info);
    } else if (f_listm) {
        if (face) fputs(f_info ? face->mli : face->ml, stdout);
        else print_methods_block(&ifc, f_info);
    } else if (f_listsig) {
        if (face) fputs(face->lsig, stdout);
        else print_signals_block(&ifc);
    } else if (f_getall) {
        if (face) {
            /* Implementation details. */
            for (int i = 0; i < face->n_props; i++) {
                const char *n = face->props[i].name;
                DBusMessageIter val;
                if (dict_find(ga, n, &val))
                    dumpfmt_prop_line(stdout, n, &val);
                else {
                    fputs(face->props[i].block ? face->props[i].block : "",
                          stdout);
                    fputc('\n', stdout);
                }
            }
            printf("\"Failed to find properties: all \"\n");
            rc = 1;
        } else {
            DBusMessageIter arr, ent;
            int ok = dbus_message_iter_init(ga, &arr) &&
                     dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_ARRAY;
            if (ok) dbus_message_iter_recurse(&arr, &ent);
            while (ok) {
                if (dbus_message_iter_get_arg_type(&ent) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter kv;
                    dbus_message_iter_recurse(&ent, &kv);
                    const char *k = NULL;
                    dbus_message_iter_get_basic(&kv, &k);
                    dbus_message_iter_next(&kv);
                    if (k) dumpfmt_prop_line(stdout, k, &kv);
                }
                ok = dbus_message_iter_next(&ent);
            }
            /* Implementation details. */
        }
    } else if (o_prop && o_prop_value) {
        if (face && !face_prop_find(face, o_prop)) {
            printf("No such property \"%s\" could be found.\n", o_prop);
            rc = 1;
        } else {
            /* Implementation details. */
            if (!have_ifc && odin_introspect_parse(&oc, &ifc, &e) == 0)
                have_ifc = 1;
            dbus_error_free(&e);
            if (have_ifc)
                rc = cmd_prop_set(&oc, &ifc, o_prop, o_prop_value);
            else {
                printf("Error: introspection failed for %s\n", oc.dest);
                rc = 1;
            }
        }
    } else if (o_prop) {
        if (face) rc = cmd_prop_get_face(face, ga, o_prop);
        else rc = cmd_prop_get(ga, o_prop);
    } else if (o_meth_name) {
        if (face) rc = cmd_method_face(&oc, face, o_meth_name);
        else if (have_ifc) rc = cmd_method(&oc, &ifc, o_meth_name);
        else {
            printf("Error: introspection failed for %s\n", oc.dest);
            rc = 1;
        }
    } else if (o_signal || o_signal1) {
        const char *spec = o_signal ? o_signal : o_signal1;
        int oneshot = o_signal ? 0 : 1;
        if (face) rc = cmd_monitor_face(&oc, face, spec, oneshot);
        else if (have_ifc) rc = cmd_monitor(&oc, &ifc, spec, oneshot);
        else {
            printf("Error: introspection failed for %s\n", oc.dest);
            rc = 1;
        }
    } else if (f_memunreach) {
        DBusMessage *m = dbus_message_new_method_call(
            oc.dest, oc.path, oc.dest, "triggerMemunreachableLog");
        dbus_uint32_t pid = (dbus_uint32_t)getpid();
        dbus_message_append_args(m, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID);
        DBusMessage *rep = dbus_connection_send_with_reply_and_block(
            oc.conn, m, ODIN_DEFAULT_TIMEOUT_MS, &e);
        dbus_message_unref(m);
        if (!rep) {
            dbus_error_free(&e);
            rc = 1;                    /* Implementation details. */
        } else {
            rc = dbus_message_get_type(rep) == DBUS_MESSAGE_TYPE_ERROR ? 1 : 0;
            dbus_message_unref(rep);
        }
    } else {
        print_usage(argv[0]);
        rc = 1;
    }

    if (ga) dbus_message_unref(ga);
    odin_client_free(&oc);
    return rc;
}
