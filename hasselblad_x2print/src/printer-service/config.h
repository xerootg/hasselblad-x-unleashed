/* config.h — Read/write /data/x2print/config.ini (minimal hand-written INI, no third-party dependencies) */
#ifndef PRN_CONFIG_H
#define PRN_CONFIG_H

/* Load: write defaults if the file does not exist (first launch); use defaults and warn
 * for invalid key values. 0=ok, -1=I/O error. */
int prn_config_load(const char *path);
/* Persist writable properties. 0=ok, -1=I/O error. */
int prn_config_save(const char *path);

/* ---- Service-level extension keys (phase 2/finalization, not D-Bus properties;
 * see NOTES.md §10/§16) ----
 * printer_ip      : Cache of known printer endpoints (comma-separated host[:port]).
 *                   When nonempty, Start_discovery tries direct connection first; newly
 *                   discovered endpoints are written back here.
 * ipp_port        : IPP port (default 631)
 * photo_root      : Default Get_photo_list directory (empty = camera default /mnt/media_runtime)
 * wifi_backend    : Connect_to_ap execution layer: "mock" (integration testing, no Wi-Fi) /
 *                   "cam" (camera orchestration)
 * idle_restore_min: Idle watchdog in minutes (default 5, 0=disabled): after a timeout with
 *                   no business-method calls while joined/ready and no print job is active,
 *                   automatically restore baseline Wi-Fi and return to disconnected.
 * Captured on load and written back unchanged on save; returns "" if unset. */
const char *prn_config_get(const char *key);
/* Update a service-level extension key and save atomically (load remembers the path);
 * 0=ok, -1=unknown key/I/O error. */
int prn_config_set(const char *key, const char *val);

#endif
