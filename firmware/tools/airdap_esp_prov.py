#!/usr/bin/env python3
"""Run Espressif provisioning with AirDAP's unbonded BLE transport."""

from __future__ import annotations

import asyncio
import importlib
import sys
from pathlib import Path
from typing import Any


class AirDapBleClient:
    """esp_prov BLE client interface, without OS pairing or unpairing.

    AirDAP permits unencrypted GATT access and protects provisioning with
    Security 2. Windows pairing otherwise waits about 60 seconds for a system
    device node; neither that association nor deleting existing bonds is needed.
    """

    def __init__(self, backend: Any = None):
        if backend is None:
            import bleak

            backend = bleak
        self.backend = backend
        self.device = None
        self.characteristics = {}
        self.nu_lookup = None

    async def connect(self, devname, iface, chrc_names, fallback_srv_uuid):
        print("Discovering...")
        discovery = await self.backend.BleakScanner.discover(
            return_adv=True, adapter=iface
        )
        found = next(
            (
                (device, advertisement)
                for device, advertisement in discovery.values()
                if (advertisement.local_name or device.name) == devname
            ),
            None,
        )
        if found is None:
            raise RuntimeError("Device not found")
        device, advertisement = found
        # Retain the discovered BLEDevice to avoid Bleak's implicit address scan.
        self.device = self.backend.BleakClient(device, adapter=iface, pair=False)
        try:
            print("Connecting and discovering services...")
            await self.device.connect()
            services = self.device.services
            service = None
            if len(advertisement.service_uuids) == 1:
                service = services.get_service(advertisement.service_uuids[0])
            if service is None:
                service = services.get_service(fallback_srv_uuid)
            if service is None:
                raise RuntimeError("Provisioning service not found")

            lookup = {}
            for characteristic in service.characteristics:
                self.characteristics[characteristic.uuid] = characteristic
                for descriptor in characteristic.descriptors:
                    if descriptor.uuid[4:8].lower() != "2901":
                        continue
                    value = await self.device.read_gatt_descriptor(descriptor.handle)
                    lookup[bytes(value).decode("utf-8").lower()] = characteristic.uuid
            self.nu_lookup = (
                lookup if all(name.lower() in lookup for name in chrc_names) else None
            )
            return True
        except BaseException:
            try:
                await self.disconnect()
            except Exception:
                pass  # Preserve the connection/descriptor error after cleanup.
            raise

    def get_nu_lookup(self):
        return self.nu_lookup

    def has_characteristic(self, uuid):
        return uuid in self.characteristics

    async def send_data(self, characteristic_uuid, data):
        await self.device.write_gatt_char(
            characteristic_uuid, bytearray(data.encode("latin-1")), response=True
        )
        response = await self.device.read_gatt_char(characteristic_uuid)
        return bytes(response).decode("latin-1")

    async def disconnect(self):
        try:
            if self.device is not None:
                print("Disconnecting...")
                await self.device.disconnect()
        finally:
            self.device = None
            self.nu_lookup = None
            self.characteristics.clear()


def main(argv=None):
    args = list(sys.argv[1:] if argv is None else argv)
    script = Path(args[0]).resolve()
    sys.path.insert(0, str(script.parent))
    esp_prov = importlib.import_module("esp_prov")
    ble_cli = importlib.import_module("transport.ble_cli")
    # Change only this child process's BLE factory. Security 2 and the interactive
    # Wi-Fi workflow remain in the downloaded Espressif client.
    ble_cli.get_client = AirDapBleClient
    sys.argv = [str(script), *args[1:]]
    asyncio.run(esp_prov.main())


if __name__ == "__main__":
    main()
