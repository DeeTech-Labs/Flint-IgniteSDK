"""Small USB Companion client for explicit development/device validation commands."""
import hashlib
import json
import struct
import time
import zlib
from pathlib import Path
from build_package import REVISION

METHODS = {v["name"]: v["id"] for v in json.loads(
    (Path(__file__).resolve().parents[1] / "spec/services.json").read_text())["services"]}


def text(value):
    data = value.encode("utf-8")
    return struct.pack("<H", len(data)) + data


class Reader:
    def __init__(self, data): self.data, self.offset = data, 0
    def number(self, size=4):
        if self.offset + size > len(self.data): raise ValueError("truncated response")
        result = int.from_bytes(self.data[self.offset:self.offset+size], "little")
        self.offset += size
        return result
    def text(self):
        size = self.number(2)
        if self.offset + size > len(self.data): raise ValueError("truncated string")
        result = self.data[self.offset:self.offset+size].decode("utf-8")
        self.offset += size
        return result


class DeviceError(RuntimeError):
    def __init__(self, method, code):
        self.code = code
        super().__init__(f"{method}: device error {code}")


class Companion:
    def __init__(self, port):
        import serial
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=.1, write_timeout=3)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = port
        self.serial.open()
        self.sequence, self.buffer = 0, bytearray()
        self.events = {}
        self.capabilities = 0
    def close(self):
        try:
            if self.capabilities & 512:
                self.request("session_close", timeout=.25)
        except Exception:
            # Disconnect cannot undo a commit already published; reconnect must inspect it.
            pass
        finally:
            self.serial.close()
    def request(self, method, data=b"", timeout=5):
        if len(data) > 4096: raise ValueError("payload too large")
        self.sequence = (self.sequence + 1) & 0xffffffff or 1
        ident, kind = self.sequence, METHODS[method]
        wire = struct.pack("<4sBBHII", b"FLCP", 1, 0, kind, ident, len(data)) + data
        self.serial.write(wire + struct.pack("<I", zlib.crc32(wire)))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.buffer.extend(self.serial.read(4096))
            while len(self.buffer) >= 4:
                if self.buffer[:4] != b"FLCP": del self.buffer[0]; continue
                if len(self.buffer) < 16: break
                _, version, reply, method_id, response_id, size = struct.unpack_from("<4sBBHII",self.buffer)
                if version != 1 or reply > 1 or size > 4096: del self.buffer[0]; continue
                if len(self.buffer) < 20 + size: break
                frame = bytes(self.buffer[:20+size]); del self.buffer[:20+size]
                if zlib.crc32(frame[:-4]) != int.from_bytes(frame[-4:],"little"): continue
                if not reply and not response_id and method_id == METHODS["companion_state"]:
                    # Coalesce events even while an unrelated control request is pending.
                    self.events[method_id] = self.decode_demand(frame[16:-4])
                    continue
                if not reply: continue
                if method_id != kind or response_id != ident: continue
                result = Reader(frame[16:-4]); error = result.number(2)
                if error: raise DeviceError(method,error)
                return frame[18:-4]
        raise TimeoutError(f"no response to {method}")
    @staticmethod
    def decode_demand(data):
        r = Reader(data)
        if r.number(2) != 1 or r.number(1) != 1:
            raise ValueError("unsupported Companion event")
        result = {"generation": r.number(8), "state": r.number(1),
                  "consumer_generation": r.number(8), "interval_ms": r.number(),
                  "ttl_ms": r.number()}
        if r.offset != len(data) or result["state"] > 2:
            raise ValueError("invalid Companion event")
        return result
    def subscribe(self, enabled=True):
        self.request("companion_subscribe", struct.pack("<HB", 1, bool(enabled)))
    def bind(self, generation):
        self.request("companion_bind", struct.pack("<HBQ", 1, 1, generation))
    def sample(self, generation, cpu, ram):
        self.request("companion_sample", struct.pack("<HBQII", 1, 1, generation, cpu, ram))
    def hello(self, required=65):
        r = Reader(self.request("hello",struct.pack("<HI",1,REVISION)+text("ignite-sdk-device-test")+struct.pack("<Q",required)))
        result = {"protocol":r.number(2),"revision":r.number(),"build":r.text(),"profile":r.text(),"capabilities":r.number(8)}
        if result["capabilities"] & required != required: raise ValueError("device lacks required capabilities")
        self.capabilities = result["capabilities"]
        return result
    def diagnostics(self):
        r = Reader(self.request("diagnostics"))
        result = {}
        count = r.number(2)
        if count > 128: raise ValueError("too many diagnostics")
        for _ in range(count):
            key, value = r.text(), r.text()
            if key in result: raise ValueError("duplicate diagnostic")
            try: result[key] = int(value)
            except ValueError: result[key] = value
        if r.offset != len(r.data): raise ValueError("trailing diagnostics")
        return result
    def info(self):
        r = Reader(self.request("info"))
        return {"profile":r.text(),"build":r.text(),"internal":r.number(),"external":r.number(),
                "storage":r.number(8),"active":r.text(),"component":r.text(),"epoch":r.number(8),"generation":r.number(8)}
    def install(self, data):
        r = Reader(self.request("install_begin",struct.pack("<I",len(data))+text(hashlib.sha256(data).hexdigest())))
        offset = r.number()
        while offset < len(data):
            chunk = data[offset:offset+1024]
            reply = Reader(self.request("install_chunk",struct.pack("<I",offset)+chunk))
            next_offset = reply.number()
            if next_offset != offset + len(chunk): raise ValueError("unexpected transfer offset")
            offset = next_offset
        return Reader(self.request("install_commit", timeout=45)).text()
    def launch(self, ident, component="main"):
        self.request("launch",text(ident)+text(component))
