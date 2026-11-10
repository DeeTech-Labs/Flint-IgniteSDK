"""Host client framing without hardware or pyserial."""
import struct
import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from companion import Companion, METHODS, text


def frame(method, ident, reply, payload):
    data = struct.pack("<4sBBHII", b"FLCP", 1, reply, METHODS[method], ident, len(payload)) + payload
    return data + struct.pack("<I", zlib.crc32(data))


class Serial:
    def __init__(self, data): self.data = bytearray(data)
    def write(self, data): return len(data)
    def read(self, count):
        # Exercise fragmentation across headers and payload boundaries.
        data = bytes(self.data[:3]); del self.data[:3]
        return data


def client(data):
    result = Companion.__new__(Companion)
    result.serial, result.sequence, result.buffer, result.events = Serial(data), 0, bytearray(), {}
    return result


class CompanionTests(unittest.TestCase):
    def test_event_between_responses(self):
        def demand(generation):
            return struct.pack("<HBQBQII", 1, 1, generation, 2, 17, 2000, 10000)
        data = frame("companion_state", 0, 0, demand(5))
        corrupt = bytearray(frame("info", 1, 1, b"\0\0bad")); corrupt[-1] ^= 1
        data += corrupt
        data += frame("companion_state", 0, 0, demand(6))
        data += frame("info", 9, 1, b"\0\0unrelated")
        data += frame("info", 1, 1, b"\0\0expected")
        device = client(data)
        self.assertEqual(device.request("info"), b"expected")
        self.assertEqual(len(device.events), 1)
        self.assertEqual(device.events[METHODS["companion_state"]]["generation"], 6)

    def test_text_diagnostics(self):
        data = struct.pack("<HH", 0, 2) + text("requests") + text("123")
        data += text("logic_message") + text("execution limit")
        device = client(frame("diagnostics", 1, 1, data))
        self.assertEqual(device.diagnostics(), {"requests": 123, "logic_message": "execution limit"})

    def test_event_validation(self):
        for payload in (b"", struct.pack("<HBQBQII", 1, 1, 1, 9, 1, 2000, 10000)):
            with self.assertRaises(ValueError): Companion.decode_demand(payload)


if __name__ == "__main__": unittest.main()
