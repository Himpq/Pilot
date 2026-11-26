#!/usr/bin/env python3
"""Apply the configured CP file patch through the Pilot camera transport."""
import hashlib
import json
import struct
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from pilot_tool.spec import hex_bytes, integer, validate_spec

CP_PATH = "/tmp/jiritsu/../../cp/cpapp-b.bin"
STAGING = ("libJit.so", "cpdelta-staged.so", "cpdelta.mode", "cp.delta", "cpdelta.result")
WRITER = Path(__file__).with_name("cpdelta.so")
WRITER_HASH = "365210a7664b71d819082a80a21a0a7c5610e422c254de22c93d38c133feab7b"
DELTA_SIZE = 9536


def digest(data):
    return hashlib.sha256(data).hexdigest()


def sample_points(spec):
    size = spec["target"]["size_bytes"]
    pts = []
    for pos in (0, size // 4, size // 2, (size * 3) // 4, max(0, size - 64)):
        end = min(size, pos + 64)
        if end > pos:
            pts.append((pos, end - pos))
    for offset, before, _ in windows(spec):
        pts.append((offset, len(before)))
    return pts


def sample_digest(image, spec):
    h = hashlib.sha256()
    for offset, length in sample_points(spec):
        h.update(struct.pack("<QI", offset, length))
        h.update(image[offset:offset + length])
    return h.hexdigest()


def load_config(path):
    data = json.loads(path.read_text(encoding="utf-8-sig"))
    if data.get("schema") != "pilot.patch-map.v1":
        raise ValueError("unsupported camera patch map")
    cp = data.get("cp")
    if not isinstance(cp, dict) or not isinstance(cp.get("patch"), dict):
        raise ValueError("camera patch map requires cp.patch")
    spec = validate_spec(cp["patch"])
    if spec["kind"] != "file" or spec["target"]["component"] != "cpapp-b.bin":
        raise ValueError("CP file patch description required")
    if (spec["target"]["model"], spec["target"]["firmware"]) != (data["target"]["model"], data["target"]["firmware"]):
        raise ValueError("CP description does not match camera model and firmware")
    return spec


def windows(spec):
    return sorted((integer(p["offset"]), hex_bytes(p["before"]), hex_bytes(p["after"])) for p in spec["patches"])


def state(image, spec):
    if len(image) != spec["target"]["size_bytes"]:
        raise ValueError("CP image size mismatch")
    sites = windows(spec)
    if all(image[o:o + len(a)] == a for o, b, a in sites):
        return "installed"
    if all(image[o:o + len(b)] == b for o, b, a in sites):
        return "original"
    return "unknown"


def build_delta(spec):
    sites = windows(spec)
    table, payload = bytearray(), bytearray()
    for offset, before, after in sites:
        table += struct.pack("<QII", offset, len(before), len(payload))
        payload += before + after
    capacity = DELTA_SIZE - 96 - len(table)
    if capacity < len(payload):
        raise ValueError("CP patch exceeds writer capacity")
    header = struct.pack("<8sIIQQ", b"A6700D01", 1, len(sites), spec["target"]["size_bytes"], capacity)
    return header.ljust(96, b"\0") + table + payload.ljust(capacity, b"\0")


def stage(session, name, data):
    session.select("/tmp/jiritsu/" + name)
    session.jiritsu(16, b"\0\xff\0" + data)
    if session.read(name) != data:
        raise RuntimeError(f"staging readback failed: {name}")


def run(session, spec, action, state_dir):
    image = session.read_path(CP_PATH)
    current = state(image, spec)
    actual_sample = sample_digest(image, spec)
    actual_sha = digest(image)
    print(f"CP image: {current} sample_sha256={actual_sample} sha256={actual_sha}")
    if action == "check":
        return
    expected = "original" if action == "install" else "installed"
    if current != expected:
        raise RuntimeError(f"CP patch state must be {expected}; found {current}")
    sample_key = "sample_sha256" if action == "install" else "patched_sample_sha256"
    expected_sample = spec["target"].get(sample_key)
    if expected_sample and actual_sample != expected_sample:
        raise RuntimeError(f"CP sample_sha256 mismatch: expected {expected_sample}, got {actual_sample}")
    expected_sha = spec["target"]["sha256"] if action == "install" else spec["target"]["patched_sha256"]
    if actual_sha != expected_sha:
        raise RuntimeError(f"CP full-image sha256 mismatch: expected {expected_sha}, got {actual_sha}")
    if session.read_path("/tmp/jiritsu/../../etc/ld.so.preload", missing=True):
        raise RuntimeError("ld.so.preload exists")
    for name in STAGING:
        if session.read(name, missing=True) is not None:
            raise RuntimeError(f"staging file already exists: {name}")
    writer = WRITER.read_bytes()
    if digest(writer) != WRITER_HASH:
        raise RuntimeError("CP writer hash mismatch")
    state_dir.mkdir(parents=True, exist_ok=True)
    backup = state_dir / ("cpapp-b-" + time.strftime("%Y%m%d-%H%M%S") + "-" + digest(image)[:12] + ".bin")
    with backup.open("xb") as f:
        f.write(image)
    print(f"Backup: {backup}")
    target = bytearray(image)
    for offset, before, after in windows(spec):
        value = after if action == "install" else before
        target[offset:offset + len(value)] = value
    delta = build_delta(spec)
    stage(session, "cp.delta", delta)
    stage(session, "cpdelta.mode", ("apply\n" if action == "install" else "restore\n").encode("ascii"))
    stage(session, "cpdelta-staged.so", writer)
    session.select("/tmp/jiritsu/libJit.so")
    session.jiritsu(16, b"\0\xff\0" + writer)
    result = None
    for _ in range(240):
        result = session.read("cpdelta.result", missing=True)
        if result is not None:
            break
        time.sleep(0.5)
    if result is None or not result.startswith(b"A6700CPDELTA\n") or b"RESULT=OK" not in result:
        raise RuntimeError(f"CP writer failed or timed out: {result!r}")
    actual = session.read_path(CP_PATH)
    if actual != bytes(target):
        raise RuntimeError("CP full-image readback mismatch")
    for name, expected_data in (("libJit.so", writer), ("cpdelta-staged.so", writer), ("cpdelta.mode", ("apply\n" if action == "install" else "restore\n").encode("ascii")), ("cp.delta", delta), ("cpdelta.result", result)):
        if session.read(name, missing=True) == expected_data:
            session.select("/tmp/jiritsu/" + name)
            session.jiritsu(18, b"\0\xff", missing=True)
    print(f"CP {action} complete sample_sha256={sample_digest(actual, spec)} sha256={digest(actual)}; power-cycle the camera")
