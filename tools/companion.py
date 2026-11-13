"""Small USB Companion client for explicit development/device validation commands."""
import hashlib
import json
import math
import re
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
        if r.offset != len(r.data) or result["protocol"] != 1 or result["revision"] != REVISION:
            raise ValueError("incompatible Companion protocol or contract revision")
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
    @staticmethod
    def _service_identifier(value):
        if (type(value) is not str or not re.fullmatch(r"[a-z0-9_-][a-z0-9._-]{0,63}", value)
                or ".." in value or value.endswith(".")):
            raise ValueError("invalid service identity")
        return value

    def background_services(self, offset=0, limit=8):
        if (type(offset) is not int or type(limit) is not int or not 0 <= offset <= 65535
                or not 1 <= limit <= 8 or offset + limit > 65536):
            raise ValueError("invalid service page")
        r = Reader(self.request("services", struct.pack("<HB", offset, limit)))
        count, more = r.number(1), r.number(1)
        result = {"max_services": r.number(1), "memory_bytes": r.number(),
                  "instructions_per_window": r.number(), "native_calls_per_window": r.number(),
                  "window_ms": r.number(), "low_energy": r.number(1), "more": bool(more), "services": []}
        if (count > limit or more > 1 or (more and not count)
                or not 1 <= result["max_services"] <= 4 or not result["memory_bytes"]
                or not result["instructions_per_window"] or not result["native_calls_per_window"]
                or not 10 <= result["window_ms"] <= 1000 or result["low_energy"] > 1):
            raise ValueError("invalid service policy or page")
        identities = set()
        for _ in range(count):
            row = {"package": r.text(), "component": r.text(), "label": r.text(),
                   "state": r.number(1), "reason": r.number(1), "error": r.number(2),
                   "instance_id": r.number(8), "generation": r.number(8),
                   "used_bytes": r.number(), "peak_bytes": r.number(), "reserved_bytes": r.number()}
            for key in ("callbacks", "instructions", "native_calls", "max_callback_us", "wakeups",
                        "event_rejected", "event_coalesced"):
                row[key] = r.number(8)
            for key in ("execute_granted", "autostart_granted", "autostart"):
                value = r.number(1)
                if value > 1: raise ValueError("invalid service boolean")
                row[key] = bool(value)
            self._service_identifier(row["package"])
            self._service_identifier(row["component"])
            identity = row["package"], row["component"]
            if (identity in identities or len(row["label"].encode("utf-8")) > 64 or "\0" in row["label"]
                    or row["state"] > 4 or row["reason"] > 8 or row["error"] > 16
                    or bool(row["instance_id"]) != bool(row["generation"])):
                raise ValueError("invalid service row")
            identities.add(identity)
            result["services"].append(row)
        if r.offset != len(r.data): raise ValueError("trailing service data")
        result["low_energy"] = bool(result["low_energy"])
        return result

    def service_control(self, action, package, component):
        if type(action) is not int or not 0 <= action <= 6:
            raise ValueError("invalid service action")
        package = self._service_identifier(package)
        component = self._service_identifier(component)
        if self.request("service_control", struct.pack("<B", action) + text(package) + text(component)):
            raise ValueError("trailing service control response")

    def sensor_catalog(self):
        r = Reader(self.request("sensor_catalog"))
        count = r.number(1)
        if count > 16: raise ValueError("too many sensor sources")
        result, ids = [], set()
        for _ in range(count):
            source = {"source_id": r.number(2), "channels": r.number(1),
                "min_interval_ms": r.number(), "default_interval_ms": r.number(),
                "max_interval_ms": r.number(), "stale_after_ms": r.number(),
                "priority": r.number(1), "simulation": r.number(1),
                "temperature_kind": r.number(1), "coordinates": r.number(1),
                "status": r.number(1), "reason": r.number(2), "generation": r.number(8),
                "sample_id": r.number(8), "timestamp_ms": r.number(8), "age_ms": r.number(8)}
            if (not source["source_id"] or source["source_id"] in ids or not 1 <= source["channels"] <= 63
                    or not 1 <= source["min_interval_ms"] <= source["default_interval_ms"] <= source["max_interval_ms"]
                    or not source["stale_after_ms"] or source["simulation"] > 1
                    or source["temperature_kind"] > 2 or source["coordinates"] > 1
                    or source["status"] > 5 or source["reason"] > 16):
                raise ValueError("invalid sensor catalog")
            ids.add(source["source_id"])
            source["simulation"] = bool(source["simulation"])
            result.append(source)
        if r.offset != len(r.data): raise ValueError("trailing sensor catalog")
        return result
    def sensor_diagnostic_start(self, source, channels, interval_ms=1000, lease_ms=5000):
        for value, low, high in ((source, 1, 65535), (channels, 1, 63),
                (interval_ms, 200, 0xffffffff), (lease_ms, 1, 30000)):
            if type(value) is not int or not low <= value <= high:
                raise ValueError("invalid sensor diagnostic lease")
        response = self.request("sensor_diagnostic", struct.pack("<BHBII", 1, source, channels, interval_ms, lease_ms))
        if response: raise ValueError("trailing diagnostic start response")
    def sensor_diagnostic_stop(self):
        if self.request("sensor_diagnostic", b"\0"):
            raise ValueError("trailing diagnostic stop response")
    def sensor_diagnostic_read(self):
        r = Reader(self.request("sensor_diagnostic", b"\2"))
        result = {"source_id": r.number(2), "channels": r.number(1),
            "generation": r.number(8), "sample_id": r.number(8), "timestamp_ms": r.number(8),
            "age_ms": r.number(8), "status": r.number(1), "reason": r.number(2),
            "drops": r.number(), "simulation": r.number(1), "values": {}}
        if (not result["source_id"] or result["channels"] > 63 or result["status"] > 5
                or result["reason"] > 16 or result["simulation"] > 1
                or (result["status"] != 2 and result["channels"])
                or (result["status"] == 2 and result["reason"])):
            raise ValueError("invalid sensor diagnostic snapshot")
        names = ("temperature", "pressure", "humidity", "ambient_light", "acceleration", "angular_velocity")
        for index, name in enumerate(names):
            if not result["channels"] & (1 << index): continue
            if r.offset + 24 > len(r.data): raise ValueError("truncated sensor values")
            xyz = struct.unpack_from("<ddd", r.data, r.offset); r.offset += 24
            if any(not math.isfinite(v) for v in xyz): raise ValueError("non-finite sensor value")
            if index < 4 and (xyz[1] or xyz[2]): raise ValueError("invalid scalar components")
            result["values"][name] = dict(zip(("x", "y", "z"), xyz))
        if r.offset != len(r.data): raise ValueError("trailing sensor values")
        result["simulation"] = bool(result["simulation"])
        return result
    def time_status(self):
        r = Reader(self.request("time_status"))
        result = {"epoch": r.number(8), "state": r.number(1), "error": r.number(2)}
        if r.offset != len(r.data) or result["state"] > 3 or result["error"] > 16:
            raise ValueError("invalid time status")
        return result
