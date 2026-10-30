#!/bin/sh
# deploy_and_join.sh — Push wpa_supplicant/wpa_cli to the camera and join a printer hotspot
# (PC-side script, run under Windows Git Bash; same channel as app.sh: 30303 rsh + busybox nc push)
#
# Usage (from x2print/):
#   sh src/wifi_bin/deploy_and_join.sh join <SSID> <PSK>
#       Push build/aarch64/{wpa_supplicant,wpa_cli} + generated config → camera
#       /data/x2print/wifi/, start supplicant (-B -Dnl80211 -i wlan0), poll
#       wpa_cli STATUS until COMPLETED (45s), then obtain an address with busybox udhcpc.
#   sh src/wifi_bin/deploy_and_join.sh stop     # Stop supplicant on the camera
#   sh src/wifi_bin/deploy_and_join.sh status   # Check process/wpa_state/wlan0 IP
#   sh src/wifi_bin/deploy_and_join.sh log      # Tail wpa log
#
# Requirements: camera rsh link is working (TESTDIR/.tmpwork/{rsh.py,push.py}); LHOST is
#       the local IP as seen by the camera; camera busybox is at /system/xbin/busybox.
set -e

TESTDIR="${TESTDIR:-C:/Users/tom/Documents/test}"
LHOST="${LHOST:-192.168.42.1}"
RSH_PY="$(cygpath -m "$TESTDIR")/.tmpwork/rsh.py"      # -m: C:/... mixed path, readable by Windows Python
PUSH_PY="$(cygpath -m "$TESTDIR")/.tmpwork/push.py"
rsh() { python "$RSH_PY" "$@"; }
push_listen() { python "$PUSH_PY" "$@"; }

HERE=$(cd "$(dirname "$0")" && pwd)            # .../x2print/src/wifi_bin
PROJ=$(cd "$HERE/../.." && pwd)                # .../x2print
BIN=$PROJ/build/aarch64
ROOT=/data/x2print/wifi                         # Camera-side deployment directory

# Push a file with busybox nc and verify its md5 on the camera (x2nes lesson: nc can report false success)
push_file() { # local_file remote_file
    lfile="$1"; rfile="$2"; port=$((9000 + RANDOM % 500))
    push_listen "$port" "$lfile" &
    pt=$!
    sleep 0.3
    rsh "busybox nc $LHOST $port > $rfile.tmp && chmod 755 $rfile.tmp && mv $rfile.tmp $rfile && ls -l $rfile"
    kill $pt 2>/dev/null || true
    lmd5=$(md5sum "$lfile" | cut -d' ' -f1)
    rmd5=$(rsh "busybox md5sum $rfile" | cut -d' ' -f1)
    [ "$lmd5" = "$rmd5" ] || { echo "MD5 mismatch: $rfile ($lmd5 != $rmd5)"; return 1; }
    echo "pushed $rfile md5 ok"
}

# Camera-side one-shot join script (send stdin with rsh -f for execution; avoids escaping); $1=ROOT
cam_join='
ROOT=/data/x2print/wifi
killall wpa_supplicant 2>/dev/null; sleep 0.5
rm -rf /tmp/wpas /tmp/wpa_supplicant.pid
busybox ip link set wlan0 up 2>/dev/null || true
"$ROOT"/wpa_supplicant -B -Dnl80211 -i wlan0 -c "$ROOT"/wpa_supplicant.conf \
    -P /tmp/wpa_supplicant.pid -f "$ROOT"/wpa.log || { echo "wpa_supplicant failed to start; tailing log:"; tail -n 20 "$ROOT"/wpa.log; exit 1; }
sleep 1
st=""; i=0
while [ "$i" -lt 45 ]; do
    st=$("$ROOT"/wpa_cli -p /tmp/wpas status 2>/dev/null | grep "^wpa_state=" | cut -d= -f2)
    echo "[poll $i] wpa_state=${st:-<supplicant not ready>}"
    [ "$st" = "COMPLETED" ] && break
    i=$((i + 1)); sleep 1
done
if [ "$st" != "COMPLETED" ]; then
    echo "JOIN FAILED (last state: ${st:-none}); tailing wpa.log:"
    tail -n 25 "$ROOT"/wpa.log
    exit 1
fi
echo "== 4-way handshake complete; obtaining address with udhcpc =="
# Generate a minimal udhcpc event script (deconfig/bound/renew) if missing
if [ ! -f /tmp/udhcpc.sh ]; then
    printf "#!/system/bin/sh\ncase $1 in deconfig) busybox ifconfig $interface 0.0.0.0;; bound|renew) busybox ifconfig $interface $ip netmask $subnet; busybox route add default gw $router 2>/dev/null;; esac\n" > /tmp/udhcpc.sh
    chmod 755 /tmp/udhcpc.sh
    echo "(note: /tmp/udhcpc.sh was missing on the camera; generated a minimal event script)"
fi
busybox udhcpc -i wlan0 -s /tmp/udhcpc.sh -n -q -t 6 -T 3 || echo "warning: udhcpc did not obtain a lease"
echo "== Result =="
"$ROOT"/wpa_cli -p /tmp/wpas status | grep -E "^(wpa_state|ssid|bssid|key_mgmt)=" || true
busybox ifconfig wlan0 2>/dev/null | grep -E "inet |HWaddr" || echo "(wlan0 has no IP)"
'

case "${1:-}" in
""|-h|--help|help)
    sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
    ;;
join)
    SSID="${2:-}"; PSK="${3:-}"
    [ -n "$SSID" ] && [ -n "$PSK" ] || { echo "Usage: $0 join <SSID> <PSK>"; exit 2; }
    # Use -f, not -x: files produced by WSL lack the executable bit in Git Bash
    # (the camera-side file is chmod 755 after upload)
    [ -f "$BIN/wpa_supplicant" ] && [ -f "$BIN/wpa_cli" ] || {
        echo "Missing $BIN/{wpa_supplicant,wpa_cli}: run third_party/build_wpa.sh first"; exit 1; }
    rsh "mkdir -p $ROOT"
    push_file "$BIN/wpa_supplicant" "$ROOT/wpa_supplicant"
    push_file "$BIN/wpa_cli"      "$ROOT/wpa_cli"
    # Replace {{SSID}}/{{PSK}} in the template (plain-text replacement via Python avoids
    # sed metacharacter pitfalls).
    # Keep the temporary file in the script directory: Windows Python cannot open MSYS /tmp paths.
    TMP="$HERE/.wpa_conf.tmp"
    SSID="$SSID" PSK="$PSK" python - "$HERE/wpa_supplicant.conf" > "$TMP" <<'PYEOF'
import os, sys
tpl = open(sys.argv[1], encoding="utf-8").read()
sys.stdout.write(tpl.replace("{{SSID}}", os.environ["SSID"]).replace("{{PSK}}", os.environ["PSK"]))
PYEOF
    push_file "$TMP" "$ROOT/wpa_supplicant.conf"
    rm -f "$TMP"
    # Save the camera-side join script and send it via rsh -f (rsh.py does not recognize /dev/stdin)
    printf '%s\n' "$cam_join" > "$HERE/.cam_join.tmp"
    rsh -f "$HERE/.cam_join.tmp"
    rm -f "$HERE/.cam_join.tmp"
    ;;
stop)
    rsh "killall wpa_supplicant 2>/dev/null; rm -rf /tmp/wpas /tmp/wpa_supplicant.pid; echo stopped"
    ;;
status)
    rsh "ps -A 2>/dev/null | grep -v grep | grep wpa_; \
$ROOT/wpa_cli -p /tmp/wpas status 2>/dev/null | grep -E '^(wpa_state|ssid|bssid|key_mgmt)=' || echo '(supplicant not running)'; \
busybox ifconfig wlan0 2>/dev/null | grep -E 'inet |HWaddr'"
    ;;
log)
    rsh "tail -n ${2:-40} $ROOT/wpa.log 2>/dev/null || echo '(no log)'"
    ;;
*)
    echo "Unknown subcommand: $1 (available: join/stop/status/log)" >&2; exit 2
    ;;
esac
