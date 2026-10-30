> **Purpose:** odindb-cli is a test port and one-to-one compatibility implementation of
> odindb-send (comparison infrastructure: tools/run_odindb_conformance.sh and comparison of
> original and reimplemented binaries under qemu). **It is not built or deployed with the
> product** (service.sh install does not include it; acceptance testing uses the camera's
> original /system/bin/odindb-send). This directory and build_odindb.py/build_wsl_odindb.sh
> are retained as reverse-engineering records.

# odindb-cli — One-to-One Generic Reimplementation of Hasselblad odindb-send

A command-line client written from scratch in C11 + libdbus, replacing the binary-patching
approach in `tools/patch_odindb_send.py` for the factory CLI `odindb-send`
(`/system/bin/odindb-send`, v10.00.09.77).

**Default behavior matches the original:** built-in known-module table = original 16 modules
+ printer. Slot-surface data (property subsets/defaults, method signatures, signal filters, and
list output) was collected verbatim from qemu runs of the original (`faces.c`, 610 properties,
all methods and signals). The core command matrix's stdout and exit codes match 100%
(`tools/odindb_cli_conformance.md`). The `--any` escape flag explicitly enables arbitrary
module names via generic introspection, for future services.

## Syntax (faithfully reproduces original usage)

```
odindb-cli -s -l                                # List services (built-in table: original 16 + printer)
odindb-cli -s <mod> -m -l                       # List methods (literal slot-surface text)
odindb-cli -s <mod> -p -l                       # List properties
odindb-cli -s <mod> -p -l -i                    # Property details (Name/Type)
odindb-cli -s <mod> -m -l -i                    # Method details (In/Out args, expanded enums)
odindb-cli -s <mod> --list-all                  # Properties → methods → signals (original order)
odindb-cli -s <mod> --get-all                   # Properties + current values (slot subset; original quirks included)
odindb-cli -s <mod> -p <prop>                   # Read property (in slot; GetAll cache)
odindb-cli -s <mod> -p <prop> <value>           # Write property (type from introspection)
odindb-cli -s <mod> -p <prop> -- -60            # Negative value
odindb-cli -s <mod> -p <prop>:<numeric-mask>    # Bit read → 0/1
odindb-cli -s <mod> -m <method> [args...]       # Call method (encode by slot signature; omit do prefix)
odindb-cli -s <mod> -m <method> ... 30          # Extra final argument = timeout seconds (default 25)
odindb-cli -s <mod> --signal a,b|all            # Listen continuously (slot signal filter; Ctrl^C to exit)
odindb-cli -s <mod> --signal-oneshot <name>     # Exit after receiving one frame
odindb-cli -s <mod> --trigger-memunreachable    # Call triggerMemunreachableLog(u)
odindb-cli --acquire-wakelock / --release-wakelock   # Through com.hasselblad.system
odindb-cli -b system|session|none ...           # Select bus (default system)
odindb-cli -e <enum> / --list-enums             # Enums (unavailable; see differences)
odindb-cli --any -s <new-module-name> ...        # Escape flag: arbitrary module name (generic introspection)
```

Names outside the known-module table (gui system usbhost storage testd camera error phocus
prodinfo upgrade exmcu settings tcphost hosttransfer wltcphost suc **printer**) are rejected
like the original (`No such service: X`, rc=1; check the local table after connecting, matching
the original bus traffic). `--any` bypasses this check and uses D-Bus introspection.

### Argument encoding (CLI text → D-Bus type; see argspec.c)

| Target type | CLI text form | Example |
|---|---|---|
| `i`/`n` | Decimal; **bitmask enum** `a\|b\|...` bitwise-ORs numeric components | `60`, `1\|2\|4` |
| `u`/`q`/`x`/`t`/`d` | Decimal/floating point | `42`, `3.14` |
| `s` | Raw string | `/cfe/939HASBL/B0000881` |
| `ay` | Comma-separated hexadecimal bytes | `41,42,43` |
| `as` | JSON array (original style) | `["One","Two"]` |
| `a{sv}` | JSON object (original style); values s/b/i/d | `{"lensModel":"CF 38","av":60}` |
| `v` | Infer type from text form | — |

Method signatures come from the slot table by default; `--any` uses D-Bus introspection.
Property-write types come from introspection (equivalent to the report's "probe first when
type is uncertain"). Per odindb wrapper convention, method calls include timeout as the final
wire argument (`int32`; -1=default 25s; `-m ... 30` sets seconds explicitly).

## Differences from the Original

1. **printer is our 17th table entry** (the original has 16 slots and no printer slot, a
   physical difference): `-s -l` adds `printer` after the original 16 names; printer slot
   data comes from the patched copy (= usbhost slot).
2. **Enums:** the original includes the HblmTypes table. This implementation explicitly errors
   for `--list-enums`/`-e`; bitmask syntax remains (`1|2` is valid), but `E_` name components
   cannot be parsed. When an enum property has **no value from the service**, print the slot
   table's literal default block (verbatim original output, including `E_Xxx(0)` form); when
   the service **provides a value**, print the i32 number (the original prints `E_Xxx(n)` and
   requires a compile-time enum table, which cannot be reproduced here).
3. **Property write marshalling:** the original always sends `variant string` (QtDBus server
   converts it); this implementation sends a **typed variant** based on introspection (the
   x2print printer-service Set validator rejects the original's marshalling). Both QtDBus
   endpoints are compatible. Writing a property through the slot path requires one
   introspection request to determine writability/type (the original checks its wrapper's
   compile-time WRITE flag with no bus request).
4. **Generic `--any` path** (extension): introspection-driven; `--get-all` shows every actual
   service property and returns rc=0 (does not reproduce original moc filtering/quirks);
   `<prop>Changed` subscriptions use propertiesChanged filtering, and even signals absent from
   introspection are subscribed. The default path does not use this.
5. **Exit cleanup:** original RemoveMatch ×4 (including one bogus rule that causes a bus error);
   this implementation removes only rules actually added. Housekeeping-only difference.
6. **`--apps-messaging-version`/`-a`/`-v`:** prints this implementation's version
   (x2print-1.0.0), not the original git hash.
7. **Exit codes:** `com.hasselblad.error.{failed,parametererror,busy,canceled,timeout}` →
   {1,2,3,4,255}; other D-Bus errors → 1. Error text goes to stdout (the original also writes
   all output there, as observed). `-p <prop> -i` single-property details are not reproduced.

## Three Build Targets

| Target | Command | Artifact | Notes |
|---|---|---|---|
| Android (camera) | `python build_odindb.py` (Windows) | `build/aarch64/odindb-cli` | NDK r25b aarch64-linux-android21, links device `inc/lib/libdbus.so` (uses camera `/system/lib64` at runtime); push to `/data/x2print/`. Camera system bus is `/dev/socket/dbus` (automatic fallback, or set `DBUS_SYSTEM_BUS_ADDRESS`/`DBUS_SOCKET`). |
| Native WSL | `wsl -d Ubuntu-24.04 -- bash build_wsl_odindb.sh` | `build/wsl/odindb-cli` | x86_64 + system libdbus-1; used for comparison/debugging. |
| Compatibility test | `wsl -d Ubuntu-24.04 -- bash tools/run_odindb_conformance.sh` | `/tmp/odinconf/` + `tools/odindb_cli_conformance.md` | Compare original binary (16 native modules) + patched copy (printer) against this implementation across stdout/rc/bus traffic. |

All builds use `-std=c11 -Wall -Wextra -Werror` with zero warnings.

## Modules

- `main.c` — CLI entry point: argument parsing (full usage), gating + slot-path dispatch,
  generic path.
- `faces.c/.h` — **Generated files (do not edit by hand):** slot-surface data for 17 modules.
  Regenerate with `wsl ... bash tools/collect_faces.sh` (collect original output under qemu) →
  `python3 tools/gen_odindb_faces.py /tmp/faces <x2print-root>`. Repeat after firmware/build
  upgrades (data is intentionally tied to the original build).
- `client.c/.h` — libdbus wrapper: templated triplet, QtDBus-equivalent startup sequence
  → GetAll(this module) → GetAll(linkstatus), verified wire-level sequence; GetAll/Get/Set/
  method calls/Introspect/introspection parsing/error-name→exit-code mapping.
- `argspec.c/.h` — CLI argument→D-Bus type encoding/decoding (table above) + bitmask enums +
  JSON subset.
- `dumpfmt.c/.h` — QVariant-style output (values/property lines/signal frames/Qt type names).

## Design Notes (Lessons Learned)

- Property-change signal member has lowercase p: `propertiesChanged` (wire-protocol critical).
  Subscription match rule exactly matches the string generated by original QtDBus (verified by
  comparison).
- Property reads do not send `Get` (original generated code has no xref); use GetAll cache for
  wire-level matching. `odin_get` is only a fallback if GetAll fails.
- Iterate dict entries with `get_basic` + `next` directly; do not `recurse` into the entry
  (observed SIGSEGV with libdbus 1.14).
- On an error reply, `send_with_reply_and_block` returns NULL and sets DBusError. Handle the
  original three-line error style (`"member" : "msg"`/msg/Error) in this branch.
- `--signal all` subscribes only to wire signals in the slot table; propertiesChanged is
  silently ignored (the original does not print that frame either).
