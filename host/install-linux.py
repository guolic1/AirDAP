#!/usr/bin/env python3
"""Install a protected systemd service using the project's locked uv environment."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess


def run(*args):
    subprocess.run(args, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install', 'start', 'stop', 'status', 'remove'))
    parser.add_argument('--python', default='/usr/bin/python3.14', help='system Python 3.13+ with TLS-PSK')
    parser.add_argument('--http-port', type=int, default=8080)
    parser.add_argument('--usbip-port', type=int, default=3242)
    parser.add_argument('--no-http', action='store_true')
    args = parser.parse_args()
    if os.name != 'posix' or not Path('/run/systemd/system').is_dir():
        parser.error('This installer requires Linux with systemd running')
    if args.action != 'status' and os.geteuid() != 0:
        parser.error('Run with sudo/root')
    unit = Path('/etc/systemd/system/airdap.service')
    marker = 'Description=AirDAP local USB/IP and device management service'
    if unit.exists() and marker not in unit.read_text():
        parser.error('Existing airdap.service is not owned by this installer')
    if args.action != 'install' and not unit.exists():
        parser.error('AirDAP service is not installed by this installer')
    if args.action == 'remove':
        run('systemctl', 'disable', '--now', 'airdap.service')
        # Exact service registration only. Program files and credentials are retained.
        unit.unlink()
        run('systemctl', 'daemon-reload')
        return
    if args.action != 'install':
        run('systemctl', args.action, 'airdap.service')
        return
    if not all(1 <= port <= 65535 for port in (args.http_port, args.usbip_port)):
        parser.error('Ports must be 1..65535')
    target = Path('/opt/airdap')
    if unit.exists() or target.exists():
        parser.error('Service or /opt/airdap already exists; inspect it before installing again')
    if not shutil.which('uv'):
        parser.error('uv must be available in the administrator PATH')
    run(args.python, '-c', 'import ssl; assert hasattr(ssl.SSLContext,"set_psk_client_callback")')
    source = Path(__file__).resolve().parents[1]
    target.mkdir(mode=0o755)
    (target / 'host').mkdir()
    (target / 'firmware/tools').mkdir(parents=True)
    for directory in ('host', 'firmware/tools'):
        for item in (source / directory).glob('*.py'):
            shutil.copyfile(item, target / directory / item.name)
    shutil.copytree(source / 'host/web', target / 'host/web')
    for name in ('pyproject.toml', 'uv.lock'):
        shutil.copyfile(source / name, target / name)
    # Only the repository's existing locked dependencies; no system pip changes.
    run('uv', 'sync', '--locked', '--project', str(target), '--python', args.python)
    prov = source / 'firmware/managed_components/espressif__network_provisioning/tool/esp_prov'
    idf_file = source / 'firmware/.airdap-env/idf-path.txt'
    idf = os.environ.get('IDF_PATH') or (idf_file.read_text().splitlines()[0] if idf_file.exists() else '')
    extra = ''
    if idf and prov.is_dir():
        shutil.copytree(Path(idf) / 'components/protocomm/python', target / 'provisioning/idf/components/protocomm/python')
        component = target / 'provisioning/network_provisioning'
        shutil.copytree(prov, component / 'tool/esp_prov')
        shutil.copytree(prov.parents[1] / 'python', component / 'python')
        if (prov.parents[1] / 'LICENSE').exists():
            shutil.copyfile(prov.parents[1] / 'LICENSE', component / 'LICENSE')
        extra = ' --idf-path /opt/airdap/provisioning/idf --provisioning-dir /opt/airdap/provisioning/network_provisioning/tool/esp_prov'
        run(str(target / '.venv/bin/python'), '-c',
            'import sys; sys.path.insert(0,sys.argv[1]); from airdap_service_devices import Devices; Devices(idf_path=sys.argv[2],provisioning_dir=sys.argv[3]).ble_client()',
            str(target / 'host'), str(target / 'provisioning/idf'), str(component / 'tool/esp_prov'))
    else:
        print('BLE provisioning components absent; USB provisioning and OTA remain available.')
    run('modprobe', 'vhci_hcd')
    # The module is needed on each boot, before automatic USB/IP attach.
    text = (source / 'host/airdap.service').read_text().replace('Type=simple', 'Type=simple\nExecStartPre=/sbin/modprobe vhci_hcd')
    text = text.replace('--data-dir /var/lib/airdap', f'--data-dir /var/lib/airdap --http-port {args.http_port} --usbip-port {args.usbip_port}' + extra + (' --no-http' if args.no_http else ''))
    unit.write_text(text)
    run('systemctl', 'daemon-reload')
    run('systemctl', 'enable', '--now', 'airdap.service')
    print(f'AirDAP service enabled: http://127.0.0.1:{args.http_port}')


if __name__ == '__main__':
    main()
