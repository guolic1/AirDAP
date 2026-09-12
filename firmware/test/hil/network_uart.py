#!/usr/bin/env python3
"""UART TCP/USB coexistence on GPIO17/18 loopback; requires pyserial."""
import argparse
import importlib.util
import json
from pathlib import Path
import sys
import time

import serial

TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location("uart_probe", TOOLS / "airdap-uart-probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def usb_read(link, size):
    data = link.read(size)
    if len(data) != size:
        raise probe.ProbeError(f"USB RX short: {len(data)}/{size}")
    return data


def expect_revoked(client):
    try:
        client.request(7, b"", 7)
    except (ConnectionResetError, BrokenPipeError):
        return
    except probe.ProbeError as error:
        if str(error) in ("AirDAP connection closed during a frame", "AirDAP error 0x0021"):
            return
        raise
    raise probe.ProbeError("revoked connection still accepts KEEPALIVE")


def run(args, credential):
    results = {}
    with probe.Client(args.host, credential, bind=False) as net:
        probe.expect_error(net.status, 0x21)
    results["before_auth_rejected"] = True
    time.sleep(0.2)
    for baud in (9600, 115200, 1000000):
        with probe.Client(args.host, credential) as net:
            net.acquire()
            net.configure(baud)
            with serial.Serial(args.usb_port, baudrate=baud, timeout=2, write_timeout=2) as usb:
                time.sleep(0.1)
                usb.reset_input_buffer()
                total = 0
                for i in range(16):
                    payload = bytes((i + k) % 256 for k in range(256))
                    net.write_all(payload)
                    assert net.read_exact(256) == payload
                    assert usb_read(usb, 256) == payload
                    total += len(payload)
                # The network already owns TX. A competing CDC write is dropped
                # by the existing USB bridge and must not reach physical UART.
                assert usb.write(b"USB-TX-MUST-BE-BUSY") == 19
                time.sleep(0.1)
                assert net.read()[0] == b""
                assert usb.in_waiting == 0
                assert net.status()["tx_owner"]
                results[f"network_owner_{baud}"] = dict(bytes=total, chunk=256,
                    usb_fanout_bytes=total, network_dropped=net.status()["dropped"],
                    usb_tx_rejected=True)
        time.sleep(0.2)
    with serial.Serial(args.usb_port, baudrate=115200, timeout=2, write_timeout=2) as usb:
        time.sleep(0.1)
        with probe.Client(args.host, credential) as net:
            probe.expect_error(net.acquire, 0x20)
            probe.expect_error(lambda: net.write(b"blocked"), 0x20)
            usb.reset_input_buffer()
            # Do not read from TCP while USB continues making progress.
            for i in range(8):
                payload = bytes([i]) * 256
                assert usb.write(payload) == 256
                assert usb_read(usb, 256) == payload
            state = net.status()
            assert state["buffered"] == 512 and state["dropped"] == 1536, state
            assert not state["tx_owner"]
            assert net.read() == (bytes([0]) * 256, 1536)
            assert net.read() == (bytes([1]) * 256, 1536)
            results["slow_network_reader"] = dict(usb_bytes=2048, chunk=256,
                buffered=512, network_dropped=1536, usb_byte_equality=True)
        # Closing the network subscriber must leave USB ownership usable.
        assert usb.write(b"after-network-close") == 19
        assert usb_read(usb, 19) == b"after-network-close"
    time.sleep(0.2)
    with probe.Client(args.host, credential, port=3260) as dap:
        with probe.Client(args.host, credential, token=dap.token) as net:
            assert dap.owner_session == net.owner_session
            net.acquire()
            net.configure(115200)
            assert probe.dap._parse_dap_info(dap.request(3, b"\x00\x09", 4)) == net.firmware
            dap.close()
            time.sleep(0.3)
            expect_revoked(net)
    results["dap_token_join_and_disconnect_revocation"] = True
    time.sleep(0.2)
    with probe.Client(args.host, credential) as net:
        net.acquire()
        net.configure(115200)
        if args.idle_expiry:
            print("Waiting 62 seconds for real owner idle expiry", flush=True)
            time.sleep(62)
            expect_revoked(net)
            results["idle_expiry_rejected"] = True
    time.sleep(0.2)
    with probe.Client(args.host, credential) as net:
        net.acquire()
        net.configure(115200)
        net.write_all(b"recovered")
        assert net.read_exact(9) == b"recovered"
        results["final_reacquire_loopback"] = True
        results["firmware"] = net.firmware
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--credential", type=Path, required=True)
    parser.add_argument("--usb-port", required=True)
    parser.add_argument("--idle-expiry", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = {"status": "running"}
    try:
        result.update(run(args, probe.load_credential(args.credential)))
        result["status"] = "passed"
    except BaseException as error:
        result.update(status="failed", error=str(error))
        raise
    finally:
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
