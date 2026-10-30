/* dumpfmt.h — QVariant-style output formatting (matches observed output of the original)
 * true/false; strings printed raw (empty string <empty>); lists [a, b];
 * byte arrays as contiguous lowercase hex; dicts as Item {k = v, ...};
 * i32 enum bits include names in the original (requires a table; generic version prints numbers only)
 */
#ifndef ODIN_DUMPFMT_H
#define ODIN_DUMPFMT_H

#include <dbus/dbus.h>
#include <stdio.h>

/* Print the value pointed to by iter (variant has already been recursed into) */
void dumpfmt_value(FILE *out, DBusMessageIter *v);

/* One "name = value" line (-p read / --get-all / signal argument) */
void dumpfmt_prop_line(FILE *out, const char *name, DBusMessageIter *v);

/* Signal frame: "member:" + one "     name = value" per argument + blank line
 * (original uses five-space indentation) */
void dumpfmt_signal_frame(FILE *out, const char *member,
                          const char *const *argnames, int n_names,
                          DBusMessage *args);

/* D-Bus signature → Qt type name (int/QByteArray/QStringList...) */
const char *dumpfmt_qttype(const char *sig);

#endif
