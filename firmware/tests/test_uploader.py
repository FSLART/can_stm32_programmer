import hashlib
import contextlib
import io
import re
from pathlib import Path
import struct
import sys
import unittest
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import can_upload as uploader


def image():
    data = struct.pack("<IIHHII", 0x96F3B83D, 0, 512, 0, 17, 0).ljust(512, b"\0") + bytes(17)
    return data + struct.pack("<HHHH", 0x6907, 40, 0x10, 32) + hashlib.sha256(data).digest()


def reply(cmd, value=0, status=0, identifier=0x580):
    return SimpleNamespace(arbitration_id=identifier, is_extended_id=False, is_remote_frame=False,
                           is_error_frame=False, is_fd=False,
                           data=bytes([cmd | 0x80, status]) + struct.pack("<I", value) + bytes([1, 0]))


class Bus:
    def __init__(self, replies):
        self.replies, self.sent = list(replies), []

    def send(self, msg, timeout):
        self.sent.append(bytes(msg.data))

    def recv(self, timeout):
        return self.replies.pop(0) if self.replies else None


class Tests(unittest.TestCase):
    def test_image(self):
        valid = image()
        self.assertEqual(uploader.check_image(valid), hashlib.sha256(valid[:529]).hexdigest())
        for bad in [valid[:20], valid[:-1], valid + b"\xff", bytes(600),
                    valid[:512] + b"\x01" + valid[513:]]:
            with self.assertRaises(ValueError):
                uploader.check_image(bad)

    def test_raw_packaging_and_vectors(self):
        raw = struct.pack("<II", 0x20040000, 0x08010209) + b"application"
        packaged = uploader.prepare_image(raw)
        self.assertEqual(packaged[512:512 + len(raw)], raw)
        uploader.check_image(packaged)
        self.assertEqual(uploader.prepare_image(packaged), packaged)
        for invalid in [b"short", struct.pack("<II", 0x20040000, 0x08000009) + b"application",
                        struct.pack("<II", 0x20040001, 0x08010209) + b"application",
                        struct.pack("<II", 0x20040008, 0x08010209) + b"application",
                        struct.pack("<II", 0x20040000, 0x08010208) + b"application",
                        raw + bytes(0x30000)]:
            with self.assertRaises(ValueError):
                uploader.prepare_image(invalid)

    def test_progress_bar(self):
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            uploader.print_progress(50, 100)
            uploader.print_progress(100, 100)
        self.assertIn("[" + "#" * 15 + "-" * 15 + "]  50%", output.getvalue())
        self.assertIn("[" + "#" * 30 + "] 100%", output.getvalue())

    def test_ack_filter_and_retry(self):
        bus = Bus([None, reply(2, 4, identifier=0x581), reply(2, 4, identifier=0x582), reply(2, 4), reply(2, 8)])
        self.assertEqual(uploader.request(bus, bytes([2]) + bytes(7), expected_offset=8), 8)
        self.assertEqual(len(bus.sent), 2)

    def test_rejection(self):
        with self.assertRaisesRegex(RuntimeError, "SHA-256"):
            uploader.request(Bus([reply(3, status=6)]), uploader.command(3))
        with self.assertRaises(TimeoutError):
            uploader.request(Bus([]), uploader.command(0), retries=2)

    def test_enter_boot_sent_once_before_info(self):
        raw = struct.pack("<II", 0x20040000, 0x08010209) + b"application"
        firmware = uploader.prepare_image(raw)
        with patch.object(sys, "argv", ["can_upload.py", "app.bin", "--channel", "TEST"]), \
             patch.object(Path, "read_bytes", return_value=firmware), \
             patch("can.Bus") as create_bus, \
             patch.object(uploader, "request") as probe, \
             patch.object(uploader, "upload") as transfer, \
             contextlib.redirect_stdout(io.StringIO()):
            bus = create_bus.return_value.__enter__.return_value

            def info_after_entry(*args, **kwargs):
                self.assertEqual(bus.send.call_count, 1)
                self.assertEqual(args[1], uploader.command(uploader.INFO))
                if probe.call_count == 1:
                    raise TimeoutError("Application is resetting")
                return 0x30000

            probe.side_effect = info_after_entry
            uploader.main()
            msg = bus.send.call_args.args[0]
            self.assertEqual(msg.arbitration_id, 0x601)
            self.assertFalse(msg.is_extended_id)
            self.assertFalse(msg.is_remote_frame)
            self.assertFalse(msg.is_fd)
            self.assertEqual(msg.dlc, 8)
            self.assertEqual(bytes(msg.data), b"\x01" + bytes(7))
            self.assertEqual(probe.call_count, 2)
            transfer.assert_called_once_with(bus, firmware)

    def test_upload(self):
        firmware = image()
        replies = [reply(0, 0x30000), reply(1)]
        replies += [reply(2, min(i + 4, len(firmware))) for i in range(0, len(firmware), 4)]
        replies += [reply(3, len(firmware)), reply(4, len(firmware))]
        bus = Bus(replies)
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            uploader.upload(bus, firmware)
        percentages = [int(value) for value in re.findall(r"\]\s+(\d+)%", output.getvalue())]
        self.assertEqual(percentages, list(range(101)))
        self.assertEqual(bus.sent[0], uploader.command(0))
        self.assertEqual(bus.sent[-1], uploader.command(4))
        self.assertEqual(bus.sent[-3][4:], firmware[-1:] + b"\xff" * 3)


if __name__ == "__main__":
    unittest.main()
