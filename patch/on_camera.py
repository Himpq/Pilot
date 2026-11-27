#!/usr/bin/env python3
"""Hardened Pilot AP base installer and recovery controller for Sony ILCE-6700 (Ver.2.00).

Hardening over the Pilot v0.2.1 revision of this entry point:
  1. Per-attempt isolated device-state.json receipts:
     each operation creates a fresh attempt_id and replaces device-state.json atomically
     without merging stale success fields from earlier runs.
  2. Full-file SHA-256 verification gate when target.binary_sha256 / patched_binary_sha256
     are declared in patch.json (prevents blind writes on unexpected firmware builds),
     with consistent effective_map verification when --force is explicitly requested.
  3. Supports hardened writer modes: --purge (zero-residue cleanup after baseline restore)
     and --full-uninstall (single-attempt restore AP + verify baseline + purge).
  4. Strict key=value parser on 512-byte bounded writer results so remount_ro failures
     are NEVER misclassified as safe rollbacks.
  5. Complete staging cleanup including /tmp/jiritsu/pilotflash.wbi.bak and
     /tmp/jiritsu/.pilotflash.result.tmp, and deadlock-free --clean after --force or
     custom --update-pilot failures.
  6. Atomic WBI backup persistence (never silently drops differing headers), baseline
     guard on --restore-wbi, and seamless --update-pilot --pilot <new.so>.
"""

from __future__ import annotations

import argparse
import hashlib
import importlib
import json
import os
import struct
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4

from on_camera_config import add_path_arguments, resolve_paths


PATCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = PATCH_DIR.parent
MAP_PATH = REPO_ROOT / "patch.json"
PILOT_DSO_PATH = REPO_ROOT / "build" / "pilot.so"
FLASH_MODULE_PATH = REPO_ROOT / "patch" / "pilot_ap_flash.so"
STATE_DIR = REPO_ROOT / "build" / "device-state"
AP_PATH = "/tmp/jiritsu/../../usr/lib/appFw.so"
PILOT_DEVICE_PATH = "/tmp/jiritsu/../../usr/lib/pilot.so"
WBI_DEVICE_BACKUP_PATH = "/tmp/jiritsu/../../usr/lib/pilot_wbi_hdr.bak"
WBI_TMP_BACKUP_PATH = "/tmp/jiritsu/pilotflash.wbi.bak"
FLASH_FILES = {
    "trigger": "/tmp/jiritsu/libJit.so",
    "staged": "/tmp/jiritsu/pilotflash-staged.so",
    "mode": "/tmp/jiritsu/pilotflash.mode",
    "result": "/tmp/jiritsu/pilotflash.result",
    "result_tmp": "/tmp/jiritsu/.pilotflash.result.tmp",
    "plan": "/tmp/jiritsu/pilotflash-plan.bin",
    "pilot": "/tmp/jiritsu/pilotflash-pilot.so",
    "wbi_tmp": WBI_TMP_BACKUP_PATH,
    "wbi_tmp_staging": "/tmp/jiritsu/.pilotflash.wbi.bak.tmp",
}
FLASH_HEADER = b"A6700PILOTFLASH\n"
FLASH_RESULT_MAX_BYTES = 512
FLASH_POLL_SECONDS = 120


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def is_valid_aarch64_elf64(data: bytes) -> bool:
    return (
        64 <= len(data) <= 65536
        and data[:6] == b"\x7fELF\x02\x01"
        and int.from_bytes(data[18:20], "little") == 183
    )


def is_valid_pilot_plan(data: bytes) -> bool:
    if len(data) < 88 or not data.startswith(b"PILOTAP1"):
        return False
    version, count, img_size = struct.unpack_from("<IIQ", data, 8)
    if version != 1 or count != 4 or img_size == 0 or img_size > 0x7FFFFFFFFFFFFFFF:
        return False
    payload = 24 + 16 * count
    for i in range(count):
        off, length, reserved = struct.unpack_from("<QII", data, 24 + 16 * i)
        if reserved != 0 or length == 0 or length > 4096 or off + length > img_size:
            return False
        payload += length * 2
    return payload == len(data)


def parse_writer_result(data: bytes) -> dict[str, str]:
    """Parse A6700PILOTFLASH result lines into exact key->value pairs."""
    fields: dict[str, str] = {}
    for line in data.decode("ascii", "replace").splitlines()[1:]:
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k.strip()] = v.strip()
    return fields


def write_bytes_atomic(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp_name = tempfile.mkstemp(prefix=f".{path.name}-", suffix=".tmp", dir=path.parent)
    tmp_path = Path(tmp_name)
    try:
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(tmp_path, path)
    finally:
        tmp_path.unlink(missing_ok=True)


def load_map() -> dict:
    patch_map = json.loads(MAP_PATH.read_text(encoding="utf-8-sig"))
    if patch_map.get("schema") != "pilot.patch-map.v1":
        raise RuntimeError("unsupported patch map")
    return patch_map


def save_device_state(state: dict, state_dir: Path | None = None) -> None:
    """Atomically write the current attempt receipt without merging stale prior state."""
    if state_dir is None:
        state_dir = STATE_DIR
    state_dir.mkdir(parents=True, exist_ok=True)
    state_path = state_dir / "device-state.json"
    payload = (json.dumps(state, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    write_bytes_atomic(state_path, payload)


def begin_device_attempt(operation: str) -> dict:
    attempt = {
        "schema": "pilot.device-state.v1",
        "attempt_id": str(uuid4()),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "last_operation": operation,
        "status": "started",
        "full_recovery_verified": False,
    }
    save_device_state(attempt)
    return attempt


def backup_device_files(
    files: dict[str, bytes | None], attempt: dict | None = None
) -> Path:
    """Preserve exact pre-write firmware bytes under local device state."""
    if attempt is None:
        attempt = begin_device_attempt("backup")
    backup_root = STATE_DIR / "backups"
    backup_root.mkdir(parents=True, exist_ok=True)
    backup = Path(tempfile.mkdtemp(prefix="before-write-", dir=backup_root))
    records = {}
    for name, data in files.items():
        if Path(name).name != name or name in (".", ".."):
            raise ValueError("invalid device backup filename")
        records[name] = (
            None
            if data is None
            else {"size_bytes": len(data), "sha256": sha256(data)}
        )
        if data is not None:
            with (backup / name).open("xb") as stream:
                stream.write(data)
                stream.flush()
                os.fsync(stream.fileno())
    write_bytes_atomic(
        backup / "metadata.json",
        (json.dumps(records, indent=2, ensure_ascii=False) + "\n").encode("utf-8"),
    )
    attempt.update({"backup_dir": str(backup), "status": "backup_recorded"})
    save_device_state(attempt)
    return backup


def load_transport(transport_root: Path):
    transport_root = transport_root.resolve()
    if not (transport_root / "cpload_run.py").is_file():
        raise FileNotFoundError(f"transport module not found under {transport_root}")
    module = sys.modules.get("cpload_run")
    if module is None:
        sys.path.insert(0, str(transport_root))
        module = importlib.import_module("cpload_run")
    module_path = getattr(module, "__file__", None)
    if module_path is None:
        raise RuntimeError("loaded cpload_run module has no source path")
    resolved_module_path = Path(module_path).resolve()
    if not resolved_module_path.is_relative_to(transport_root):
        raise RuntimeError(
            f"cpload_run was loaded from {resolved_module_path}, outside {transport_root}"
        )
    return module


def load_writer(patch_map: dict, require_hardened: bool = False) -> bytes:
    writer = FLASH_MODULE_PATH.read_bytes()
    writer_meta = patch_map.get("flash_writer")
    if writer_meta is not None and (
        len(writer) != writer_meta.get("size_bytes")
        or sha256(writer) != writer_meta.get("sha256")
    ):
        raise RuntimeError("AP writer differs from the artifact recorded in patch.json")
    if not is_valid_aarch64_elf64(writer):
        raise RuntimeError("AP writer is not a valid AArch64 ELF64 module (<= 64 KiB)")
    if b"pilot_ap_generic_v1\0" not in writer:
        raise RuntimeError("generic AP writer required")
    if require_hardened and b"pilot_ap_hardened_v2\0" not in writer:
        raise RuntimeError(
            "this operation requires the hardened AP writer (pilot_ap_hardened_v2)"
        )
    return writer


def load_pilot(patch_map: dict, allow_custom_pilot: bool = False) -> bytes:
    pilot = PILOT_DSO_PATH.read_bytes()
    if not is_valid_aarch64_elf64(pilot):
        raise RuntimeError(
            "pilot.so is not a valid AArch64 ELF64 shared library (64..65536 bytes)"
        )
    pilot_meta = patch_map.get("pilot_dso")
    if pilot_meta is not None and (
        len(pilot) != pilot_meta.get("size_bytes")
        or sha256(pilot) != pilot_meta.get("sha256")
    ):
        if not allow_custom_pilot:
            raise RuntimeError("pilot.so differs from the DSO recorded in patch.json")
        print(
            f"NOTE: using custom pilot.so ({len(pilot)} bytes, sha256={sha256(pilot)}) "
            f"for --update-pilot"
        )
    return pilot


def parse_offset(value: int | str) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def iter_patch_windows(patch_map: dict) -> list[tuple[str, int, bytes, bytes]]:
    windows: list[tuple[str, int, bytes, bytes]] = []
    for entry in patch_map.get("patches", []):
        offset = parse_offset(entry["file_offset"])
        length = int(entry["length"])
        original = bytes.fromhex(entry["original_bytes"])
        target = bytes.fromhex(entry["target_bytes"])
        if offset < 0 or length <= 0 or len(original) != length or len(target) != length:
            raise ValueError(f"invalid patch window: {entry.get('id', 'unknown')}")
        windows.append((str(entry["id"]), offset, original, target))
    return windows


def window_hashes(data: bytes, patch_map: dict) -> dict:
    windows = iter_patch_windows(patch_map)
    if not windows:
        raise ValueError("patch map contains no AP patch windows")
    actual_parts: list[bytes] = []
    baseline_parts: list[bytes] = []
    patched_parts: list[bytes] = []
    mismatches: list[dict] = []
    window_states: list[dict] = []
    baseline_count = 0
    patched_count = 0
    for name, offset, original, target in windows:
        length = len(original)
        if offset + length > len(data):
            raise ValueError(f"patch window {name} exceeds appFw.so size ({len(data)})")
        actual = data[offset : offset + length]
        actual_parts.append(actual)
        baseline_parts.append(original)
        patched_parts.append(target)
        if actual == original:
            wstate = "baseline"
            baseline_count += 1
        elif actual == target:
            wstate = "patched"
            patched_count += 1
        else:
            wstate = "unknown"
            mismatches.append(
                {
                    "id": name,
                    "offset": offset,
                    "length": length,
                    "actual": actual,
                    "expected_original": original,
                    "expected_target": target,
                }
            )
        window_states.append(
            {
                "id": name,
                "offset": offset,
                "length": length,
                "state": wstate,
            }
        )
    return {
        "actual_sha256": sha256(b"".join(actual_parts)),
        "baseline_sha256": sha256(b"".join(baseline_parts)),
        "patched_sha256": sha256(b"".join(patched_parts)),
        "mismatches": mismatches,
        "window_states": window_states,
        "is_mixed": not mismatches and baseline_count > 0 and patched_count > 0,
    }


def _reconstruct_full_image(data: bytes, patch_map: dict, target_patched: bool) -> bytes:
    buf = bytearray(data)
    for _, offset, original, target in iter_patch_windows(patch_map):
        replacement = target if target_patched else original
        buf[offset : offset + len(replacement)] = replacement
    return bytes(buf)


def ap_state(data: bytes, patch_map: dict) -> str:
    """Classify appFw.so using BOTH full-file SHA-256 (when declared) and 4 patch windows.

    Returns one of: 'baseline', 'patched', 'mixed' (all non-window bytes match stock and
    every window is a known baseline/target value, e.g. interrupted 2-stage write), or 'unknown'.
    """
    target = patch_map["target"]
    if len(data) != target["binary_size"]:
        return "unknown"
    hashes = window_hashes(data, patch_map)
    full_sha = sha256(data)
    expected_baseline_full = target.get("binary_sha256")
    expected_patched_full = target.get("patched_binary_sha256")

    if hashes["actual_sha256"] == hashes["baseline_sha256"]:
        if expected_baseline_full and full_sha != expected_baseline_full:
            return "unknown"
        return "baseline"
    if hashes["actual_sha256"] == hashes["patched_sha256"]:
        if expected_patched_full and full_sha != expected_patched_full:
            return "unknown"
        return "patched"
    if hashes["is_mixed"]:
        if expected_baseline_full:
            reconstructed = _reconstruct_full_image(data, patch_map, target_patched=False)
            if sha256(reconstructed) != expected_baseline_full:
                return "unknown"
        elif expected_patched_full:
            reconstructed = _reconstruct_full_image(data, patch_map, target_patched=True)
            if sha256(reconstructed) != expected_patched_full:
                return "unknown"
        return "mixed"
    return "unknown"


def read_ap(session) -> bytes:
    data = session.read_path(AP_PATH, missing=True)
    if data is None:
        raise RuntimeError("cannot read /usr/lib/appFw.so over Jiritsu")
    return data


def read_flash_files(session) -> dict[str, bytes | None]:
    return {
        key: session.read_path(path, missing=True)
        for key, path in FLASH_FILES.items()
    }


def remove_device_file(session, path: str) -> None:
    session.select(path)
    session.jiritsu(18, b"\0\xff", missing=True)


def classify_owned_files(
    files: dict[str, bytes | None],
    writer: bytes,
    pilot: bytes | None = None,
    plan: bytes | None = None,
) -> list[str]:
    problems: list[str] = []
    valid_modes = (
        b"check\n",
        b"write\n",
        b"restore\n",
        b"pilot\n",
        b"clear-wbi\n",
        b"restore-wbi\n",
        b"purge\n",
    )
    for key, data in files.items():
        if data is None:
            continue
        if key in ("trigger", "staged") and data == writer:
            continue
        if key == "pilot" and (
            (pilot is not None and data == pilot) or is_valid_aarch64_elf64(data)
        ):
            continue
        if key == "plan" and (
            (plan is not None and data == plan) or is_valid_pilot_plan(data)
        ):
            continue
        if key == "mode" and data in valid_modes:
            continue
        if (
            key in ("result", "result_tmp")
            and len(data) <= FLASH_RESULT_MAX_BYTES
            and data.startswith(FLASH_HEADER)
        ):
            continue
        if key in ("wbi_tmp", "wbi_tmp_staging") and len(data) == 512 and data[:4] == b"WBI2":
            continue
        problems.append(
            f"{FLASH_FILES[key]} contains an unrecognized file "
            f"({len(data)} bytes, sha256={sha256(data)})"
        )
    return problems


def clean_owned_files(
    session,
    writer: bytes,
    pilot: bytes | None = None,
    plan: bytes | None = None,
    allow_foreign: bool = False,
) -> None:
    files = read_flash_files(session)
    problems = classify_owned_files(files, writer, pilot, plan)
    if problems and not allow_foreign:
        raise RuntimeError(
            "; ".join(problems)
            + " — inspect above SHA-256 first; if confirmed leftover from an earlier probe, "
            "re-run with --clean-foreign --yes"
        )
    if problems and allow_foreign:
        for item in problems:
            print(f"WARNING (--clean-foreign): removing {item}")
    removed = 0
    for key, data in files.items():
        if data is not None:
            remove_device_file(session, FLASH_FILES[key])
            print(f"removed staging file: {FLASH_FILES[key]}")
            removed += 1
    if removed == 0:
        print("no Pilot staging files present on camera")


def ensure_no_flash_files(session) -> None:
    files = read_flash_files(session)
    present = [FLASH_FILES[key] for key, data in files.items() if data is not None]
    if present:
        raise RuntimeError(
            "Pilot flash staging files already exist ("
            + ", ".join(present)
            + "); inspect them, then run --clean"
        )


def stage_pilot_dso(session, pilot: bytes, patch_map: dict) -> bool:
    current = session.read_path(PILOT_DEVICE_PATH, missing=True)
    if current is not None and current != pilot:
        raise RuntimeError(
            "existing /usr/lib/pilot.so differs from this build; inspect before replacing"
        )
    print(
        "persistent bootstrap: "
        + ("verified" if current == pilot else "will install in /usr/lib")
    )
    return current != pilot


def run_flash_module(
    session,
    writer: bytes,
    mode: str,
    patch_map: dict | None = None,
    pilot: bytes | None = None,
) -> bytes:
    ensure_no_flash_files(session)
    staged_path = FLASH_FILES["staged"]
    mode_path = FLASH_FILES["mode"]
    trigger_path = FLASH_FILES["trigger"]

    inputs: dict[str, bytes] = {}
    if mode in ("check", "write", "restore", "purge"):
        if patch_map is None:
            raise ValueError("AP patch map required")
        from prepare_ap import plan_from_config

        inputs["plan"] = plan_from_config(patch_map)
    if mode in ("write", "pilot"):
        if not pilot:
            raise ValueError("pilot.so required")
        inputs["pilot"] = pilot
    for key, data in inputs.items():
        session.select(FLASH_FILES[key])
        session.jiritsu(16, b"\0\xff\0" + data)
        if session.read_path(FLASH_FILES[key], missing=True) != data:
            raise RuntimeError(f"AP writer {key} staging readback mismatch")

    session.select(mode_path)
    session.jiritsu(16, b"\0\xff\0" + (mode + "\n").encode("ascii"))
    if session.read_path(mode_path, missing=True) != (mode + "\n").encode("ascii"):
        raise RuntimeError("AP writer mode staging readback mismatch")
    session.select(staged_path)
    session.jiritsu(16, b"\0\xff\0" + writer)
    if session.read_path(staged_path, missing=True) != writer:
        raise RuntimeError("AP writer staging readback mismatch")

    session.select(trigger_path)
    session.jiritsu(16, b"\0\xff\0" + writer)

    result: bytes | None = None
    for _ in range(FLASH_POLL_SECONDS):
        result = session.read_path(FLASH_FILES["result"], missing=True)
        if result is not None:
            break
        time.sleep(1.0)
    if result is None:
        raise RuntimeError(
            "AP writer produced no result; leave camera powered and preserve pilotflash files"
        )
    if not result.startswith(FLASH_HEADER):
        raise RuntimeError(f"unexpected AP writer result: {result[:32]!r}")

    for key in ("trigger", "staged", "mode"):
        data = session.read_path(FLASH_FILES[key], missing=True)
        if data is None:
            continue
        if key in ("trigger", "staged") and data != writer:
            raise RuntimeError(f"{FLASH_FILES[key]} changed during AP writer run")
        if key == "mode" and data != (mode + "\n").encode("ascii"):
            raise RuntimeError(f"{mode_path} changed during AP writer run")
        remove_device_file(session, FLASH_FILES[key])
    for key, expected in inputs.items():
        current = session.read_path(FLASH_FILES[key], missing=True)
        if current is not None and current != expected:
            raise RuntimeError(f"{FLASH_FILES[key]} changed during AP writer run")
        if current is not None:
            remove_device_file(session, FLASH_FILES[key])
    return result


def remove_result_if_matches(session, result: bytes) -> None:
    current = session.read_path(FLASH_FILES["result"], missing=True)
    if current == result:
        remove_device_file(session, FLASH_FILES["result"])


def print_result(data: bytes) -> None:
    print("AP writer result:")
    for line in data.decode("ascii", "replace").splitlines():
        print(f"  {line}")


def print_window_states(data: bytes, patch_map: dict) -> None:
    if len(data) != patch_map["target"]["binary_size"] or not patch_map.get("patches"):
        return
    hashes = window_hashes(data, patch_map)
    for item in hashes["window_states"]:
        print(
            f"  window: {item['id']} offset={hex(item['offset'])} "
            f"length={item['length']} state={item['state']}"
        )


def print_window_mismatch(data: bytes, patch_map: dict) -> None:
    target = patch_map["target"]
    if len(data) != target["binary_size"]:
        print(
            f"WARNING: appFw.so size {len(data)} != expected {target['binary_size']}"
        )
        return
    hashes = window_hashes(data, patch_map)
    full_sha = sha256(data)
    print("WARNING: appFw.so verification mismatch:")
    print(f"  actual   full sha256:    {full_sha}")
    if target.get("binary_sha256"):
        print(f"  expected baseline full:  {target['binary_sha256']}")
    if target.get("patched_binary_sha256"):
        print(f"  expected patched  full:  {target['patched_binary_sha256']}")
    print(f"  actual   windows sha256: {hashes['actual_sha256']}")
    print(f"  baseline windows sha256: {hashes['baseline_sha256']}")
    print(f"  patched  windows sha256: {hashes['patched_sha256']}")
    print_window_states(data, patch_map)
    for item in hashes["mismatches"]:
        print(
            f"  differ: {item['id']} offset={hex(item['offset'])} length={item['length']}"
        )


def with_actual_windows(patch_map: dict, data: bytes, mode: str) -> dict:
    """Build a consistent effective patch_map for --force with strict per-window validation.

    Every patch window on camera MUST equal either its known original_bytes or target_bytes.
    Refuses to overwrite unknown code at fixed offsets. Preserves exact original_bytes/target_bytes
    for all 4 windows so the hardened writer can converge even from a mixed window state while
    recomputing full-file baseline/patched SHA-256 against the on-camera non-window bytes.
    """
    hashes = window_hashes(data, patch_map)
    if hashes["mismatches"]:
        ids = ", ".join(m["id"] for m in hashes["mismatches"])
        raise RuntimeError(
            f"refusing --force: unknown bytes at patch window(s) [{ids}] "
            "(each window must match known original_bytes or target_bytes)"
        )
    if mode == "write" and hashes["actual_sha256"] == hashes["patched_sha256"]:
        raise RuntimeError(
            "refusing --install --force: all 4 patch windows already match target_bytes "
            "(difference is outside the 4 windows; inspect with --prepare-ap instead)"
        )
    if mode == "restore" and hashes["actual_sha256"] == hashes["baseline_sha256"]:
        raise RuntimeError(
            "refusing --restore --force: all 4 patch windows already match original_bytes "
            "(difference is outside the 4 windows; inspect with --prepare-ap instead)"
        )

    baseline_img = _reconstruct_full_image(data, patch_map, target_patched=False)
    patched_img = _reconstruct_full_image(data, patch_map, target_patched=True)

    updated = dict(patch_map)
    updated_target = dict(patch_map["target"])
    updated_target["binary_sha256"] = sha256(baseline_img)
    updated_target["patched_binary_sha256"] = sha256(patched_img)
    updated["target"] = updated_target
    updated["patches"] = [dict(entry) for entry in patch_map["patches"]]
    return updated


def record_camera_state(
    patch_map: dict,
    mode: str,
    before_hash: str,
    after_hash: str,
    result: bytes,
    attempt: dict | None = None,
) -> None:
    applied = mode == "write"
    target = patch_map["target"]
    baseline_sha = target.get("binary_sha256")
    patched_sha = target.get("patched_binary_sha256")
    if (baseline_sha is None or patched_sha is None) and patch_map.get("patches"):
        windows = iter_patch_windows(patch_map)
        baseline_sha = baseline_sha or sha256(
            b"".join(orig for _, _, orig, _ in windows)
        )
        patched_sha = patched_sha or sha256(
            b"".join(tgt for _, _, _, tgt in windows)
        )
    if attempt is None:
        attempt = begin_device_attempt("install" if applied else "restore")
    attempt.update(
        {
            "status": (
                "applied_pending_power_cycle"
                if applied
                else "ap_restored_pending_power_cycle"
            ),
            "recovery_scope": None if applied else "ap_only",
            "full_recovery_verified": False,
            "target": target.get("model"),
            "firmware": target.get("firmware"),
            "camera_applied": applied,
            "camera_installation": {
                "last_operation": "install" if applied else "restore",
                "baseline_sha256": baseline_sha,
                "patched_sha256": patched_sha,
                "before_sha256": before_hash,
                "after_sha256": after_hash,
                "writer_result": result.decode("ascii", "replace").strip(),
                "activation": "pending_full_power_cycle",
            },
        }
    )
    save_device_state(attempt)


def run_check(session, patch_map: dict) -> int:
    data = read_ap(session)
    state = ap_state(data, patch_map)
    full_sha = sha256(data)
    print(f"appFw.so: {len(data)} bytes sha256={full_sha}")
    if len(data) == patch_map["target"]["binary_size"] and patch_map.get("patches"):
        hashes = window_hashes(data, patch_map)
        print(f"actual windows sha256: {hashes['actual_sha256']}")
    if state == "mixed":
        print_window_states(data, patch_map)
        print(
            "state: mixed (interrupted two-stage write; all 4 windows match known baseline/target bytes — "
            "run --restore --yes or --install --yes to converge)"
        )
        return 3
    if state == "unknown":
        print_window_mismatch(data, patch_map)
        print("state: unknown; no files were written")
        return 3
    print(f"state: {state}")
    bootstrap = session.read_path(PILOT_DEVICE_PATH, missing=True)
    wbi_bak = session.read_path(WBI_DEVICE_BACKUP_PATH, missing=True)
    if state == "patched":
        if bootstrap is None:
            print("persistent bootstrap: missing /usr/lib/pilot.so")
            return 4
        digest = sha256(bootstrap)
        print(f"persistent bootstrap: {len(bootstrap)} bytes sha256={digest}")
        expected = patch_map.get("pilot_dso")
        if expected is not None and (
            len(bootstrap) != expected["size_bytes"] or digest != expected["sha256"]
        ):
            print("persistent bootstrap: mismatch (custom or updated pilot.so)")
            return 4
    else:
        print(
            "persistent bootstrap: "
            + (
                "absent (clean stock)"
                if bootstrap is None
                else f"present ({len(bootstrap)} bytes)"
            )
        )
    print(
        "on-camera WBI backup: "
        + ("absent" if wbi_bak is None else f"present ({len(wbi_bak)} bytes)")
    )
    return 0


def pull_wbi_backup(session) -> None:
    """Atomically save the 512B WBI2 header backup on host without silently discarding conflicts."""
    data = session.read_path(WBI_TMP_BACKUP_PATH, missing=True)
    if not (data and len(data) == 512 and data[:4] == b"WBI2"):
        return
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    dst = STATE_DIR / "wbi_hdr.bak"
    if not dst.exists():
        write_bytes_atomic(dst, data)
        print(f"WBI header backup saved to {dst} (512 bytes, sha256={sha256(data)})")
    else:
        existing = dst.read_bytes()
        if existing == data:
            print(f"WBI header backup already preserved at {dst} (verified identical)")
        else:
            stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
            alt = STATE_DIR / f"wbi_hdr.{stamp}.bak"
            write_bytes_atomic(alt, data)
            print(
                f"WARNING: existing {dst} differs from camera WBI2 header; "
                f"saved new copy to {alt} (sha256={sha256(data)})"
            )
    remove_device_file(session, WBI_TMP_BACKUP_PATH)


def run_writer_check(session, patch_map: dict, writer: bytes) -> int:
    """Read-only dry-run of pilot_ap_flash.so in mode=check on camera.

    Verifies Jiritsu dlopen loading, AArch64 syscalls, patch plan parsing, /usr statfs mount
    flags & free space, and /dev/nflasha5 WBI header status without remounting /usr RW or
    modifying /usr or /dev/nflasha5 (only stages and cleans temporary files under /tmp/jiritsu).
    """
    preload = session.read_path("/tmp/jiritsu/../../etc/ld.so.preload", missing=True)
    if preload:
        raise RuntimeError("/etc/ld.so.preload exists; refusing to invoke Jiritsu loader")
    ensure_no_flash_files(session)
    result = run_flash_module(session, writer, "check", patch_map)
    print_result(result)
    fields = parse_writer_result(result)
    remove_result_if_matches(session, result)
    if fields.get("RESULT") != "OK":
        return 6
    summary = (
        f"state={fields.get('state', 'unknown')}, "
        f"usr_statfs_stage={fields.get('usr_statfs_stage', '?')}, "
        f"usr_free={fields.get('usr_free', '?')}, "
        f"wbi_status={fields.get('wbi_status', '?')}"
    )
    if "wbi_hdr4" in fields:
        summary += f", wbi_hdr4={fields['wbi_hdr4']}"
    print(f"WRITER CHECK OK (read-only on /usr & /dev/nflasha5; {summary})")
    if fields.get("usr_statfs_stage") not in (None, "0"):
        print("WARNING: /usr statfs preflight reported non-zero stage; inspect before --install!")
    if fields.get("wbi_status") in ("unknown", "error"):
        print("WARNING: /dev/nflasha5 header probe reported unknown/error; inspect before --install!")
    return 0


def run_write(
    session,
    patch_map: dict,
    writer: bytes,
    pilot: bytes,
    mode: str,
    force: bool = False,
    attempt: dict | None = None,
) -> int:
    if attempt is None:
        attempt = begin_device_attempt("install" if mode == "write" else "restore")
    preload = session.read_path("/tmp/jiritsu/../../etc/ld.so.preload", missing=True)
    if preload:
        attempt.update({"status": "aborted_preload_present"})
        save_device_state(attempt)
        raise RuntimeError(
            "/etc/ld.so.preload exists; refusing to invoke Jiritsu loader"
        )

    before = read_ap(session)
    before_hash = sha256(before)
    state = ap_state(before, patch_map)
    print(f"before appFw: {len(before)} bytes sha256={before_hash} state={state}")
    effective_map = patch_map
    if state == "mixed":
        print("WARNING: detected mixed patch window state (likely interrupted two-stage write):")
        print_window_states(before, patch_map)
        print(f"Proceeding to converge appFw.so cleanly via mode={mode} (no --force required)")
    elif state == "unknown":
        print_window_mismatch(before, patch_map)
        if len(before) != patch_map["target"]["binary_size"]:
            attempt.update(
                {"status": "aborted_size_mismatch", "observed_ap_sha256": before_hash}
            )
            save_device_state(attempt)
            raise RuntimeError("appFw size does not match target binary_size")
        if not force:
            attempt.update(
                {"status": "aborted_state_unknown", "observed_ap_sha256": before_hash}
            )
            save_device_state(attempt)
            raise RuntimeError(
                "AP binary or patch windows do not match expected baseline/patched/mixed state; "
                "run --prepare-ap first or pass --force only after manual audit"
            )
        print("WARNING: --force enabled; proceeding with validated on-camera window bytes")
        effective_map = with_actual_windows(patch_map, before, mode)

    if mode == "write":
        ensure_no_flash_files(session)
        stage_pilot_dso(session, pilot, patch_map)
        if state == "patched":
            print(
                "install: appFw patch is already present; still running writer to verify pilot.so and clear WBI2"
            )
    else:
        ensure_no_flash_files(session)
        if state == "baseline":
            print(
                "restore: appFw is already at baseline; still running writer to verify/clear WBI2"
            )

    backup_device_files(
        {
            "appFw.so": before,
            "pilot.so": session.read_path(PILOT_DEVICE_PATH, missing=True),
        },
        attempt,
    )
    result = run_flash_module(session, writer, mode, effective_map, pilot)
    print_result(result)
    fields = parse_writer_result(result)
    after = read_ap(session)
    after_hash = sha256(after)
    after_state = ap_state(after, effective_map)

    expected_state = "patched" if mode == "write" else "baseline"
    # If first post-write readback disagrees, re-read once in the same session to rule out USB glitch.
    if after_state != expected_state and fields.get("RESULT") == "OK":
        print("WARNING: first post-write appFw readback mismatched; re-reading once in same session ...")
        after = read_ap(session)
        after_hash = sha256(after)
        after_state = ap_state(after, effective_map)

    print(f"after appFw:  {len(after)} bytes sha256={after_hash} state={after_state}")

    if fields.get("RESULT") != "OK" or after_state != expected_state:
        reason = fields.get("reason", "")
        remount_ro_failed = (
            "remount_ro" in reason or fields.get("recovery") == "do_not_reboot"
        )
        restore_wbi_only_failed = (
            mode == "restore"
            and reason == "restore_wbi_clear_failed"
            and after_state == "baseline"
            and not remount_ro_failed
        )
        clean_rollback = (
            not remount_ro_failed
            and after_hash == before_hash
            and (
                reason == "wbi_clear_rolled_back"
                or (reason == "write_or_readback" and fields.get("state") == "restored")
            )
        )
        if remount_ro_failed:
            status = "failed_remount_ro_do_not_reboot"
        elif restore_wbi_only_failed:
            status = "ap_restored_wbi_clear_failed"
        elif clean_rollback:
            status = "failed_rolled_back_to_before"
        elif after_hash == before_hash:
            status = "failed_unchanged"
        else:
            status = "failed_unexpected_ap_change"

        attempt.update(
            {
                "status": status,
                "before_sha256": before_hash,
                "after_sha256": after_hash,
                "writer_result": result.decode("ascii", "replace").strip(),
            }
        )
        # Save receipt and print CRITICAL/WARNING alert BEFORE pulling WBI backup,
        # so a USB error during backup pull can never suppress the operator alert.
        save_device_state(attempt)
        if remount_ro_failed:
            print(
                "CRITICAL: /usr remount_ro failed (filesystem may still be RW); "
                "DO NOT power-cycle! Preserve result files and inspect before proceeding."
            )
        elif restore_wbi_only_failed:
            print(
                "WARNING: appFw.so IS restored to stock baseline, but WBI2 clear failed. "
                "Run --clear-wbi --yes (in next session) before expecting a cold boot."
            )
        elif after_hash == before_hash:
            print(
                "AP bytes match the pre-run image (unchanged/rolled back cleanly); "
                "inspect result before rebooting."
            )
        else:
            print(
                "CRITICAL: AP image changed unexpectedly; DO NOT power-cycle; "
                "preserve result files and inspect immediately."
            )
        # Best-effort pull of any staged WBI2 header backup to host even on failure.
        try:
            pull_wbi_backup(session)
        except Exception as pull_err:
            print(f"WARNING: failed to pull staged WBI backup after error: {pull_err}")
        return 6

    if mode == "write":
        if session.read_path(PILOT_DEVICE_PATH, missing=True) != pilot:
            attempt.update({"status": "failed_pilot_readback_mismatch"})
            save_device_state(attempt)
            raise RuntimeError("persistent /usr/lib/pilot.so readback mismatch")
        attempt.update(
            {
                "camera_installed_size_bytes": len(pilot),
                "camera_installed_sha256": sha256(pilot),
                "camera_staged_size_bytes": len(pilot),
                "camera_staged_sha256": sha256(pilot),
                "camera_stage_readback_verified": True,
                "camera_stage_activation": "not_confirmed",
                "camera_candidate_state": "staged_readback_verified_power_cycle_pending",
                "camera_persistent_path": "/usr/lib/pilot.so",
            }
        )
        print(
            f"persistent bootstrap readback OK: {len(pilot)} bytes sha256={sha256(pilot)}"
        )

    record_camera_state(effective_map, mode, before_hash, after_hash, result, attempt)
    remove_result_if_matches(session, result)
    pull_wbi_backup(session)
    if mode == "write":
        print(
            "INSTALL OK; fully power-cycle the camera "
            "(off -> remove battery >=5s -> reinsert -> on)"
        )
    else:
        print(
            "RESTORE OK (WBI2 cleared); fully power-cycle the camera "
            "(off -> remove battery >=5s -> reinsert -> on)"
        )
    return 0


def run_purge(
    session, patch_map: dict, writer: bytes, attempt: dict | None = None
) -> int:
    if attempt is None:
        attempt = begin_device_attempt("purge")
    preload = session.read_path("/tmp/jiritsu/../../etc/ld.so.preload", missing=True)
    if preload:
        attempt.update({"status": "aborted_preload_present"})
        save_device_state(attempt)
        raise RuntimeError(
            "/etc/ld.so.preload exists; refusing to invoke Jiritsu loader"
        )

    current_ap = read_ap(session)
    ap_hash = sha256(current_ap)
    state = ap_state(current_ap, patch_map)
    if state != "baseline":
        attempt.update(
            {"status": "aborted_appfw_not_baseline", "observed_ap_sha256": ap_hash}
        )
        save_device_state(attempt)
        raise RuntimeError(
            f"refusing --purge because appFw.so state is {state!r}, not 'baseline'; "
            "run --restore first"
        )

    cur_pilot = session.read_path(PILOT_DEVICE_PATH, missing=True)
    cur_wbi_bak = session.read_path(WBI_DEVICE_BACKUP_PATH, missing=True)
    if cur_pilot is None and cur_wbi_bak is None:
        print("purge: /usr/lib/pilot.so and /usr/lib/pilot_wbi_hdr.bak are already absent")
        attempt.update(
            {
                "status": "already_clean",
                "recovery_scope": "ap_and_bootstrap_purged",
                "camera_applied": False,
                "observed_ap_sha256": ap_hash,
            }
        )
        save_device_state(attempt)
        return 0

    ensure_no_flash_files(session)
    if "backup_dir" not in attempt:
        backup_device_files(
            {"pilot.so": cur_pilot, "pilot_wbi_hdr.bak": cur_wbi_bak},
            attempt,
        )
    else:
        # Preserve prior restore backup_dir inside full-uninstall receipt while still backing up residual files
        prior_backup = attempt["backup_dir"]
        purge_backup = backup_device_files(
            {"pilot.so": cur_pilot, "pilot_wbi_hdr.bak": cur_wbi_bak},
            attempt,
        )
        attempt["restore_backup_dir"] = prior_backup
        attempt["purge_backup_dir"] = str(purge_backup)
        attempt["backup_dir"] = prior_backup
        save_device_state(attempt)

    result = run_flash_module(session, writer, "purge", patch_map)
    print_result(result)
    fields = parse_writer_result(result)
    if fields.get("RESULT") != "OK":
        reason = fields.get("reason", "")
        if reason == "wbi_not_clear":
            print(
                "WARNING: /dev/nflasha5 WBI2 header is not cleared; "
                "run --clear-wbi --yes (in next power cycle) before --purge."
            )
        status = (
            "failed_remount_ro_do_not_reboot"
            if ("remount_ro" in reason or fields.get("recovery") == "do_not_reboot")
            else ("aborted_wbi_not_clear" if reason == "wbi_not_clear" else "purge_failed")
        )
        attempt.update(
            {
                "status": status,
                "purge_writer_result": result.decode("ascii", "replace").strip(),
            }
        )
        save_device_state(attempt)
        return 6

    if (
        fields.get("bootstrap_removed") != "1"
        or fields.get("wbi_bak_removed") != "1"
        or fields.get("verify") != "enoent"
    ):
        attempt.update(
            {
                "status": "purge_camera_verify_incomplete",
                "purge_writer_result": result.decode("ascii", "replace").strip(),
            }
        )
        save_device_state(attempt)
        raise RuntimeError(
            "purge did not report on-camera ENOENT verification (bootstrap_removed/wbi_bak_removed/verify=enoent)"
        )

    rem_pilot = session.read_path(PILOT_DEVICE_PATH, missing=True)
    rem_wbi = session.read_path(WBI_DEVICE_BACKUP_PATH, missing=True)
    if rem_pilot is not None or rem_wbi is not None:
        attempt.update({"status": "purge_verification_failed"})
        save_device_state(attempt)
        raise RuntimeError(
            "purge readback check failed: residual file still present under /usr/lib"
        )

    attempt.update(
        {
            "status": "purged_pending_power_cycle",
            "recovery_scope": "ap_and_bootstrap_purged",
            "full_recovery_verified": False,
            "camera_applied": False,
            "observed_ap_sha256": ap_hash,
            "purge_writer_result": result.decode("ascii", "replace").strip(),
        }
    )
    save_device_state(attempt)
    remove_result_if_matches(session, result)
    print(
        "PURGE OK; /usr/lib/pilot.so and /usr/lib/pilot_wbi_hdr.bak removed; "
        "power-cycle the camera"
    )
    return 0


def run_wbi_action(
    session, patch_map: dict, writer: bytes, mode: str, force: bool = False
) -> int:
    attempt = begin_device_attempt(mode)
    preload = session.read_path("/tmp/jiritsu/../../etc/ld.so.preload", missing=True)
    if preload:
        attempt.update({"status": "aborted_preload_present"})
        save_device_state(attempt)
        raise RuntimeError(
            "/etc/ld.so.preload exists; refusing to invoke Jiritsu loader"
        )

    if mode == "restore-wbi" and not force:
        current_ap = read_ap(session)
        state = ap_state(current_ap, patch_map)
        if state != "baseline":
            attempt.update(
                {
                    "status": "aborted_restore_wbi_not_baseline",
                    "observed_ap_sha256": sha256(current_ap),
                }
            )
            save_device_state(attempt)
            raise RuntimeError(
                f"refusing --restore-wbi while appFw.so state is {state!r} (not 'baseline'); "
                "run --restore first or pass --force only if you know why"
            )

    ensure_no_flash_files(session)
    result = run_flash_module(session, writer, mode)
    print_result(result)
    fields = parse_writer_result(result)
    if fields.get("RESULT") != "OK":
        reason = fields.get("reason", "")
        status = (
            "failed_remount_ro_do_not_reboot"
            if ("remount_ro" in reason or fields.get("recovery") == "do_not_reboot")
            else f"{mode}_failed"
        )
        attempt.update(
            {
                "status": status,
                "writer_result": result.decode("ascii", "replace").strip(),
            }
        )
        save_device_state(attempt)
        if status == "failed_remount_ro_do_not_reboot":
            print(
                "CRITICAL: /usr remount_ro failed (filesystem may still be RW); "
                "DO NOT power-cycle! Preserve result files and inspect before proceeding."
            )
        if mode == "clear-wbi":
            try:
                pull_wbi_backup(session)
            except Exception as pull_err:
                print(f"WARNING: failed to pull staged WBI backup after error: {pull_err}")
        return 6
    if mode == "clear-wbi":
        pull_wbi_backup(session)
    attempt.update(
        {
            "status": f"{mode}_ok_pending_power_cycle",
            "writer_result": result.decode("ascii", "replace").strip(),
        }
    )
    save_device_state(attempt)
    remove_result_if_matches(session, result)
    print(f"{mode.upper()} OK; fully power-cycle the camera now")
    return 0


def run_update_pilot(session, patch_map: dict, writer: bytes, pilot: bytes) -> int:
    attempt = begin_device_attempt("update-pilot")
    preload = session.read_path("/tmp/jiritsu/../../etc/ld.so.preload", missing=True)
    if preload:
        attempt.update({"status": "aborted_preload_present"})
        save_device_state(attempt)
        raise RuntimeError(
            "/etc/ld.so.preload exists; refusing to invoke Jiritsu loader"
        )

    ensure_no_flash_files(session)
    current = session.read_path(PILOT_DEVICE_PATH, missing=True)
    if current == pilot:
        print(
            f"persistent /usr/lib/pilot.so already matches: "
            f"{len(pilot)} bytes sha256={sha256(pilot)}"
        )
        attempt["status"] = "pilot_candidate_already_present"
        save_device_state(attempt)
        return 0

    backup_device_files({"pilot.so": current}, attempt)
    result = run_flash_module(session, writer, "pilot", pilot=pilot)
    print_result(result)
    fields = parse_writer_result(result)
    if fields.get("RESULT") != "OK":
        reason = fields.get("reason", "")
        status = (
            "failed_remount_ro_do_not_reboot"
            if ("remount_ro" in reason or fields.get("recovery") == "do_not_reboot")
            else "update_pilot_failed"
        )
        attempt.update(
            {
                "status": status,
                "writer_result": result.decode("ascii", "replace").strip(),
            }
        )
        save_device_state(attempt)
        return 6
    if session.read_path(PILOT_DEVICE_PATH, missing=True) != pilot:
        attempt.update({"status": "failed_pilot_readback_mismatch"})
        save_device_state(attempt)
        raise RuntimeError("persistent /usr/lib/pilot.so readback mismatch")

    attempt.update(
        {
            "status": "applied_pending_power_cycle",
            "camera_installed_size_bytes": len(pilot),
            "camera_installed_sha256": sha256(pilot),
            "camera_staged_size_bytes": len(pilot),
            "camera_staged_sha256": sha256(pilot),
            "camera_stage_readback_verified": True,
            "camera_stage_activation": "not_confirmed",
            "camera_candidate_state": "staged_readback_verified_power_cycle_pending",
            "camera_persistent_path": "/usr/lib/pilot.so",
            "writer_result": result.decode("ascii", "replace").strip(),
        }
    )
    save_device_state(attempt)
    remove_result_if_matches(session, result)
    print(f"PILOT UPDATE OK: {len(pilot)} bytes sha256={sha256(pilot)}")
    print("fully power-cycle the camera to load the updated /usr/lib/pilot.so")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--check", action="store_true", help="read-only AP & bootstrap hash check (page cache)")
    group.add_argument("--writer-check", action="store_true", help="read-only dry-run of hardened writer (mode=check) on camera")
    group.add_argument("--install", action="store_true", help="stage Pilot and install the AP patch (clears WBI2)")
    group.add_argument("--restore", action="store_true", help="restore the four original AP windows and clear WBI2")
    group.add_argument("--purge", action="store_true", help="remove /usr/lib/pilot.so & WBI backup after baseline restore")
    group.add_argument("--full-uninstall", action="store_true", help="restore AP + clear WBI2 + purge Pilot files")
    group.add_argument("--update-pilot", action="store_true", help="atomically update /usr/lib/pilot.so (hardened writer only)")
    group.add_argument("--clear-wbi", action="store_true", help="back up and clear /dev/nflasha5 WBI2 header (512B)")
    group.add_argument("--restore-wbi", action="store_true", help="restore /dev/nflasha5 WBI2 header when device header is all-zero")
    group.add_argument("--clean", action="store_true", help="remove recognized Pilot flash staging files")
    group.add_argument("--clean-foreign", action="store_true", help="remove ALL staging files under /tmp/jiritsu/ including foreign probes (requires --yes)")
    group.add_argument("--prepare-ap", action="store_true", help="read AP and prepare a camera-specific patch and writer")
    group.add_argument("--cp-check", action="store_true", help="check configured CP patch")
    group.add_argument("--cp-install", action="store_true", help="install configured CP patch")
    group.add_argument("--cp-restore", action="store_true", help="restore configured CP patch")
    parser.add_argument("--yes", action="store_true", help="required for all write/destructive operations")
    parser.add_argument("--force", action="store_true", help="allow AP install/restore when non-window bytes differ (windows still validated)")
    parser.add_argument("--output-dir", type=Path, help="new directory for --prepare-ap")
    add_path_arguments(parser)
    args = parser.parse_args()

    write_op = any(
        (
            args.install,
            args.restore,
            args.purge,
            args.full_uninstall,
            args.update_pilot,
            args.clear_wbi,
            args.restore_wbi,
            args.clean_foreign,
            args.cp_install,
            args.cp_restore,
        )
    )
    if write_op and not args.yes:
        parser.error("write operations require --yes")
    if args.full_uninstall and args.force:
        parser.error("--full-uninstall cannot be combined with --force; run --restore --force first, verify, then --purge")

    explicit_pilot = args.pilot is not None
    paths = resolve_paths(args, REPO_ROOT)
    global MAP_PATH, PILOT_DSO_PATH, FLASH_MODULE_PATH, STATE_DIR
    MAP_PATH = paths.patch_map
    PILOT_DSO_PATH = paths.pilot
    FLASH_MODULE_PATH = paths.writer
    STATE_DIR = paths.state_dir

    if args.prepare_ap:
        if args.output_dir is None:
            parser.error("--prepare-ap requires --output-dir")
        import prepare_ap

        patch_map = load_map()
        pilot = load_pilot(patch_map, allow_custom_pilot=explicit_pilot)
        writer = load_writer(patch_map, require_hardened=True)
        cr = load_transport(paths.transport_root)
        with cr.Session(skip_hasp=True) as session:
            image = read_ap(session)
        result = prepare_ap.prepare(patch_map, image, pilot, args.output_dir, writer)
        if result["already_installed"]:
            print("AP patch windows already match the target; no preparation files created.")
        else:
            print(f"AP preparation: {result['directory']}")
        print(f"before sha256={result['before_sha256']}")
        print(f"after  sha256={result['after_sha256']}")
        for item in result["windows"]:
            print(
                f"  {item['id']} offset={item['file_offset']} length={item['length']} "
                f"state={item['state']} differences={item['different_bytes']}"
            )
        return 0

    if args.cp_check or args.cp_install or args.cp_restore:
        import cp_flash

        spec = cp_flash.load_config(MAP_PATH)
        cr = load_transport(paths.transport_root)
        action = "install" if args.cp_install else "restore" if args.cp_restore else "check"
        with cr.Session(skip_hasp=True) as session:
            cp_flash.run(session, spec, action, STATE_DIR / "cp-backups")
        return 0

    patch_map = load_map()
    # Enforce hardened writer (atomic renameat for pilot.so, 2-stage barrier, WBI clear on restore)
    # on all active flash operations (never allow O_TRUNC upstream writer on --update-pilot / --install / --restore).
    needs_hardened = bool(
        args.writer_check
        or args.install
        or args.restore
        or args.update_pilot
        or args.purge
        or args.full_uninstall
        or args.clear_wbi
        or args.restore_wbi
    )
    writer = None if args.check else load_writer(patch_map, require_hardened=needs_hardened)
    pilot = (
        load_pilot(patch_map, allow_custom_pilot=bool(args.update_pilot and explicit_pilot))
        if (args.install or args.update_pilot)
        else b""
    )

    print(
        f"target: {patch_map['target'].get('model', 'unknown')} "
        f"firmware {patch_map['target'].get('firmware', 'unknown')} /usr/lib/appFw.so"
    )
    if patch_map.get("patches"):
        windows = iter_patch_windows(patch_map)
        print(f"baseline windows sha256: {sha256(b''.join(orig for _, _, orig, _ in windows))}")
        print(f"patched  windows sha256: {sha256(b''.join(tgt for _, _, _, tgt in windows))}")
    if writer is not None:
        tag = "hardened-v2" if b"pilot_ap_hardened_v2\0" in writer else "upstream-v1"
        print(f"AP writer ({tag}): {len(writer)} bytes sha256={sha256(writer)}")
    if pilot:
        print(f"Pilot DSO: {len(pilot)} bytes sha256={sha256(pilot)}")

    cr = load_transport(paths.transport_root)
    with cr.Session(skip_hasp=True) as session:
        if args.check:
            return run_check(session, patch_map)
        if args.writer_check:
            assert writer is not None
            return run_writer_check(session, patch_map, writer)
        if args.clean or args.clean_foreign:
            assert writer is not None
            from prepare_ap import plan_from_config

            staged = read_flash_files(session)
            clean_pilot = None
            if staged["pilot"] is not None:
                try:
                    clean_pilot = load_pilot(patch_map, allow_custom_pilot=explicit_pilot)
                except Exception:
                    clean_pilot = None
            clean_plan = plan_from_config(patch_map) if staged["plan"] is not None else None
            clean_owned_files(
                session, writer, clean_pilot, clean_plan, allow_foreign=bool(args.clean_foreign)
            )
            return 0
        if args.update_pilot:
            assert writer is not None
            return run_update_pilot(session, patch_map, writer, pilot)
        if args.clear_wbi:
            assert writer is not None
            return run_wbi_action(session, patch_map, writer, "clear-wbi", force=args.force)
        if args.restore_wbi:
            assert writer is not None
            return run_wbi_action(session, patch_map, writer, "restore-wbi", force=args.force)
        if args.purge:
            assert writer is not None
            return run_purge(session, patch_map, writer)
        if args.full_uninstall:
            assert writer is not None
            attempt = begin_device_attempt("full-uninstall")
            rc = run_write(session, patch_map, writer, b"", "restore", force=False, attempt=attempt)
            if rc != 0:
                return rc
            return run_purge(session, patch_map, writer, attempt=attempt)
        assert writer is not None
        return run_write(
            session,
            patch_map,
            writer,
            pilot,
            "write" if args.install else "restore",
            force=args.force,
        )


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(4)
