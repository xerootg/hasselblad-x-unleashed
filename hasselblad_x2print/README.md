# x2print — Production Deployment Overview

x2print is a general-purpose IPP wireless printing service for the X2D II camera (`com.hasselblad.printer`; see the D-Bus contract in
`dbus/INTERFACE.md`). For production deployment, use the root-level `service.sh` (run Git Bash on a PC to control the camera over adb):

Connection lifecycle (GUI view): `Connect_to_ap(ssid,psk)` → joining → joined (on camera:
insmod bcmdhd → wpa_supplicant→COMPLETED → udhcpc) → `Start_discovery` (three-stage
cascade: direct connection using cached `printer_ip` → mDNS → gateway/subnet scan;
verify via IPP, then write back to config) → `Select_printer` (probe and publish capabilities)
→ `Start_print_job` → after completion, `Disconnect_and_restore` (idempotent; restores the
camera's baseline Wi-Fi state without leaving residue). The PSK is stored only at
`/data/x2print/psk` (0600), never as a D-Bus property. An idle watchdog provides recovery
if the GUI crashes (`idle_restore_min` config, default 5 minutes; 0 disables it). Full build,
regression, and on-camera acceptance records are in `NOTES.md` (§14 wpa cross-compilation,
§15 end-to-end camera test, §16 production hardening).
