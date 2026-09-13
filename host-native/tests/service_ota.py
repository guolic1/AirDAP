"""OTA API against a TLS-PSK firmware peer: no hardware writes."""
import json
import os
import struct
import threading
import time
import unittest
import service_http
from network_usbip import Firmware, FRAME, DEVICE, exact

class OtaFirmware(Firmware):
    def __init__(self):
        self.offset = 0
        self.wrong_offset = False
        self.committed = False
        self.aborted = False
        self.rebooted = False
        self.query_after_boot = False
        self.write_started = threading.Event()
        self.begin_count = 0
        super().__init__(3260)

    def run(self, link):
        try:
            while not self.closed:
                magic, version, kind, flags, session, sequence, size, reserved = FRAME.unpack(exact(link,20))
                assert (magic,version,flags,reserved) == (b'ADAP',1,0,0)
                data = exact(link,size)
                if kind == 1:
                    response = bytes(16)+struct.pack('>I',31)+DEVICE.encode()+b'vtest'
                elif kind == 2:
                    assert data == b''
                    response = struct.pack('>I',7)+b't'*32
                else:
                    assert kind == 5
                    kind = 6
                    op, payload = data[0],data[1:]
                    response = bytes([op,0])
                    if op == 0x30:
                        running, inactive = (0x410000,0x20000) if self.rebooted else (0x20000,0x410000)
                        response += struct.pack('>BBIIIB',1,1,0x3f0000,running,inactive,1)+b'vtest'
                        self.query_after_boot |= self.rebooted
                    elif op == 0x31:
                        self.begin_count += 1
                        self.size = int.from_bytes(payload,'big')
                    elif op == 0x32:
                        assert int.from_bytes(payload[:4],'big') == self.offset
                        self.write_started.set(); time.sleep(.3)
                        self.offset += len(payload)-4
                        response += (self.offset+int(self.wrong_offset)).to_bytes(4,'big')
                    elif op == 0x33:
                        assert self.offset == self.size
                        self.committed = True
                    elif op == 0x34:
                        self.aborted = True
                    elif op == 0x35:
                        assert self.committed
                        self.rebooted = True
                        return  # Deliberately lose the reboot ACK.
                    else:
                        raise AssertionError(op)
                link.sendall(FRAME.pack(b'ADAP',1,kind,0,session,sequence,len(response),0)+response)
        except (OSError,EOFError):
            pass
        finally:
            link.close()

class NativeOta(service_http.NativeService):
    # Inherit the reusable process harness; these base cases also protect OTA's API boundary.
    def setUp(self):
        self.firmware = OtaFirmware()
        self.addCleanup(self.firmware.close)
        super().setUp()

    def begin_ota(self):
        self.profile()
        image = bytearray(8192); image[0]=0xe9; image[12]=9
        image[32:36]=(0xabcd5432).to_bytes(4,'little'); image[48:53]=b'vtest'
        status, meta = self.request('POST','/api/image',bytes(image))
        self.assertEqual(status,200)
        status,_ = self.request('POST','/api/ota',{'device_id':DEVICE,'transport':'network','confirm':True,'sha256':json.loads(meta)['sha256']})
        self.assertEqual(status,202)

    def test_committed_image_requires_new_slot_and_confirmation_after_lost_ack(self):
        self.begin_ota()
        state = self.job()
        self.assertTrue(state['job']['result']['confirmed'])
        self.assertEqual(state['job']['result']['running_address'],0x410000)
        self.assertTrue(self.firmware.query_after_boot)
        self.assertIsNone(state['image'])
        self.assertEqual(self.firmware.begin_count,1)

    def test_bad_offset_aborts_and_never_replays_write(self):
        self.firmware.wrong_offset = True
        self.begin_ota()
        end = time.monotonic()+5
        while (state := self.state())['job']['state'] == 'running':
            self.assertLess(time.monotonic(),end); time.sleep(.02)
        self.assertEqual(state['job']['state'],'failed')
        self.assertTrue(self.firmware.aborted)
        self.assertFalse(self.firmware.committed)
        self.assertEqual(self.firmware.begin_count,1)
        self.assertIsNone(state['image'])

    @unittest.skipIf(os.name == 'nt', 'TerminateProcess cannot emulate SCM graceful stop; exercised on Linux SIGTERM')
    def test_sigterm_drains_inflight_ota_before_exit(self):
        self.begin_ota()
        self.assertTrue(self.firmware.write_started.wait(5))
        self.stop()
        self.assertEqual(self.process.returncode,0)
        self.assertTrue(self.firmware.query_after_boot)

if __name__ == '__main__':
    unittest.main()
