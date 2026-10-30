/* wifi_cam.h — Camera-side Wi-Fi STA orchestration (NOTES §15 camera-tested path,
 * backend_real execution layer)
 *
 * P0 starvation-prevention refactor (NOTES §22): run all long commands
 * (insmod/udhcpc/wpa_cli/iw/killall/rmmod) as fork+exec background subprocesses, poll with
 * waitpid(WNOHANG) in tick, and SIGKILL on timeout.
 * The join/scan/restore state machines perform at most one nonblocking poll per step, so the
 * main event loop can run between every operation (D-Bus dispatch and watchdog remain active)
 * and disconnect/cancel can interrupt immediately.
 *
 * Join state machine (advanced once per backend tick):
 *   wifi_cam_join_start(ssid, psk)  accepted (0) / invalid args (-1)
 *   wifi_cam_join_step()            0=joined / 1=in progress / -1=failed (details in log)
 *   wifi_cam_join_phase()           Human-readable phase name (for logs/monitoring)
 *
 * STA sequence (fully verified on camera, X2D II + bcmdhd IAPSTA):
 *   1. Check baseline: does /sys/class/net/wlan0 exist? If yes, module is already loaded
 *      and not owned by us (never rmmod it); otherwise insmod /system/lib/modules/bcmdhd.ko
 *      (we own it; record module.owned marker; rmmod during restore/next-start boot_cleanup)
 *      — retry 3 times, 3s apart (SDIO race).
 *   2. ifconfig wlan0 up
 *   3. Write /data/x2print/wpa.conf (0600) → bundled static wpa_supplicant
 *      -B -Dnl80211 -i wlan0 -c ... (`wl join` was conclusively ruled out; see NOTES §14/§15).
 *   4. Poll `wpa_cli -p /data/x2print/wpasock status` for wpa_state=COMPLETED (45s budget).
 *   5. Run `busybox udhcpc -i wlan0 -s <event-script> -n -q -t 4` (obtain dynamic hotspot lease).
 *
 * Scan state machine (§20; also advanced by tick):
 *   wifi_cam_scan_begin()           accepted (0) / environment error (-1)
 *   wifi_cam_scan_step(&out,&n)     0=complete (*out=malloc'd list, entries
 *                                   "SSID\tRSSI(dBm)\tSEC"; caller frees)
 *                                   1=in progress / -1=failed
 *   Chip setup is shared with join; keep chip available after scan (watchdog/restore cleans up).
 *
 * Restore state machine (reverse order; returns on acceptance, tick advances to completion):
 *   wifi_cam_restore_begin()        Abort in-flight subprocess/state machine, then start restoration
 *   wifi_cam_restore_step()         1=in progress / 0=complete (idempotent; always 0 after completion)
 *
 * Interrupt:
 *   wifi_cam_abort()                Set on disconnect/cancel: SIGKILL in-flight subprocess
 *                                   immediately; reset join/scan machine in place
 *                                   (restore_begin starts the restore machine).
 *
 * Residue cleanup (P1②, NOTES §22):
 *   wifi_cam_boot_cleanup()         One-time service-start cleanup (synchronous, idempotent):
 *                                   kill stale bundled wpa_supplicant by pid file + pgrep,
 *                                   rmmod only when module.owned marker exists, flush wlan0
 *                                   address only when marker exists (never disturb native AP
 *                                   wlan0). Log all actions.
 *
 * Restore sequence (reverse order): wpa_cli terminate → killall → ip addr flush dev wlan0 →
 *   ifconfig wlan0 down → rmmod bcmdhd (only if we insmod'ed it; clear ownership marker).
 *
 * PSK security: store only in /data/x2print/psk and wpa.conf (both chmod 0600);
 *   never in D-Bus properties / command line (visible via ps) / logs.
 */
#ifndef PRN_WIFI_CAM_H
#define PRN_WIFI_CAM_H

/* Join state machine: after start is accepted, tick calls step repeatedly until 0/-1 */
int  wifi_cam_join_start(const char *ssid, const char *psk); /* 0=accepted, -1=invalid args/environment */
int  wifi_cam_join_step(void);  /* 0=joined, 1=in progress, -1=failed */
const char *wifi_cam_join_phase(void);                       /* Current phase name (static string) */

/* §20 SSID scan/selection (advanced by tick): result entries "SSID\tRSSI(dBm)\tSEC",
 * SEC=wpa2|open; skip hidden networks, deduplicate SSIDs by strongest signal, sort by
 * descending RSSI, max 32 items. */
int  wifi_cam_scan_begin(void);                       /* 0=accepted, -1=environment error */
int  wifi_cam_scan_step(char ***out, int *n);         /* 0=complete, 1=in progress, -1=failed */

/* Restore camera baseline Wi-Fi in reverse order (async steps, idempotent):
 * converges from any intermediate state; best effort. */
void wifi_cam_restore_begin(void);
void wifi_cam_psk_purge(void);   /* Remove PSK residue from session/crash */
int  wifi_cam_restore_step(void);                     /* 1=in progress, 0=complete */

/* Interrupt (set by Disconnect_and_restore/Cancel_job): terminate in-flight subprocess immediately */
void wifi_cam_abort(void);

/* Startup residue cleanup (P1②): synchronous, one-shot, idempotent, fully logged;
 * no-op outside camera environment. */
int  wifi_cam_boot_cleanup(void);

/* Session wakelock (NOTES §19): hold from join acceptance (kernel named lock
 * x2print-session, /sys/power/wake_lock). Camera standby (autosleep suspends without a lock)
 * would freeze camera-system async power sequence (power_on_wifi is "accepted but does not
 * start") and STA link. backend_real releases after legal power sync completes; release once
 * at service startup to clear crash residue. No-op stub outside camera environment (WSL). */
void wifi_cam_wake_hold(void);
void wifi_cam_wake_release(void);

#endif
