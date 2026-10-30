# hblProductInfo "Fatal, Unknown product" reverse-engineering notes

Target: `/system/bin/camera-gui` (aarch64 PIE, full symbols, NDK r16b, statically linked Qt/libc++)
Symptom: manual startup with LD_PRELOAD aborts inside `Version::hblProductInfo::hblProductInfo()`:
`hblversion.cpp:0353 Version::hblProductInfo::hblProductInfo(): Fatal, Unknown product`

## 0. Summary of findings

- **The data source is a file: `fopen("/proc/cmdline", "rb")`**, not property / getenv / DBus.
- Its contents are searched with statically linked libc++ `std::regex` for **`hw_version=([0-9]{1,2}).([0-9]{1,2}).([0-9]{1,2})`** (dots are unescaped and match any character).
- Group 1 = product id, group 2 = hw revision, converted to decimal by `strtol`.
- **Valid product ids: 4..14** (constructor checks `(unsigned)(id-4) <= 10`).
- The hardware kernel cmdline (observed in crashlogs dmesg) contains **`androidboot.hw_version=06.04.01.04`** → id=6 ("X2D II 100C", i.e. splashboot=hb722), hwRev=4.
- **Workaround: export `fopen` from the LD_PRELOAD .so and return a fake FILE* from `fmemopen()` for `"/proc/cmdline"`**, containing at least `hw_version=06.04.01.04`. All reads use stdio (fopen→fread→fclose, all interceptable through the PLT), and fmemopen's FILE* is fully compatible.

## 1. Symbols and call chain (all tombstone addresses match)

| Symbol | Address |
|---|---|
| `Version::hblProductInfo::instance()` | 0x179f0c8 (tombstone #06: +0x28 = 0x179f0f0 = `bl ctor`) |
| `Version::hblProductInfo::hblProductInfo()` C1/C2 | 0x179f540 (#05: +316 = +0x13C = 0x179f67c = `bl QMessageLogger::fatal`) |
| `Version::hblProductInfo::m_instance` (BSS) | 0x2b4ec88 |
| `GuiObjectImpl::joystick_move_focus() const` | 0x110b5b8 (#07: +36 = 0x110b5dc = `bl instance()`) |
| `producttype_get_product_id` | 0x17a3288 |
| `producttype_get_hw_revision` | 0x17a4068 |
| `productInfo()` (anonymous ns, producttype.cpp) | 0x17a32e8 |
| `str2int(char const*)` | 0x17a40c8 |
| `gCmdLinePath` (static std::string, producttype.cpp) | 0x2b4f178 |
| `productInfo()::prodType` / `::hwRev` | 0x2b4f190 / 0x2b4f198 (each int+1 ok byte: 0x2b4f194 / 0x2b4f19c) |
| `gMutex` | 0x2b4f150 |
| `_GLOBAL__sub_I_producttype.cpp` | 0x409e78 |

```
joystick_move_focus()                        0x110b5b8
  GuiProxies::camera()->vtable[+1208]()      Returns false early in startup
  → hblProductInfo::instance()               0x179f0c8   m_instance==NULL → new(8) → ctor
    → hblProductInfo::hblProductInfo()       0x179f540
        w19 = producttype_get_product_id()   0x17a3288  (gMutex + productInfo)
        w0  = producttype_get_hw_revision()  0x17a4068
        if ((u32)(w19 - 4) > 10) → warning("Unknown product type: N") + fatal("Fatal, Unknown product")
        else object = {int productId; int hwRev;}   // 8 bytes, stp w0, w19
```

The object is only 8 bytes: `[0]=hwRev(int)`, `[4]=productId(int)` (`stp w0, w19, [x20]`).
`Version::hardwareID()` (0x179f120) reuses the same m_instance singleton—fixing construction fixes all paths.

### Key constructor disassembly (0x179f540)

```asm
179f570: bl   17a3288 <producttype_get_product_id>
179f574: mov  w19, w0
179f578: bl   17a4068 <producttype_get_hw_revision>
179f57c: sub  w8, w19, #0x4          ; id - 4
179f580: cmp  w8, #0xa               ; > 10 ?
179f584: b.hi 179f5b4                ; → warning + fatal
179f588: stp  w0, w19, [x20]         ; Store {hwRev, productId}
...
179f5e8: bl   QMessageLogger::warning()      ; "Unknown product type: " << id (QDebug)
...
179f67c: bl   29b094 <QMessageLogger::fatal(char const*, ...) const>   ; ← Crash site +316
        ; fatal format string 0x1cd1787 = "Fatal, Unknown product"
        ; QMessageLogger context: file=0x1cd147d ".../appscommon/hblversion.cpp"
        ;                       func=0x1c898e2 "Version::hblProductInfo::hblProductInfo()"
        ;                       line=353 (0x161, in the d0 doubleword at 0x1d196d0)
```

## 2. Full data-source path: productInfo() (0x17a32e8)

`producttype_get_product_id`: `gMutex.lock()` → `productInfo()` → `w0=low 32 bits=id`, `w1=validity flag`; returns 255 if invalid. `get_hw_revision`: high 32 bits, returns -1 if invalid.

`productInfo()` (parses on first call; subsequent cache hits at 0x2b4f190/198 + the ok byte at 0x2b4f19c directly return `(hwRev<<32)|prodType`, w1=1):

```asm
17a347c: add  x1, x1, #0xe3e      ; x1 = "rb"        (0x1cd1e3e)
         mov  x0, x21             ; x0 = gCmdLinePath.c_str()
17a3488: bl   29a3f0 <fopen@plt>  ; fopen("/proc/cmdline", "rb")
17a348c: str  x0, [sp, #344]      ; Store in basic_filebuf::__file_
17a3490: cbz  x0, 17a33c8         ; NULL → "Failed to open" + path (cerr) + failbit
17a349c: ...  operator new(0x800) + memset + istream::read(buf, 2048)
17a34d0: cmp  w8(gcount), #1 ; < 1 → "Failed input-read: " + strerror(errno)
         ; Then std::string content(buf, gcount); fclose@plt
17a36e8: Regex literal 0x1cd1d9e = "hw_version=([0-9]{1,2}).([0-9]{1,2}).([0-9]{1,2})"
         basic_regex::__parse + regex_search(content, m)
17a3920: tbz  w19, #0 → "No or too few matches found"   ; !matched
17a3934: cmp  size, #2 → same as above                   ; m.size() < 2
17a3944: m[1] → std::string → str2int → str w0,[0x2b4f190]; ok byte [0x2b4f194]
         ; !ok → "Failed str conversion, prodType: " + m[1]
17a3d04: m[2] → str2int → [0x2b4f198] / ok [0x2b4f19c]
         ; !ok → "Failed str conversion, version, hwRev: " + m[2]
17a3320: Success: w1=1; x0 = (hwRev<<32) | prodType
```

`str2int` (0x17a40c8) = `strtol(s, &end, 10)`; `end==s` or errno==ERANGE ("Over/under-flow in: " / "No valid number found in: ") → failure. Returns `x0 = (ok?1:0)<<32 | value`.

`gCmdLinePath` is statically initialized by `_GLOBAL__sub_I_producttype.cpp` (0x409e78) to the SSO literal **`/proc/cmdline`** (0x1cd1460, 13 characters; first byte 26=13<<1 is libc++'s SSO length encoding). No setter in nm references this global—**env/property cannot change it at runtime**. (The `Version::readCmdline/setCmdlineFile` functions and cache 0x2b4ec90 in hblversion.cpp form a separate mechanism for firmware-version display, unrelated to product identification.)

### Read path confirmed to be entirely stdio
Statically linked libc++ `basic_filebuf<char>::underflow()` (0x17a4b68) internally uses `bl fread@plt` (0x17a4c88/0x17a4d1c), opens with `fopen@plt`, and closes with `fclose@plt`. **No raw read()/openat() calls**—this means:
1. Our existing openat hook cannot intercept this chain (bionic fopen calls the openat syscall directly inside libc, bypassing the interposable openat symbol);
2. But `fopen` is a PLT dynamic symbol, so exporting a same-name function via LD_PRELOAD interposes it.

## 3. hblversion.cpp:353 semantics + product ID table

Line 353 logic (reconstructed from referenced strings):

```cpp
Version::hblProductInfo::hblProductInfo() {
    unsigned id = producttype_get_product_id();     // Group 1 of hw_version= in /proc/cmdline
    unsigned rev = producttype_get_hw_revision();   // Group 2
    if (id - 4 > 10 /*unsigned*/) {                 // Valid: 4..14; all others (including 255) are fatal
        qWarning() << "Unknown product type:" << id;
        qFatal("Fatal, Unknown product");           // ← Line 353
    }
    productId = id; hwRevision = rev;
}
```

`Version::productName(Version::ProductID)` (0x179eb30, same `id-4≤10` check + jump table @0x26797c8, entry=offset/4, 0x22→default):

| id | productName (0x179eb7c family) | Notes |
|---|---|---|
| 4 | "X2D 100C" | |
| 5 | "CFV 100C" | |
| **6** | **"X2D II 100C"** | **This camera (hw_version=06.04.01.04)** |
| 7–11 | (valid but "Unhandled model:") | Reserved |
| 12 | "X1D II 50C" | |
| 13 | (valid but Unhandled) | |
| 14 | "CFV II 50C" | |

modelNumber-related strings: `HB722`, `X2DII-SSD`, `X2DII-CFe`, `X2D-100C-SSD`, `CFV-100C-CFe`, etc. (0x1cd1640–0x1cd16c0).
Hardware dmesg (crashlogs): `Kernel command line: ... splashboot=hb722 ... androidboot.hw_version=06.04.01.04 ...`
→ Regex matches `hw_version=06.04.01` (`hw_version=` is a suffix of `androidboot.hw_version=`; the search is unanchored and matches directly), id=6, hwRev=4, construction succeeds.

**Why our launch crashes**: outside the service environment, failure at any of the three stages (open → read ≥1 byte → regex matches and both groups convert to numbers) → `productInfo()` returns w1=0 → get_product_id returns 255 → fatal. Startup stderr should show the corresponding precursor (QMessageLogger::warning also prints one of "Failed to open"/"Failed input-read"/"No or too few matches found", identifying the exact failure point).

## 4. Spoofing approach (LD_PRELOAD, -nostdlib, extern declarations)

**Hook point: `fopen` (libc.so dynamic symbol, PLT interposition). Return a fake `/proc/cmdline`.**

```c
/* extern declarations, resolved to libc.so at both link time and runtime */
extern void *fdopen(int fd, const char *mode);
extern void *fmemopen(void *buf, unsigned long size, const char *mode);

/* Fake cmdline matching the hardware (minimum content: hw_version=NN.NN.NN, NN₁∈[04..14]) */
static const char FAKE_CMDLINE[] =
    "console=secS0 androidboot.hardware=eagle2 splashboot=hb722 "
    "mp_state=production androidboot.hw_version=06.04.01.04 "
    "androidboot.slot_suffix=1 quiet\n";
/* Note: NUL termination required; use size=sizeof-1 for fmemopen */

void *fopen(const char *path, const char *mode) {
    if (str_eq(path, "/proc/cmdline"))            /* Handwritten comparison, -nostdlib */
        return fmemopen((void*)FAKE_CMDLINE, sizeof(FAKE_CMDLINE)-1, "r");
    /* Forward other paths: mode's first character r/w/a → openat + fdopen */
    int flags = (mode[0]=='r') ? 0 : (mode[0]=='w') ? 0x241/*O_WRONLY|O_CREAT|O_TRUNC*/ : 0x441;
    long fd = my_openat(AT_FDCWD, path, flags | 0x80000/*O_CLOEXEC*/, 0644); /* Use existing syscall wrapper */
    if (fd < 0) return 0;
    return fdopen((int)fd, mode);
}
```

Key points:
- **Value**: `06.04.01.04` → id=6 (X2D II 100C) / hwRev=4, matching hardware; subsequent branches such as `hasJoystick` behave natively. To spoof another model, change group 1 (4/5/6/12/14 have names; 7–11/13 are unnamed but valid).
- **fmemopen compatibility**: subsequent `istream::read(2048)` uses `fread@plt` (verified in underflow), gcount=len>0 ✓; final `fclose@plt` is valid for a fmemopen FILE* ✓. bionic has supported fmemopen since API 23 (this camera is Android 27).
- **Do not** hook `__system_property_get`/`getenv`/DBus—this chain never touches them.
- **Cannot** hook `producttype_get_product_id` itself: it is an internal main-program symbol (0x17a3288, not PLT), called by direct `bl`, so LD_PRELOAD cannot intercept it.
- Cache effect: after the first success, `productInfo()` stores results in a static cache (0x2b4f190/198); later reads in the same process no longer access the file. The fopen hook only needs to be active before the first joystick/GUI event (LD_PRELOAD is present from process startup, satisfying this naturally).
- Alternative (for a more "real" file): in the `fopen` hook, write fake content to a real temporary file (openat O_CREAT), then return it through `fdopen`—uses the kernel file path throughout, compatible with any stdio implementation details.
- Another root-cause investigation path: if the stderr precursor is "Failed to open", the existing openat hook/environment has broken /proc (e.g. /proc is missing in the mount namespace)—either pass `/proc/*` through directly or use fmemopen with the fake content above.

## 5. joystick_move_focus trigger conditions (optional defense; conclusion: not a root-cause fix)

```asm
110b5c8: bl   GuiProxies::camera()
110b5d0: ldr  x8, [x8, #1208]   ; camera proxy virtual method (a setting, false early in startup)
110b5d4: blr  x8
110b5d8: tbnz w0, #0, 110b5e8   ; true → skip product query
110b5dc: bl   instance()        ; ← Crashes here (first singleton construction)
110b5e0: bl   hasJoystick()
110b5e4: tbz  w0, #0 → return 0
110b5e8: bl   GuiProxies::system() (+1176) …
```

Any joystick event dispatched during startup reaches this code; once constructed successfully, `m_instance` is cached permanently. Preventing the trigger (swallowing joystick events) is unreliable—other paths such as `hardwareID()` construct the same singleton. **The proper fix is to supply the correct /proc/cmdline.**

## 6. Key string address reference

| Address | Content |
|---|---|
| 0x1cd147d | `/home/build/jenkins/default/hardware/hbl/cam_fw/linux/libs/appscommon/hblversion.cpp` (QMessageLogger file) |
| 0x1c898e2 | `Version::hblProductInfo::hblProductInfo()` (QMessageLogger func) |
| 0x1cd1770 | `Unknown product type: ` (warning prefix) |
| 0x1cd1787 | `Fatal, Unknown product` (fatal format string) |
| 0x1cd1460 | `/proc/cmdline` (gCmdLinePath default) |
| 0x1cd1d9e | `hw_version=([0-9]{1,2}).([0-9]{1,2}).([0-9]{1,2})` (regex, dots unescaped) |
| 0x1cd1e3e | `rb` (fopen mode) |
| 0x1c8afc3 | `Failed to open` (14 bytes, printed on fopen failure) |
| 0x1cd1e2a | `Failed input-read: ` (19 bytes) |
| 0x1cd1e0e | `No or too few matches found` (27 bytes) |
| 0x1cd1dd0 | `Failed str conversion, prodType: ` (32 bytes) |
| 0x1cd1dfc | `Failed str conversion, version, hwRev: ` |
| 0x1cd1e41 | `No valid number found in: ` (str2int) |
| 0x1cd1e5c | `Over/under-flow in: ` (str2int) |
| 0x1cd1600 region | `X1D II 50C`/`CFV II 50C`/`CFV 100C`; 0x1cd08df `X2D 100C`; 0x1cd08e8 `X2D II 100C`; `HB722`, `X2DII-SSD`, etc. |
