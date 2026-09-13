#!/bin/sh
# Registration uses a distinct name so the Python service remains recoverable.
set -eu
action=${1:-status}
source_binary=${2:-./target/release/airdap-service}
unit=/etc/systemd/system/airdap-native.service
target=/opt/airdap-native
marker='# Installed by AirDAP native installer v1'
case "$action" in install|start|stop|status|remove) ;; *) echo 'Usage: install-linux.sh install [binary] | start | stop | status | remove' >&2; exit 2;; esac
if [ "$action" != status ] && [ "$(id -u)" != 0 ]; then echo 'Run with sudo/root' >&2; exit 1; fi
if [ -e "$unit" ] && ! grep -Fxq "$marker" "$unit"; then echo 'Refusing to change an unrecognized service unit' >&2; exit 1; fi
if [ "$action" != install ]; then
    test -f "$unit" || { echo 'Native service is not installed' >&2; exit 1; }
    if [ "$action" = remove ]; then
        systemctl disable --now airdap-native.service
        rm -- "$unit"
        systemctl daemon-reload
        echo 'Service registration removed; program and credentials retained.'
    else systemctl "$action" airdap-native.service; fi
    exit
fi
test ! -e "$unit" && test ! -e "$target" && test ! -e /var/lib/airdap-native || { echo 'Existing native installation or data found; inspect it before replacing files.' >&2; exit 1; }
test -f "$source_binary" && test -x "$source_binary" || { echo 'Supply the native release executable' >&2; exit 1; }
command -v usbip >/dev/null || { echo 'Install the distribution usbip package first' >&2; exit 1; }
"$source_binary" --version
modprobe vhci_hcd
install -d -o root -g root -m 755 "$target"
install -o root -g root -m 755 "$source_binary" "$target/airdap-service"
install -d -o root -g root -m 700 /var/lib/airdap-native
cat > "$unit" <<'UNIT'
# Installed by AirDAP native installer v1
[Unit]
Description=AirDAP native local USB/IP and device management service
After=network-online.target bluetooth.service
Wants=network-online.target bluetooth.service

[Service]
Type=simple
ExecStartPre=/sbin/modprobe vhci_hcd
ExecStart=/opt/airdap-native/airdap-service --data-dir /var/lib/airdap-native --http-port 8080 --usbip-port 3242
Restart=on-failure
RestartSec=3
# A stop drains the current device write; never SIGKILL an OTA operation.
TimeoutStopSec=infinity
UMask=0077
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/airdap-native

[Install]
WantedBy=multi-user.target
UNIT
chmod 644 "$unit"
systemctl daemon-reload
systemctl enable --now airdap-native.service
echo 'AirDAP native enabled: http://127.0.0.1:8080'
