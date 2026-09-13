#!/usr/bin/env python3
"""Run the AirDAP local management service, optionally under Windows SCM."""
import argparse
import asyncio
from contextlib import contextmanager
import logging
from logging.handlers import RotatingFileHandler
import os
from pathlib import Path
import signal
import ssl
import threading


@contextmanager
def instance_lock(path):
    path.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
    with path.open('a+b') as stream:
        if os.name == 'nt':
            import msvcrt
            stream.seek(0)
            if not stream.read(1):
                stream.write(b'0')
                stream.flush()
            stream.seek(0)
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield


async def serve(args, stop, ready):
    from airdap_service import Service, Store, WebServer
    from airdap_service_devices import Devices
    service = Service(Store(args.data_dir), usbip_port=args.usbip_port,
                      devices=Devices(idf_path=args.idf_path, provisioning_dir=args.provisioning_dir),
                      usbip_executable=args.usbip_executable)
    web = maintenance = None
    try:
        if not args.no_http:
            web = WebServer(('127.0.0.1', args.http_port), service, asyncio.get_running_loop())
            threading.Thread(target=web.serve_forever, daemon=True).start()
            logging.info('Web management: http://127.0.0.1:%d', web.server_port)
        if service.store.profile.get('bridge_enabled'):
            service.start_job('恢复 USB 桥接', service.start_bridge)
        maintenance = asyncio.create_task(service.maintain())
        ready()
        while not stop.is_set():
            await asyncio.sleep(.25)
    finally:
        service.closing = True
        if web:
            await asyncio.to_thread(web.shutdown)
            web.server_close()
        if maintenance:
            maintenance.cancel()
            await asyncio.gather(maintenance, return_exceptions=True)
        await service.close()


def run(args, stop, ready=lambda: None):
    args.data_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
    with instance_lock(args.data_dir / 'service.lock'):
        logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s',
            handlers=[RotatingFileHandler(args.data_dir / 'service.log', maxBytes=2*1024*1024,
                                         backupCount=2, encoding='utf-8')])
        logging.info('AirDAP service starting')
        try:
            asyncio.run(serve(args, stop, ready), loop_factory=asyncio.SelectorEventLoop)
        finally:
            logging.info('AirDAP service stopped')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    default_data = Path(os.environ.get('LOCALAPPDATA', Path.home() / '.local/share')) / 'AirDAP/service'
    parser.add_argument('--data-dir', type=Path, default=default_data)
    parser.add_argument('--http-port', type=int, default=8080)
    parser.add_argument('--usbip-port', type=int, default=3242)
    parser.add_argument('--no-http', action='store_true')
    parser.add_argument('--usbip-executable', help='administrator-configured USB/IP client path')
    parser.add_argument('--idf-path', type=Path)
    parser.add_argument('--provisioning-dir', type=Path)
    parser.add_argument('--windows-service', action='store_true')
    parser.add_argument('--service-name', default='AirDAP')
    args = parser.parse_args()
    if not all(1 <= p <= 65535 for p in (args.http_port, args.usbip_port)):
        parser.error('ports must be 1..65535')
    if not hasattr(ssl.SSLContext, 'set_psk_client_callback'):
        parser.error('Python 3.13+ with TLS-PSK support is required')
    if args.windows_service:
        if os.name != 'nt':
            parser.error('--windows-service is only available on Windows')
        from airdap_service_windows import dispatch
        return dispatch(args.service_name, lambda stop, ready: run(args, stop, ready))
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    run(args, stop)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
