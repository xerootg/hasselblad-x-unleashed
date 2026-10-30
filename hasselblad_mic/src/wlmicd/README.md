# wlmicd — DJI Mic 2 wireless microphone service daemon (injection-free wl service stack)

Standalone single-process daemon replacing the phase 1 frida injection engine (miccore). All
interaction with bt_bsa_app uses only the two dsock channels it **already creates natively in main()**—no hooks,
no LD_PRELOAD, no ptrace, no in-process calls, and no memory writes. **No changes to bt_bsa_app.**

```
TX <===== SPP (private AA/Wl/DUSS protocol) =====> bt_bsa_app (100% native behavior)
                                              │
        ┌─────────────────────────────────────┤
        │ /dsock/57012 uplink dgram            │ /dsock/57011 downlink dgram
        │ {01, seq8, len16be, bd[6], payload}  │ {01, seq8, len16be(=10+plen), bd[6], raw DUSS≤125B}
        │ payload = N×125B audio units        │ bt_bsa_app natively wraps AA-ver1 frames for SPP
        │  (6B{btclk u32le,intra u16le}+119B Wl)│
        ▼                                      │
  ┌========================= wlmicd =========================┐
  │ 57012 RX thread → unified parser (AA ver0/ver1/raw blocks) │
  │   Wl 0x10 audio → LC3plus decode (ETSI, PLC) → mic.pcm / WAV │
  │   Wl 0x19 status → TLV → battery/charging/mute/recording  │
  │ 57011 TX thread (RQ) → 0x57 status query (10s) / 0x55 keepalive (1Hz) │
  │   / conn-OK refresh / sleep notice 0x54{00,20}            │
  │ bsa daemon socket client → scan/pair/unpair/RSSI(0x582)/  │
  │   DgClose(0x4B4)/TmDisconnect(0x583)                       │
  │ Three interfaces (INTERFACE.md v3 unchanged): mic_ctl / mic_status.json / │
  │   mic.pcm—no GUI changes                                 │
  └===========================================================┘
```

## Why this is a "complete wl service stack"

The Action 5 product stack has three processes: bt_bsa_app (transport) + dji_network (state machine/frame construction) +
libwirelessmicservice (protocol parsing/LC3/status aggregation). The latter two are absent
from Hasselblad firmware (`feature_bt_audio=0`); wlmicd combines their responsibilities in one process,
attached to the native dsock channels:

| Stack component | Product implementation | wlmicd implementation |
|---|---|---|
| SPP connection | TX HFP→SPP special-case handling / WMS cmd 0x13 | **Native behavior retained** (TX initiates the handshake; WMS channel 90000 requires source=80000, occupied by dji_network and inaccessible externally → connect=armed-passive) |
| Version/time synchronization handshake | Built into bt_bsa_app | **Native behavior retained** (runs natively after bt_audio.cfg is written; see below) |
| Uplink audio→decode→PCM | wl service receives on 57012 | Same: bind /dsock/57012, strip 125B units, LC3plus→PCM FIFO/WAV |
| Status TLV→GUI | wl service 0x39a20 | Same: Wl 0x19 → TLV (battery=T10/charging=T05/mute=T01/recording=T06) → status JSON |
| Stream maintenance (0x55/0x56) | dji_network responds passively | 1Hz 0x55 (replicates the RSSI event path) + native 0x58 handshake; hardware tests show TX fw316 needs only these two |
| 0x57 status exchange | dji_network responds | **Active 10s polling** (correct 16+6n format; btclk from cached audio-unit headers) |
| conn-OK chain (0x65/03+0x54/10) | dji_network five-message sequence | Periodic refresh (60s, configurable) + kickstart command |
| Sleep notice (0x54{00,20}) | feature_bt_audio_stop | Sent before disconnect + 200ms grace period |
| Scan/pair/unpair | WMS→bt_bsa_app | Direct bsa daemon socket connection (0x0C8/0x12C/0x12D/0x131/0x134, available to any client) |

## Native behavior boundaries (confirmed by disassembly, fw316, all evidence-backed)

- **Uplink routing** `_bt_audio_analysis_v1@0x21a48`: audio units→57012;
  for TX→camera set12, **0x58/0x65 are handled locally; the rest (0x15/0x17/0x57/0x65-ACK) are silently dropped**;
  DUSS addressed to non-camera destinations→WMS cmd 0x05(80000, no receiver). ⇒ TX control frames do not appear on 57012;
  all wlmicd maintenance paths are **actively periodic** (a reactive responder remains in the parser, triggered only when DUSS
  actually arrives in a 57012 payload—offline/future firmware).
- **Downlink gate** `_bt_audio_recv_thread@0x241e0`: dgram bd must match
  a bt_audio.cfg slot (memory structure @0x800e90), and slot+0x32(ver_done)≠0.
  ver_done is set when TX responds to the camera's 0x65/04 version synchronization (@0x226c8→0x22770),
  while the camera's version-sync transmission is itself gated on cfg existence by is_master_addr(read_config).
  ⇒ **Once bt_audio.cfg is written, the next SPP connection naturally completes version synchronization and opens the gate**
  (cutover mode maintains this file automatically, equivalently replicating the product's update_config).
- CRC16 correction: the phase 1 engine's duss_crc16 has a degenerate transcription error (different inputs produce the same value;
  TX tolerated the incorrect CRC). wlmicd uses the standard lookup-table method; all six verified reference cases (actual captured frame
  58/02→03EE, report templates 0x54→1464 / 0x55→CB49 / 0x57→2A99 / ccode→E08D /
  ver-sync→B9C8) are reproduced byte for byte. **The bug is documented in miccore.c but has not been redeployed.**

## Running

```
adb shell /data/mic/wlmicd -d        # Daemon (-d background; -v also logs to stderr)
adb shell /data/mic/micctl status    # Interface unchanged (after cutover)
# Offline self-test (binds no dsock/socket; can coexist with a live engine)
adb shell /data/mic/wlmicd --probe57                     # Verify 0x57/0x55 byte templates
adb shell "cd /data/mic && ./wlmicd --replay spp_audio_capture.bin \
           --expect-frames 28829 --tlv-synth --wav"      # End-to-end replay
```

### Compatibility phase (compat, auto-detected by default)

Probe /data/mic/mic_ctl at startup: if the phase 1 engine responds → compat=1—
- Side-channel interfaces: wlmicd_ctl / wlmicd_status.json (leave mic_ctl/mic_status.json/mic.pcm untouched)
- Hard-disabled: keepalive_055/responder/auto_connect/linkup_refresh/manage_bt_audio_cfg/
  poll_057 are all off (avoid duplicate responses with the engine; native code simply drops 0x57 polls while the gate is closed)
- Only: **consume the 57012 mirror** (audio decoding + observation of four TLV fields, writing side-channel status) + RSSI polling

### Cutover (mic.cfg `compat=0`, or automatic after the engine exits)

1. Add `compat=0` to mic.cfg (or wait for automatic detection after the engine disappears)
2. wlmicd takes over the three canonical interfaces + writes bt_audio.cfg
3. **The next TX reconnection** completes native version synchronization → the 57011 gate opens → 0x57 polling/conn-OK
   refresh/kickstart all take effect
4. Remove frida (no further attaches); no injection is needed after bt_bsa_app next restarts naturally

## mic.cfg configuration management (wlmicd has exclusive write access)

Forward-compatible dual-mic format (`bd=` is the legacy spelling, always retained in sync with bd0=):

```
bd=e4:7a:2c:f2:53:69      # mic0 (legacy line; the old engine recognizes only this)
bd0=e4:7a:2c:f2:53:69     # mic0 (canonical spelling)
bd1=                      # Reserved for mic1 (written to bt_audio.cfg's second slot)
auto_connect=1  keepalive_055=1  passive=1     # ← Four keys shared by both sides
compat=0  poll_057=1  poll_057_sec=10  responder=1
linkup_refresh_sec=60  manage_bt_audio_cfg=1  rssi_poll=1  adopt_bd=1
```

- **Successful pair → automatically rewrite bd=/bd0= with the new address** (preserve all other lines, replace atomically;
  disable with adopt_bd=0). Streams from unconfigured bd addresses are counted but not decoded (logs suggest pairing to adopt the address).
- **Live reload**: the hk thread checks mtime every second and rereads on change (ctl `reload` forces it)—
  no restart needed for changes to poll_057_sec/keepalive_055/bd, etc.
- **Ownership during migration**: both sides read the same file; only wlmicd writes it. Ownership of the four shared keys—
  `bd/auto_connect` are adopted and maintained by wlmicd (on pair); `keepalive_055` belongs to the engine during
  compatibility mode (wlmicd hard-disables it regardless of the file value), then to wlmicd after cutover; `passive`
  is recognized only by the engine (wlmicd is inherently passive by design and ignores it). The old engine's parser skips
  unknown lines, so keys appended by wlmicd are harmless to it.

## Offline validation record (2026-10-03, hardware /data/mic)

| Test case | Result |
|---|---|
| `--probe57/--probe55` byte templates | **PASS** (byte-for-byte match with REPORT_ctrl_frames §4, including corrected CRC16) |
| 3.6MB capture replay (5766 dgram) | frames **28830/28830, drops 0** (recovers all blocks missed when feeding the engine raw data; only 8,245B differ from the reference WAV, all at the reference's own frame-concealment points) |
| Synthetic Wl 0x19 TLV injection (real 57012 dgram path) | **battery=82 charging=1 mute=0 recording=1 all populated** |
| AA frame live dump (feeddump, including trailing DUSS frames) replay | Parser+responder trigger correctly: TX 0x65/00 → conn-OK pair (27B+21B, correct CRC) |
| 22.7MB full stress test (35,963 dgram) | frames **179,815/179,815, drops 0**, 17.6s (>10× real-time headroom) |

## Known limitations

- `connect` is armed-passive (waits for TX's native handshake); it cannot actively DgOpen—
  limited by WMS command-interface source validation; service resumes as soon as TX wakes and reconnects.
- Cutover disconnect uses DgClose(handle=bt-dg-socket<N> index)+TmDisconnect;
  handle ownership when two mics are online has not been refined (no impact on the current single-mic case).
- ccode(0x65/02) is not sent: fw316 TX streams without ccode (verified in a PASSIVE session),
  and its native trigger (WMS cmd 0x19) is inaccessible externally.
