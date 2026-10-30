# com.hasselblad.printer — General-Purpose IPP Printing Service D-Bus Interface Specification (v2.3)

Date: 2026-09-23 | This is the **single source of truth** shared by the GUI rebuild, printerctl,
and printer-service.
Every wire-protocol detail matches `analysis/dbus/REPORT_odindb_conventions.md` (reverse-engineering
findings for odindb/AppsMessaging).

> **v2.3 starvation prevention and complete restoration (P0/P1, NOTES §22)**:
> 1. `Start_discovery` now **returns on acceptance** (§3): returns an empty `as`; results are
>    published one at a time through `discovered_printers` notify. Discovery is complete when
>    `connection_state` leaves `discovering`; total budget is 60s.
> 2. `Disconnect_and_restore` returns on acceptance (§4b-1): restoration proceeds in the
>    background and completion is announced through `propertiesChanged`; client semantics remain
>    unchanged (idempotent and complete).
> 3. `printer_ip` is strictly an internal session cache and is always cleared on session restore (§5).

> **v2.2 SSID scanning and selection (`Scan_wifi` added in §3 + `scanned_networks` property added in §2)**:
> 1. New method `Scan_wifi() → as`: scan for Wi-Fi networks (blocking until complete; about
>    2–4s on camera; mock takes 2–2.5s, with slight differences between the two mock stubs;
>    camera `iw` path takes about 3s). List entries are `"SSID\tRSSI(dBm)\tSEC"` (tab-separated;
>    SEC=`wpa2`|`open`). Skip hidden networks (empty SSID); deduplicate SSIDs by strongest
>    signal; sort by descending RSSI; max 32 entries.
> 2. New `scanned_networks` property (`as`, read-only, notify): result of the most recent
>    Scan_wifi call, published when scanning completes. The GUI list can bind to it directly
>    (property and method use the same source).
> 3. **Sequence (updated connection flow; Connect_to_ap signature unchanged)**:
>    `Scan_wifi` (blocking return / `scanned_networks` notify) → GUI highlights a choice in
>    the scrolling list (selection provides SSID/SEC) → only WPA2 networks require a password
>    (open networks connect without one) → `Connect_to_ap(ssid, psk)` (pass an **empty string**
>    for open networks: empty PSK means open network and writes `key_mgmt=NONE` to wpa.conf;
>    1–7 character partial passwords still return `failed`).
>    Scanning is available only in `disconnected`: joining/joined/ready/discovering/scanning → `busy`.
> 4. Chip behavior on camera: scan setup matches Connect_to_ap (if wlan0 is missing, insmod+up;
>    if the native AP is running, first yield it legally with `power_on_wifi(false)` and remember
>    its original state); **keep the chip available after scanning** (the user is about to connect,
>    so rescans/connections use the fast path). If the user abandons the flow, the idle watchdog
>    (§4b-3 now includes disconnected-with-scan-retained state) or Disconnect_and_restore cleans up.
>    Session protection (§4b-2b) is held from scan acceptance. Prefer `iw dev wlan0 scan`;
>    if iw is unavailable, fall back to bundled wpa_supplicant + `wpa_cli scan/scan_results`
>    (parse both formats).

> **v2.1 Free crop/rotation/preview (3 methods added in §3 + edit parameter contract in §3b)**:
> 1. New methods: `Get_photo_preview(s,i,i)→s` (fast, cached preview), `Get_photo_info(s)→a{sv}`
>    (header probe), and `Preview_print(s,a{sv})→s` (WYSIWYG, exactly the same pipeline and
>    parameters as printing).
> 2. Extend opts for `Start_print_job` / `Preview_print` with edit keys:
>    `crop_x/crop_y/crop_w/crop_h` (d, normalized 0..1, defined on the **rotated** image) and
>    `rotation` (i, -1=EXIF auto / explicit override with 0/90/180/270).
> 3. Pipeline order is fixed (§3b coordinate definition): decode → rotate → crop → fit media
>    aspect ratio → HQ scale → sharpen → encode.
>    **`doPreview_print` output is byte-for-byte identical to the JPEG actually sent by
>    `Start_print_job` with the same parameters.**
>
> **v2 Generalization refactor (breaking change; finalized during the window before GUI integration)**:
> 1. Removed fixed-enum properties `paper_size`/`media_type`; retired integer semantics for
>    `color_mode`/`printer_quality`, replacing them with **native printer IPP keyword string**
>    properties `media`/`print_quality`/`color_mode`.
> 2. Added capability properties (`Select_printer` probes Get-Printer-Attributes
>    [all+media-col-database] on success, then notifies):
>    `supported_media`/`supported_qualities`/`supported_color_modes`/`supported_formats`/
>    `copies_max`/`caps_ready`/`media_size_info`/`document_format`.
> 3. Set validation uses capabilities: after `caps_ready`, media/print_quality/color_mode must
>    be members of the corresponding supported_* list or return `parametererror`;
>    `copies ≤ copies_max`.
> 4. `Start_print_job` opts `a{sv}` keys are IPP keywords (copies/media/print-quality/
>    print-color-mode/print-scaling), validated against capabilities before forwarding (§3).
> 5. Derive print target dimensions automatically: selected media physical size (media-col µm)
>    × printer-resolution-default (fallback to 300dpi if unavailable) = imgprep target pixels.
>    Discover all printer attributes; hard-code none.

---

## 1. Bus and Identifiers

| Item | Value |
|---|---|
| Bus | system bus, camera socket `unix:path=/dev/socket/dbus` (inject with `DBUS_SYSTEM_BUS_ADDRESS`; use session bus for WSL integration tests) |
| bus name (destination) | `com.hasselblad.printer` |
| object path | `/printer` |
| Business interface | `com.hasselblad.printer` (= bus name, following the odindb same-name convention) |
| Properties interface | `org.freedesktop.DBus.Properties` `Get(s)→v`, `Set(ssv)`, `GetAll(s)→a{sv}`; **the interface argument for Get/Set/GetAll must be `com.hasselblad.printer`** |
| Property-change signal | `org.freedesktop.DBus.Properties.propertiesChanged` (**lowercase p; nonstandard spelling**), path=`/printer`, signature `sa{sv}as`, arg2 (invalidated) is always an empty array |
| Method member | odindb name without the `do` prefix, **preserve snake_case** (e.g. `Connect_to_ap`) |
| Enum marshalling | i32 (`i`); from v2, native printer options are strings (`s`), while copies/preset remain `i` |
| Error names | `com.hasselblad.error.{failed\|parametererror\|busy\|canceled\|timeout}` (all lowercase, no separators); `failed` message may include backend details (e.g. format negotiation failure) |
| Introspect | Implement `org.freedesktop.DBus.Introspectable.Introspect` → hand-written XML (see §6) |

Equivalent GUI-side match rule (generated by QtDBus):
```
type='signal',sender='com.hasselblad.printer',path='/printer',interface='org.freedesktop.DBus.Properties',member='propertiesChanged'
```

---

## 2. Complete Property Table (v2.2: 24 properties, all notify; names use lowercase snake_case)

GetAll returns properties in the order shown below. `W` indicates Settable; all others are
read-only, and Set on a read-only property returns `parametererror`.

| # | Property | D-Bus signature | W | Initial value | Values / meaning |
|--:|---|---|---|---|---|
| 1 | `connection_state` | `s` | – | `"disconnected"` | `disconnected` \| `joining` \| `joined` \| `discovering` \| `ready` (state machine in §4) |
| 2 | `ssid` | `s` | ✓ | `""` | Target printer hotspot SSID (may be preset in config.ini) |
| 3 | `printer_state` | `s` | – | `"idle"` | `idle` \| `busy` \| `error` (printer's own state, populated by probing) |
| 4 | `printer_name` | `s` | – | `""` | Currently selected printer name (nonempty after successful Select_printer) |
| 5 | `printer_ip` | `s` | – | `""` | Printer IP (filled after selection) |
| 6 | `job_progress` | `i` | – | `0` | Percentage from 0–100, advances with job_status |
| 7 | `job_status` | `s` | – | `"idle"` | `idle` \| `preparing` \| `sending` \| `printing` \| `done` \| `failed` \| `canceled` (§4) |
| 8 | `copies` | `i` | ✓ | `1` | Number of copies, 1–copies_max; out of range → `parametererror` |
| 9 | `media` | `s` | ✓ | `""` | **Native printer media keyword** (e.g. `na_index-4x6_4x6in`); after probing = printer media-default; after caps_ready must be in `supported_media` |
| 10 | `print_quality` | `s` | ✓ | `"normal"` | `draft` \| `normal` \| `high` (keyword names for RFC 8011 print-quality 3/4/5); after probing = printer default, validated against capabilities |
| 11 | `color_mode` | `s` | ✓ | `"auto"` | **Native print-color-mode keyword** (`color`/`monochrome`/`auto`/`auto-monochrome`… as discovered); after probing = printer default, validated against capabilities |
| 12 | `preset` | `i` | ✓ | `0` | Application preset: 0=default, 1=ID photo, 2=instant photo (independent of printer capabilities; not sent to printer) |
| 13 | `document_format` | `s` | – | `""` | Actual document-format sent (after selection = `image/jpeg`; format negotiation in §2b) |
| 14 | `caps_ready` | `b` | – | `false` | Capability probe complete (true on successful Select_printer; reset to false and capability properties cleared on disconnect/reselection failure) |
| 15 | `copies_max` | `i` | – | `99` | copies-supported limit for this printer (99 before probing) |
| 16 | `supported_media` | `as` | – | `[]` | List of native media keywords that can be sent (extracted from media-supported + media-col) |
| 17 | `supported_qualities` | `as` | – | `[]` | Supported print-quality keyword subset (e.g. only `["normal","high"]`) |
| 18 | `supported_color_modes` | `as` | – | `[]` | Native print-color-mode-supported list |
| 19 | `supported_formats` | `as` | – | `[]` | document-format-supported list |
| 20 | `media_size_info` | `as` | – | `[]` | Each item `"keyword=WxHmm"` (e.g. `na_index-4x6_4x6in=101.6x152.4mm`), readable by people and software |
| 21 | `discovered_printers` | `as` | – | `[]` | Results of the most recent Start_discovery (entries `"name\tIP"`, tab-separated) |
| 22 | `scanned_networks` | `as` | – | `[]` | **v2.2** Most recent Scan_wifi result (entries `"SSID\tRSSI(dBm)\tSEC"`, SEC=`wpa2`\|`open`; descending RSSI, max 32; notify on scan completion) |
| 23 | `synced` | `b` | – | `true` | odindb compatibility (§7b) |
| 24 | `tx_active` | `b` | – | `false` | odindb compatibility (§7b); true while tx() executes |

Set wire format: `Properties.Set("com.hasselblad.printer", name, variant)`. A signature
mismatch returns `org.freedesktop.DBus.Error.InvalidArgs`; semantic errors (read-only/outside
capabilities/unknown name/empty string) return `com.hasselblad.error.parametererror`. Successful
Set returns an empty reply, then broadcasts `propertiesChanged` (single-property dict).

## 2b. Capability Discovery and Negotiation (v2: source of truth; no hard-coded printer attributes)

After a successful `Select_printer`, the service immediately executes Get-Printer-Attributes
(requested = `all` + `media-col-database`, with collections flattened), then:

1. **Format negotiation** (determines `document_format`):
   - If `image/jpeg` ∈ `document-format-supported`, send JPEG directly and set
     `document_format="image/jpeg"`.
   - Otherwise, if only raster formats are available (`image/pwg-raster`/`image/urf`/
     `cups-raster`), Select_printer returns `failed` with the message "This printer requires
     a raster backend, which is not yet supported."
   - Otherwise, return `failed` (format list does not include image/jpeg).
2. **Publish capabilities**: notify each property: `supported_media` (native keywords only;
   omit display names synthesized from media-col), `supported_qualities` (map enum 3/4/5 to
   draft/normal/high keywords), `supported_color_modes`, `supported_formats`, `copies_max`,
   and `media_size_info` (only media with advertised dimensions); then set `caps_ready=true`.
3. **Normalize defaults**: `media`=printer media-default, `print_quality`=print-quality-default,
   `color_mode`=print-color-mode-default (validate each against capability list; if not
   advertised, use the first list entry or normal); clamp `copies` to the maximum if needed
   and notify.
4. **Calculate dimensions automatically** (when Start_print_job runs): selected media's
   physical dimensions (media-col x/y-dimension µm, see `media_size_info`) ×
   `printer-resolution-default` (fallback to 300dpi if unavailable) = target pixels. Swap
   width/height if content (after EXIF rotation) and paper orientations differ; do not swap
   for square paper.
5. **Two-stage Set validation**: after `caps_ready`, Set values for `media`/`print_quality`/
   `color_mode` must be in the corresponding supported_* list (otherwise `parametererror`);
   overrides in `Start_print_job` opts are validated the same way.

Discovery (v2): mDNS queries both `_ipp._tcp.local` and `_universal._sub._ipp._tcp.local`
(AirPrint subtype). Supports TXT `rp=` paths other than `/ipp/print` (endpoint paths/ports are
stored in an internal service-side table; `discovered_printers` wire format is unchanged).
The direct-connection fallback `printer_ip` supports **multiple comma-separated endpoints
`host[:port]`** (for staging multiple printers).

---

## 3. Complete Method Table (member = name without the do prefix; multiple return values are flattened, not wrapped in a struct)

| C function | wire member | Input signature | Return signature | Semantics | Errors |
|---|---|---|---|---|---|
| `doConnect_to_ap` | `Connect_to_ap` | `ss` (ssid, psk) | `b` | Join the printer hotspot (WPA2-PSK). Returns immediately with whether the request was accepted (TRUE means async joining→joined; failure returns to disconnected and emits a signal). Camera execution layer (`wifi_backend=cam`) follows the sequence tested in NOTES §15: if the native AP is running, yield it legally with `power_on_wifi(false)` and remember its original state → insmod bcmdhd (only if wlan0 is missing; take ownership) → ifconfig up → bundled wpa_supplicant (-Dnl80211) → poll until wpa_state=COMPLETED (45s) → udhcpc. **PSK semantics**: supplied only through this method; stored at `/data/x2print/psk` and `/data/x2print/wpa.conf` (both chmod 0600); never a D-Bus property (no PSK read interface), command-line/process-table value, or log entry. | `busy` (already joining/joined/ready), `parametererror` (empty ssid), `failed` (invalid PSK, including 1–7 character partial passwords; from v2.2 **empty string = open network**, connect without password and write key_mgmt=NONE to wpa.conf; on camera: missing wpa binary/invalid arguments) |
| `doScan_wifi` | `Scan_wifi` | `` | `as` | **§20/v2.2 SSID scan and selection**: Wi-Fi scan, blocking until complete (camera uses `iw dev wlan0 scan`, falling back to bundled wpa_supplicant + `wpa_cli scan/scan_results` if iw is unavailable; mock returns 4 fake networks after 2.5s). Entries are `"SSID\tRSSI(dBm)\tSEC"` (SEC=`wpa2`\|`open`; skip hidden networks, deduplicate SSIDs by strongest signal, sort by descending RSSI, max 32); results are also written to `scanned_networks` and notified (GUI binds its list to this property). Camera chip setup matches Connect_to_ap (insmod+up if wlan0 is missing; legally yield and remember a running native AP); **keep the chip available after scanning** (user is about to connect; abandonment is handled by idle watchdog/Disconnect_and_restore). Hold session protection (§4b-2b) from scan acceptance. P0: real backend progresses in background ticks after acceptance (chip yield/protection/scanning all use subprocesses); service holds and sends the method reply on completion — **client-visible contract remains blocking until complete**, while other calls continue to be served. | `busy` (joining/joined/ready/discovering/scanning), `failed` (chip/binary unavailable or interrupted by disconnect) |
| `doDisconnect_and_restore` | `Disconnect_and_restore` | `` | `` | **Idempotent and complete** (§4b restoration protocol): any connection_state (including mid-join) and repeated calls converge to disconnected + camera baseline Wi-Fi. Camera sequence: cancel active jobs → wpa_cli terminate → ip addr flush → ifconfig down → rmmod bcmdhd (only if insmod was performed in this session; scan insmod is also cleaned up here) → **legal power synchronization** `power_on_wifi(original_state)` (if native AP was running when the session/scan began, pass true to restore it; otherwise pass false to turn it off legally and avoid desynchronizing the camera-system state machine) → clear printer_name/ip/discovered/caps. | `failed` (backend failure) |
| `doStart_discovery` | `Start_discovery` | `` | `as` | **Return on acceptance** (v2.3, same semantics as `Connect_to_ap`; P0 starvation prevention): accepted request returns **empty as**; `busy`/`failed` are synchronous errors. **Four-level cascade advances in the background tick state machine** (§2b/§16): L1 direct-connect to cached config `printer_ip` endpoints (comma-separated `host[:port]`, written back after prior discovery) → L2 mDNS (`_ipp._tcp` + `_universal._sub` AirPrint subtype, TXT `rp=` endpoint path) → L3 default-route gateway → L4 fast scan of wlan0's /24 on TCP:631 with IPP verification. Each endpoint/window runs in its own subprocess (abandon black-hole endpoint after 8s; **60s total budget**, then return discovered results). Publish results **one by one through `discovered_printers` notify** (GUI is property-driven); complete when `connection_state` leaves `discovering`. Continue serving Get/GetAll during discovery (at least one event-loop iteration every 2s). Write successful endpoint back to config `printer_ip`. | `busy` (joining/discovering), `failed` (not connected) |
| `doSelect_printer` | `Select_printer` | `s` (name) | `b` | Select a printer from the discovery list → populate printer_name/printer_ip, set connection_state→ready, and **immediately probe capabilities** (§2b: format negotiation + capability publication + default normalization, with propertiesChanged per property). name may be the full entry (`name\tIP`), a name, or an IP. | `parametererror` (not in discovered list), `failed` (never discovered/probe failed/raster backend required) |
| `doGet_photo_list` | `Get_photo_list` | `s` (path) | `as` | List jpg/jpeg files in the directory (max 64 items). path is a camera photo-album directory. | `parametererror` (empty path/directory does not exist) |
| `doStart_print_job` | `Start_print_job` | `as` (paths), `a{sv}` (opts) | `s` (job_id) | Submit a print job. **opts keys are IPP keywords** (v2): `copies(i)`, `media(s)`, `print-quality(s)`, `print-color-mode(s)`, `print-scaling(s)`; **§18 edit keys**: `crop_x/crop_y/crop_w/crop_h(d)`, `rotation(i)` (semantics/validation in §3b). Unspecified values use the corresponding property's current value (edit defaults to full frame + EXIF auto; note media/print-quality/print-color-mode use hyphens, property names use underscores); unknown keys are ignored. Validate against capabilities before forwarding (unsupported value → parametererror). job_id has form `job-<n>`. Only available when ready and no job is in progress. | `busy` (job in progress), `parametererror` (empty paths/unsupported opts value/invalid edit key), `failed` (not ready/capabilities not ready) |
| `doCancel_job` | `Cancel_job` | `` | `b` | Cancel the current job: TRUE=cancelled (locally set job_status→canceled→idle immediately; dispatch IPP Cancel-Job(0x0008) and perform a bounded confirmation loop of ≤3s in a background service subprocess; result is visible through rx event frame ev=2 and logs — method returns immediately without blocking); FALSE=no active job (not an error; see service log for reason). | — |
| `doGet_photo_preview` | `Get_photo_preview` | `s` (path), `i` (max_w), `i` (max_h) | `s` (jpeg_path) | **§18 Quick preview image** (photo grid/editor background): FAST pipeline (no sharpening), automatic EXIF rotation, maximum long edge ≤ max (max_w/max_h ≤0 or omitted=1024, limit 4096), preserve aspect ratio and shrink only (never enlarge). 3FR-aware: extract embedded preview (fseek/fread only the matching range). **Cached** at `/data/x2print/preview/<path-hash>_<w>x<h>.jpg` (WSL=`/tmp/x2print/preview/`); automatically invalidated when source mtime changes. Returns an **absolute path** for the GUI to load directly from the file. | `parametererror` (empty path/file does not exist), `failed` (decode failure/no 3FR preview) |
| `doGet_photo_info` | `Get_photo_info` | `s` (path) | `a{sv}` | **§18 Header probe** (does not decode pixels): `width(i)`, `height(i)` (stored dimensions), `orientation(i)` (EXIF 1..8, 0=none), `rotated_width/rotated_height(i)` (logical dimensions after EXIF), `has_jpeg(b)` (file itself is JPEG), `is_3fr(b)` (Hasselblad RAW; dimensions/orientation based on embedded preview), `file_size(x, i64)`. GUI swaps rotated width/height itself for explicit rotation. | `parametererror` (empty path/file does not exist), `failed` (not an image/no 3FR preview) |
| `doPreview_print` | `Preview_print` | `s` (path), `a{sv}` (opts) | `s` (jpeg_path) | **§18 WYSIWYG print preview**: exactly the same pipeline and parameters as `Start_print_job` (same crop/rotation/media/print-scaling/HQ/sharpening/encoding; shares `backend rj_prep_file`) — **output is byte-for-byte identical to the JPEG actually sent to the printer**. opts keys match Start_print_job (`copies` ignored); capability validation uses the same gate (media must be in supported_media). Writes `/data/x2print/preview/job_<n>_preview.jpg` (n increments per service; ring retains most recent 8); JPEG includes 300dpi density metadata (GUI can read print dimensions from the file). | `busy` (print job in progress), `parametererror` (empty path/unsupported opts/invalid edit key including crop window too small), `failed` (no printer selected/capabilities unavailable/image processing failed) |

odindb clients classify the 5 `com.hasselblad.error.*` types by name; message contains
human-readable text.

## 3b. Edit Parameter Contract (§18 crop/rotation; implement this in the GUI editor)

**Fixed pipeline order (implemented by imgprep)**:

```
decode (DCT scale) → rotate → crop (user-normalized rectangle) → center-fit media aspect ratio
      → HQ scale (Lanczos3) → sharpen (unsharp) → encode (q95 4:4:4 300dpi)
```

**Coordinate system (important)**: `crop_x/crop_y/crop_w/crop_h` are normalized 0..1 and
defined on the **rotated** image — the image shown in the GUI preview (`Get_photo_preview`
after automatic EXIF rotation or after applying explicit rotation). x/y origin is top-left,
with y increasing downward.

**rotation semantics**: `-1` = automatic EXIF Orientation (current behavior, default);
`0/90/180/270` = explicit clockwise rotation that **overrides** EXIF (does not stack with it):
recalculate from stored pixels and ignore the EXIF marker. GUI rotation control starts at -1
(follow EXIF); send the corresponding value after the user selects 90/180/270.

**Validation (reject at adaptor layer with `parametererror`)**:
- Four crop values must be 0..1, with `x+w≤1`, `y+h≤1` (1e-6 tolerance), `w>0`, `h>0`;
  reject NaN/Inf.
- rotation must be `-1/0/90/180/270`.
- Minimum crop size: source pixels in both dimensions must be ≥100px (`IMGPREP_MIN_CROP_PX`,
  calculated from rotated logical dimensions; preflight in `Preview_print`; if prep fails in
  `Start_print_job`, the job fails).
- Clamp out-of-bounds values (e.g. legal values where x+w slightly exceeds 1) to the edge
  (`w=1-x`); log the clamped window.

**Fit media after cropping**: center-crop the crop window to the target media aspect ratio
(cover/fill semantics, consistent with behavior when there is no user crop). The GUI should
use the same rule to preview the crop frame (including the centered inner crop band). Do not
crop for square media.

**WYSIWYG guarantee**: `Preview_print(path, opts)` output == the JPEG actually sent to the
printer by `Start_print_job([path], opts)` (same pipeline, same parameters, deterministic
encoder). Integration test `test_integration_wsl.sh (i10g)` asserts byte-for-byte equality.
The GUI only needs to use the same opts for preview and submission.

**printerctl examples** (crop_* decimals are automatically marshalled as double;
rotation/copies integers as i32):
```
printerctl call Get_photo_preview /mnt/.../B0000007.3FR 1024 1024
printerctl call Get_photo_info    /mnt/.../B0000007.3FR
printerctl call Preview_print     /mnt/.../B0000007.3FR crop_x=0.5 crop_y=0 crop_w=0.5 crop_h=1 rotation=90
printerctl call Start_print_job   /mnt/.../B0000007.3FR crop_x=0.5 crop_y=0 crop_w=0.5 crop_h=1 rotation=90
```

---

## 4. State Machine

```
connection_state:
  disconnected ──Connect_to_ap(ss)──▶ joining ──association + DHCP succeed──▶ joined
       ▲                                │failure (signal emitted)     │
       │                                ▼                         │Start_discovery()
       │                           disconnected        discovering
       │                                                          │scan complete (results→discovered_printers)
       │                            ready ◀──Select_printer(s)────┘
       └────────────────Disconnect_and_restore() (available in any state)─────┘
                                   (also cancels jobs and clears printer_name/ip)

job_status (orthogonal to connection_state; meaningful only in ready state):
  idle ──Start_print_job──▶ preparing ─▶ sending ─▶ printing ─▶ done ─(2s)─▶ idle
                 │              │           │           │
                 │        Cancel_job: canceled ─(1s)─▶ idle
                 └─────── any stage fails ──▶ failed ─(2s)─▶ idle
  job_progress: preparing stays at 0; sending 10→40 (starts at 10, then 10+30·k/n);
                printing 40→100 (mock 2%/sec);
                done/failed/canceled retain their final value; reset to 0 on return to idle.
  printer_state: idle⇄busy (busy while job is active); backend error=error.
```

## 4b. Cleanup/Restore Protocol and Idle Watchdog (v2 finalization, mandatory)

The system must **reliably restore state** when printing finishes or the GUI closes the
printing screen; there is also a fallback if the GUI crashes or forgets to call restore:

1. **GUI-initiated close**: `Disconnect_and_restore()` is idempotent and complete — safe to
   call repeatedly; calls in any connection_state (including mid-join/scan/discovery) converge
   to `disconnected` + baseline camera Wi-Fi (no residue: no wlan0 address/no supplicant/
   self-loaded module unloaded).
   **Returns on acceptance (P0)**: immediately SIGKILL in-flight subprocesses (insmod/udhcpc/
   probe); the restore sequence (reverse Wi-Fi cleanup → legal power sync → release protection
   → cleanup) progresses in the background. Completion is visible via `propertiesChanged`
   (`connection_state→disconnected`); repeated calls are accepted idempotently.
2. **Legal power synchronization** (`wifi_backend=cam`): detect native AP state at session
   start (hostapd running or wlan0=192.168.2.1 means the user is using it). If running, first
   yield it through `com.hasselblad.system.power_on_wifi(false)` and remember its state. At the
   end of restoration, return to that state (if originally on, restart AP via
   `power_on_wifi(true)`; if off, turn it off legally).
   Never bypass the state machine by directly running rmmod; that desynchronizes the
   camera-system wifi_power internal state from reality (one cause of Phocus Bluetooth failing
   to bring up the hotspot; NOTES §16.4).
   Perform legal synchronization only on the first restore (repeated disconnects must not
   restart the AP repeatedly).
2b. **Session/standby protection** (NOTES §19, user expectation: "keep the screen on while
   printing, restore it when done"): from Connect_to_ap acceptance until restore completes,
   the service maintains two protection layers — ① display layer (primary): temporarily set
   `screen_idle_timeout` to 86400 (setting the property itself wakes the screen; save original
   value to /data/x2print/screen_timeout.saved for crash recovery), then restore the original
   value **after** legal power sync. When screen is off, camera-system may "accept but ignore"
   power_on_wifi (`Wifi settings ignored when wifi is blocked`, the explanation for the
   misdiagnosed "AP burned out" in §18.5); the screen must be on for the power request to run.
   ② Kernel layer: wakelock `x2print-session` (/sys/power/wake_lock) prevents deep autosleep
   suspend (2h setting; does not block FSM standby or direct writes to /sys/power/state).
   On startup, boot_init clears stale locks and restores screen timeout from saved state.
3. **Idle watchdog**: config `idle_restore_min` (default 5, 0=disabled). If joined/ready,
   no print job is active, and there are **no business-method calls** (including Properties.Set;
   pure Get/GetAll polling does not count, so an open but idle GUI still times out) for longer
   than this many minutes, automatically run the same restore path as Disconnect_and_restore
   and broadcast `propertiesChanged` (`connection_state→disconnected`). Job completion counts
   as recent activity (restart idle timer at job completion); do not trigger while joining or
   discovering; active operations (job/blocking scan) use their own timeout safeguards (e.g.
   120s total job timeout).
   **v2.2 extension**: also time the disconnected state after Scan_wifi when the chip is left
   available but the user abandons the connection (scan-retained state). On expiry, clean up
   automatically (remove chip/terminate scan supplicant/restore power state) so "scan but do
   not connect" leaves no wlan0 up, self-loaded module, or yielded native AP behind.
4. **Power-cycle assertion**: `service.sh verify` (installed as /data/x2print/bin/x2_verify.sh,
   driven by `printerctl syswifi on|off`) runs `power_on_wifi(true)` → repeatedly assert
   wlan0=192.168.2.1 + hostapd running + SSID configured → `power_on_wifi(false)` → assert
   hostapd stopped and wlan0 is DOWN. Camera-side QtDBus slot signature is strict: exactly one
   bool, no timeout trailing arg from the odindb wrapper (odindb-cli `-m` adds one and fails
   with `signature 'bi'` mismatch, so use printerctl's dedicated path). Starting with §19,
   verify overrides `screen_idle_timeout=86400` before running (keeps screen on and wakes it,
   restores original value afterward) and holds its own wakelock, so assertions remain valid
   in screen-off/standby states. STA residue at the end is informational only (not asserted).

---

## 5. config.ini (/data/x2print/config.ini, minimal hand-written INI, no third-party dependencies)

If absent at first launch, the following defaults are written automatically. Successful Set of
a writable property is persisted immediately; save again on SIGTERM.

```ini
; x2print printer-service configuration (auto-generated, v2)
[printer]
ssid =
copies = 1
media =
print_quality = normal
color_mode = auto
preset = 0
```

- Parsing rules: `;`/`#` comments; `[section]` (section name is informational only; keys are
  global and unique); split `key = value` at the first `=`; trim surrounding whitespace;
  tolerate CRLF.
- Ignore unknown keys (forward compatibility); use defaults and warn on invalid values.
- `printer_ip` is an **internal session cache**: service writes it after discovery (next scan
  tries direct L1 fast path); **always clear it on session restore (Disconnect_and_restore/
  watchdog/failure rollback)** — do not edit it manually (it will be cleared). See §4b for
  `idle_restore_min`. PSK is not stored in config.ini; it is stored separately at
  `/data/x2print/psk` (0600).

## 5b. Logging

`--log <file>` overrides the default `/data/x2print/log/service.log` (create directory
automatically; truncate and rewrite above 256 KB); also write to stderr. Levels:
`--loglevel debug|info|warn|error`, default info.

---

## 6. Introspect XML (hand-written static string; for dbus-send/busctl validation)

Key points: node declares 4 interfaces (business + Properties + Introspectable + Peer);
propertiesChanged is declared under the **Properties** interface and the member is lowercase.
Full XML is in `INTROSPECTION_XML` in `src/printer-service/adaptor.c` (matches this table; item 4
of `test_wsl.sh` validates it with python3 xml).

---

## 7. odindb Compatibility (Interoperability Facts)

1. **The current `odindb-send` cannot drive this service directly**: its
   `ServiceFactory::createService` hard-codes 16 module names
   (gui/system/usbhost/storage/testd/camera/error/phocus/prodinfo/upgrade/exmcu/settings/
   tcphost/hosttransfer/wltcphost/suc) and has no `printer`. Choose one interoperability option:
   - (a) Patch odindb-send factory to add one name (single-point change) — **implemented**:
     length-preserving renamed copy `tools/odindb-send-printer` (usbhost→printer); corresponding
     server compatibility surface is in §7b.
   - (b) `/system/bin/dbus-send` (dbus.conf allows all) — used by test_wsl.sh for acceptance.
   - (c) Bundled `printerctl` (pure libdbus client, odindb-send-style subset) — primary
     acceptance tool before GUI rebuild.
2. **Alternative acceptance test**: `printerctl monitor` fully parses `sa{sv}as` and decodes/
   prints from the GUI's perspective (QtDBus strictly matches slot signatures). If it can read
   the output, the GUI can too.
3. Camera GUI (camera-gui) currently has no generated PrinterProxyDbus class; during GUI
   rebuild, implement a QML/C++ proxy following this document (wire protocol is compatible).
4. Do not reuse the existing 16 module names (avoid conflicts with real services); the
   msg2dbus 30303 remote channel has no printer handler (local calls are unaffected).

## 7b. odindb-send-printer Compatibility Surface (5 Items)

The patched factory CLI `odindb-send-printer` (output of `tools/patch_odindb_send.py`, a
35-byte length-preserving usbhost→printer rename; see
`analysis/dbus/REPORT_odindb_send_printer_patch.md`) drives this service through the
**fixed call surface of the usbhost wrapper**. The server implements **only** the following
5 items (the CLI's moc surface prevents it from reading/calling any other properties/methods):

| Item | Wire format | Semantics |
|---|---|---|
| Method `tx` | `tx(y message_len, ay message)` → empty reply | **Raw debug byte pipe**: log received hex with LOGD (all bytes up to 64 B; truncate and mark longer messages), then echo Ok; also send the received bytes **unchanged in an `rx` frame (echo mode)** for CLI loopback tests. Not forwarded to backend; may later support IPP/command passthrough (payload protocol is custom within `ay`). |
| Signal `rx` | `rx(y, ay)`, path=/printer | Async event pipe. Two triggers: ① on each print-job `job_status` change (backend_real), send an **X2P event frame** (`"X2P"` magic + u16 ev_type (1=job_status/2=cancel result) + u32 job_id + ASCII summary, fields big-endian; see `rj_emit_rx` comment in `backend_real.c` for frame format); ② `tx()` echo (unchanged bytes). Emit function `prn_adaptor_emit_rx()` (adaptor.h). |
| Signal `testrx` | `testrx(y, ay)` | Reserved for self-test; `prn_adaptor_emit_testrx()` (no internal trigger in this release). |
| Property `synced` | `b`, read-only, notify, initially `true` | Compatibility placeholder (service has no link-sync concept; always true). |
| Property `tx_active` | `b`, read-only, notify, initially `false` | Synchronously flips true→false while `tx()` runs (single-threaded; emits `propertiesChanged` for each transition). |

- `synced`/`tx_active` are at the end of the §2 property table (GetAll items 23/24), accessed
  through the same Properties interface as other properties.
- **Use `printerctl` or `/system/bin/dbus-send` for full property/method access** —
  `odindb-send-printer` `-p` recognizes only properties declared in the wrapper moc table
  (synced/tx_active), and `-m` supports only `tx`.

Device example (`/data/x2print/odindb-send-printer`, see patch report §5):

```sh
B=/data/x2print/odindb-send-printer
$B -s -l                       # List services: printer appears (usbhost is gone)
$B -s printer -p synced        # Read property → synced = true
$B -s printer -p tx_active     # → tx_active = false
$B -s printer -m tx 3 41,42,43 # tx(y=3, ay=41 42 43) → Ok; LOGD logs hex, then echoes rx(3, 41 42 43)
$B -s printer --signal rx      # Listen for rx(y,ay): receives tx echo frames and print-job X2P event frames
$B -s printer --signal all     # Observe propertiesChanged when tx_active toggles
```

Equivalent WSL regression commands (do not require patched copy; test_wsl.sh group (6)):
`dbus-send --print-reply=literal --dest=com.hasselblad.printer /printer com.hasselblad.printer.tx byte:3 array:byte:65,66,67`;
`Properties.Get ... string:synced`.

## 8. CLI (printerctl, pure libdbus; session bus by default, system bus when DBUS_SYSTEM_BUS_ADDRESS is set)

```
printerctl [--bus ADDR] get <prop>            # Single property (Properties.Get)
printerctl [--bus ADDR] set <prop> <value>    # Marshal according to §2 type table (auto-detect s/i properties)
printerctl [--bus ADDR] call <method> [args...]  # Auto-marshal according to method table; for Start_print_job,
                                              #   trailing k=v pairs go into a{sv} (number=i, otherwise=s)
printerctl [--bus ADDR] caps                  # Print all capabilities for selected printer in human-readable form
printerctl [--bus ADDR] monitor               # Subscribe to all service signals and decode as readable output
printerctl [--bus ADDR] --get-all             # All properties + current values
```
Examples:
```
printerctl get connection_state
printerctl set copies 3
printerctl call Scan_wifi                  # §20: scan nearby networks (entries "SSID\tRSSI(dBm)\tSEC")
printerctl get scanned_networks            # Same-source property (notify broadcast on scan completion)
printerctl caps                            # Capability overview (media/size/quality/color/format/copy limit)
printerctl call Connect_to_ap "Mi Photo Printer 1S [A1B2]" 12345678
printerctl call Start_discovery
printerctl call Select_printer "Mi Photo Printer 1S [A1B2]"
printerctl get supported_media             # Available after selection (published by probe)
printerctl set media oe_photo_3x3in        # Accepted only if present in supported_media
printerctl call Start_print_job /tmp/a.jpg /tmp/b.jpg copies=2 print-quality=high print-scaling=fit
printerctl call Cancel_job
printerctl monitor
```
§18 additions (v2.1): `call Get_photo_preview <path> <max_w> <max_h>`, `call Get_photo_info <path>`,
`call Preview_print <path> k=v...` (same k=v format as Start_print_job; `crop_*=decimal` marshalled
as d, `rotation/copies=integer` as i, all others as s).

## 9. Service Process

- Binary `/data/x2print/bin/printer-service` (aarch64, built with NDK r25b, linked to device
  `/system/lib64/libdbus.so`); same directory contains `printerctl`, `selftest`, `selftest_img`,
  `wpa_supplicant`, `wpa_cli`, `odindb-cli`, `udhcpc.sh`, `x2_verify.sh`. Use root-level
  `service.sh install|start|stop|status|log|ctl|sh|verify` for production deployment/control
  (adb channel; see README).
- Options: `--config <path>` (default /data/x2print/config.ini), `--mock` (force mock backend;
  automatically fall back to mock with warning if real backend symbols are absent),
  `--bus <address>` (explicit bus address), `--log <file>`, `--loglevel <lvl>`,
  `--daemon` (fork to background), `--help/--version`.
- `service.sh start` is idempotent (skip if already running); `stop` sends SIGTERM for clean
  exit (and saves config).
- Backend seam `src/printer-service/backend.h`: function-pointer table
  (connect/join/restore/discover/select/photo_list/print/cancel/tick); phase 2 print core
  (`src/net/`) implements the same table as a replacement.
