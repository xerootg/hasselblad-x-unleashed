/* adaptor.h — libdbus adaptor: owns com.hasselblad.printer, dispatches methods/properties,
 * emits propertiesChanged (lowercase p!) — see dbus/INTERFACE.md for the wire contract. */
#ifndef PRN_ADAPTOR_H
#define PRN_ADAPTOR_H

#include <dbus/dbus.h>
#include "backend.h"

/* Register the /printer object filter (method dispatch + Properties + Introspect); 0=ok, -1=failed */
int  prn_adaptor_start(DBusConnection *c, const prn_backend *be);

/* Broadcast a property-change signal (using the connection saved at start):
 * path=/printer iface=org.freedesktop.DBus.Properties member=propertiesChanged
 * Signature sa{sv}as, arg0=com.hasselblad.printer, arg2 always empty */
void prn_adaptor_emit_changed(int prop_idx);

/* odindb-send-printer compatibility signals (INTERFACE.md §7b): interface
 * com.hasselblad.printer, path=/printer, member=rx|testrx, signature "yay"
 * (y=message_len, ay=message).
 * rx sources: backend_real job_status changes (X2P event frame, see backend_real.c) and
 * tx() echo; testrx is reserved (no internal source). Clients subscribe with `--signal rx|testrx`. */
void prn_adaptor_emit_rx(unsigned char len, const unsigned char *data, int n);
void prn_adaptor_emit_testrx(unsigned char len, const unsigned char *data, int n);

/* ---- Business-activity timestamp (idle-watchdog criterion, INTERFACE.md §4b) ----
 * Refreshed on entry to business methods (all of handle_methods, including tx) and
 * Properties.Set; Properties.Get/GetAll (polling only) do not refresh it — an open but
 * inactive GUI still counts as idle.
 * prn_adaptor_idle_ms(): milliseconds since the last business call (measured from start). */
void prn_adaptor_touch(void);
long long prn_adaptor_idle_ms(void);

/* ---- Synchronize Wi-Fi power through the camera system service (§4b restore protocol) ----
 * Call com.hasselblad.system /system power_on_wifi(b enable) with the strict QtDBus
 * signature: exactly one bool, no wrapper timeout trailing argument. Returns 0=ok,
 * -1=call failed (already logged).
 * Used to release the chip through the supported API before a session (turn off a running
 * native AP) and synchronize the camera-system state machine at restore completion, returning
 * to the pre-session state and avoiding state desynchronization from direct rmmod. */
int prn_adaptor_system_wifi_power(int on);

/* ---- Camera screen-idle timeout (§19 session/standby management, display layer) ----
 * com.hasselblad.system property screen_idle_timeout (int, seconds): temporarily set to
 * a large value during a session so the screen stays on while printing, then restore its
 * original value at completion. Uses org.freedesktop.DBus.Properties Get/Set (standard ssv,
 * variant int32). Returns 0=ok, -1=failed (already logged). */
int prn_adaptor_screen_idle_get(int *sec);
int prn_adaptor_screen_idle_set(int sec);

/* ---- Delayed Scan_wifi reply (P0 starvation prevention, NOTES §22) ----
 * When the real backend's scan_wifi returns 1 (accepted asynchronously; chip release,
 * protection, and scanning proceed in the tick state machine), the adaptor holds the
 * method reply (refs the message in m_scan_wifi). When the backend completes, call this
 * function to send it: ok=1 → as (same source as scanned_networks); ok=0 →
 * com.hasselblad.error.failed (including be_err details). The wire contract remains
 * unchanged (the client still sees a blocking call until completion), while the event loop
 * continues dispatching. No-op if there is no held reply. */
void prn_adaptor_scan_complete(int ok);

#endif
