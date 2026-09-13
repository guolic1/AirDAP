#!/usr/bin/env python3
"""Expose AirDAP network DAP/UART as a local CMSIS-DAP + CDC USB/IP device."""

import argparse
import asyncio
import logging
from logging.handlers import RotatingFileHandler
import math
from pathlib import Path
import signal
import ssl

from airdap_network_credential import CredentialError, load_credential
from airdap_usbip import NetworkBackend, Server


async def serve(args, credential):
    bridge = Server(credential.device_id, lambda: NetworkBackend(args.host, credential,
                    args.dap_port, args.uart_port, args.timeout))
    server = await asyncio.start_server(bridge.handle, "127.0.0.1", args.port)
    stop = asyncio.Event()
    loop = asyncio.get_running_loop()
    previous = {}
    for sig in (signal.SIGINT, signal.SIGTERM):
        previous[sig] = signal.signal(sig, lambda *_: loop.call_soon_threadsafe(stop.set))
    logging.info("Listening on 127.0.0.1:%d, bus 1-1, device %s; upstream %s:%d/%d",
                 args.port, credential.device_id, args.host, args.dap_port, args.uart_port)
    try:
        await stop.wait()
    finally:
        server.close()
        # On Python 3.13+, wait_closed also waits for accepted transports.
        # Close the imported session before waiting for listener shutdown.
        await bridge.close()
        await server.wait_closed()
        for sig, handler in previous.items():
            signal.signal(sig, handler)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="AirDAP IP address or mDNS hostname")
    parser.add_argument("--credential", type=Path, required=True)
    parser.add_argument("--port", type=int, default=3240, help="local USB/IP TCP port")
    parser.add_argument("--dap-port", type=int, default=3260)
    parser.add_argument("--uart-port", type=int, default=3261)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--log-file", type=Path, help="rotating background log, 2 MiB x 3")
    args = parser.parse_args(argv)
    if (not all(1 <= port <= 65535 for port in (args.port, args.dap_port, args.uart_port)) or
            not math.isfinite(args.timeout) or not 0 < args.timeout <= 60):
        parser.error("ports must be 1..65535 and timeout must be finite, in (0, 60]")
    if not hasattr(ssl.SSLContext, "set_psk_client_callback"):
        parser.error("Python 3.13+ with TLS-PSK support is required")
    handlers = [RotatingFileHandler(args.log_file, maxBytes=2 * 1024 * 1024,
                                   backupCount=2, encoding="utf-8")] if args.log_file else None
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s",
                        handlers=handlers)
    credential = load_credential(args.credential)
    # Windows VHCI can reset its socket during detach. Use the selector loop
    # for this socket-only server to avoid Proactor shutdown callback failures.
    asyncio.run(serve(args, credential), loop_factory=asyncio.SelectorEventLoop)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (CredentialError, OSError) as error:
        logging.error("airdap-usbip: %s", error)
        raise SystemExit(1)
