#!/bin/sh
# Only manage units carrying this installer's marker.
set -eu
action=${1:-status}
source_binary=${2:-./target/release/airdap-service}
http_port=${3:-8080}
usbip_port=${4:-3242}
unit=/etc/systemd/system/airdap.service
target=/opt/airdap
marker='# Installed by AirDAP service installer v1'
case "$action" in install|start|stop|status|remove) ;; *) echo 'Usage: install-linux.sh install [binary [http-port [usbip-port]]] | start | stop | status | remove' >&2; exit 2;; esac
if [ "$action" = install ]; then
    for port in "$http_port" "$usbip_port"; do
        case "$port" in ''|*[!0-9]*|0*) echo 'Ports must be decimal integers from 1 to 65535' >&2; exit 2;; esac
        if [ "${#port}" -gt 5 ] || [ "$port" -gt 65535 ]; then
            echo 'Ports must be decimal integers from 1 to 65535' >&2; exit 2
        fi
    done
    if [ "$http_port" = "$usbip_port" ]; then echo 'HTTP and USB/IP ports must differ' >&2; exit 2; fi
fi
if [ "$action" != status ] && [ "$(id -u)" != 0 ]; then echo 'Run with sudo/root' >&2; exit 1; fi
if [ "$action" = install ] && [ -e /etc/systemd/system/airdap-native.service ]; then echo 'Migrate the existing airdap-native service first; see README.' >&2; exit 1; fi
if [ -e "$unit" ] && ! grep -Fxq "$marker" "$unit"; then echo 'Refusing to change an unrecognized service unit' >&2; exit 1; fi
if [ "$action" != install ]; then
    test -f "$unit" || { echo 'AirDAP service is not installed' >&2; exit 1; }
    if [ "$action" = remove ]; then
        systemctl disable --now airdap.service
        rm -- "$unit"
        systemctl daemon-reload
        echo 'Service registration removed; program and credentials retained.'
    else systemctl "$action" airdap.service; fi
    exit
fi
test ! -e "$unit" && test ! -e "$target" && test ! -e /var/lib/airdap || { echo 'Existing AirDAP installation or data found; inspect it before replacing files.' >&2; exit 1; }
test -f "$source_binary" && test -x "$source_binary" || { echo 'Supply the native release executable' >&2; exit 1; }
command -v usbip >/dev/null || { echo 'Install the distribution usbip package first' >&2; exit 1; }
"$source_binary" --version
modprobe vhci_hcd
install -d -o root -g root -m 755 "$target"
install -o root -g root -m 755 "$source_binary" "$target/airdap-service"
install -d -o root -g root -m 700 /var/lib/airdap
cat > "$unit" <<UNIT
# Installed by AirDAP service installer v1
[Unit]
Description=AirDAP local USB/IP and device management service
After=network-online.target bluetooth.service
Wants=network-online.target bluetooth.service

[Service]
Type=simple
ExecStartPre=/sbin/modprobe vhci_hcd
ExecStart=/opt/airdap/airdap-service --data-dir /var/lib/airdap --http-port $http_port --usbip-port $usbip_port
Restart=on-failure
RestartSec=3
# A stop drains the current device write; never SIGKILL an OTA operation.
TimeoutStopSec=infinity
UMask=0077
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/var/lib/airdap

[Install]
WantedBy=multi-user.target
UNIT
chmod 644 "$unit"
systemctl daemon-reload
systemctl enable --now airdap.service
echo "AirDAP enabled: http://airdap.localhost:$http_port"
