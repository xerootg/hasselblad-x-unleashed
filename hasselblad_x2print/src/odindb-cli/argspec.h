/* argspec.h — CLI argument → D-Bus type encoding/decoding
 * Supported: i/s/b/d/x (64-bit)/y/u/n/q/t, ay (comma-separated hex bytes), as (JSON array),
 *            a{sv} (JSON object), v (automatic), a{i,u,x,d} (comma-separated numbers, generic extension)
 * Bitmask enums: "E_A|E_B" syntax → i32 bitwise OR; numeric components are ORed directly.
 * E_-prefixed names return an explicit error because the generic version does not include
 * the HblmTypes table (see differences in README).
 */
#ifndef ODIN_ARGSPEC_H
#define ODIN_ARGSPEC_H

#include <dbus/dbus.h>
#include <stddef.h>

/* "E_A|E_B|1|2" → int32; returns 0/-1 (reason is written to err) */
int argspec_parse_bitset(const char *s, dbus_int32_t *out, char *err, size_t errsz);

/* Encode a CLI string as one complete D-Bus value according to sig and append to iter; 0/-1 */
int argspec_encode(DBusMessageIter *iter, const char *sig, const char *s,
                   char *err, size_t errsz);

/* Heuristically infer the type when no signature is provided; returns a static signature string */
const char *argspec_guess(const char *s);

/* Detect machine-readable JSON containers in value text, in addition to dumpfmt */
int argspec_is_json_object(const char *s);
int argspec_is_json_array(const char *s);

#endif
