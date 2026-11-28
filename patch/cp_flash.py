#!/usr/bin/env python3
"""Hardened CP UIPC v4 flashing for the ILCE-6700 2.00."""

from __future__ import annotations

import json
import struct
import sys
import tempfile
import time
from datetime import datetime, timezone
from pathlib import Path
from uuid import uuid4


from on_camera import is_valid_aarch64_elf64, parse_writer_result, sha256, write_bytes_atomic

PATCH_DIR = Path(__file__).resolve().parent
REPO_ROOT = PATCH_DIR.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))
from pilot_tool.spec import hex_bytes, integer, validate_spec

WRITER = PATCH_DIR / "pilot_cp_flash.so"
WRITER_HASH = "65802b98ec1efdbffdc60aea27eebb1da9e37704e77d5a667ae590cd955193cc"
LEGACY_STAGING = tuple("/tmp/jiritsu/" + name for name in (
    "cpdelta-staged.so", "cpdelta.mode", "cp.delta", "cpdelta.result",
))

CP_PATH = "/tmp/jiritsu/../../cp/cpapp-b.bin"
PRELOAD_PATH = "/tmp/jiritsu/../../etc/ld.so.preload"
FLASH_FILES = {
    "trigger": "/tmp/jiritsu/libJit.so",
    "staged": "/tmp/jiritsu/cpflash-staged.so",
    "mode": "/tmp/jiritsu/cpflash.mode",
    "plan": "/tmp/jiritsu/cpflash-plan.bin",
    "result": "/tmp/jiritsu/cpflash.result",
    "result_tmp": "/tmp/jiritsu/.cpflash.result.tmp",
}
FLASH_HEADER = b"A6700CPFLASH\n"
FLASH_RESULT_MAX_BYTES = 512
FLASH_POLL_SECONDS = 120


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
    if (spec["target"]["model"], spec["target"]["firmware"]) != ("Sony ILCE-6700", "2.00"):
        raise ValueError("the hardened CP writer supports Sony ILCE-6700 firmware 2.00 only")
    layout = [(offset, len(before)) for _, offset, before, _ in iter_cp_windows(spec)]
    if layout != [(0x39FE52, 4), (0xB0AD68, 568)]:
        raise ValueError("CP patch must use the ILCE-6700 2.00 UIPC v4 hook and handler windows")
    return spec


def iter_cp_windows(spec: dict) -> list[tuple[str, int, bytes, bytes]]:
    windows: list[tuple[str, int, bytes, bytes]] = []
    for entry in sorted(spec["patches"], key=lambda site: integer(site["offset"])):
        offset = integer(entry["offset"])
        before = hex_bytes(entry["before"])
        after = hex_bytes(entry["after"])
        if offset < 0 or len(before) == 0 or len(before) != len(after):
            raise ValueError(f"invalid CP patch window: {entry.get('id', 'unknown')}")
        windows.append((str(entry["id"]), offset, before, after))
    # Window 0 MUST be the 4-byte branch hook; Window 1 MUST be the handler body.
    if len(windows[0][2]) != 4 or len(windows[1][2]) <= 4:
        raise ValueError("CP patch window 0 must be 4-byte branch hook and window 1 handler body")
    return windows


def build_cp_plan(spec: dict) -> bytes:
    file_size = int(spec["target"]["size_bytes"])
    windows = iter_cp_windows(spec)
    header = struct.pack("<8sIIQ", b"PILOTCP1", 1, len(windows), file_size)
    table = bytearray()
    payload = bytearray()
    for _, offset, before, after in windows:
        if offset + len(before) > file_size:
            raise ValueError("CP patch window out of bounds")
        table += struct.pack("<QII", offset, len(before), 0)
        payload += before + after
    return header + bytes(table) + bytes(payload)


def is_valid_cp_plan(data: bytes) -> bool:
    if len(data) < 56 or not data.startswith(b"PILOTCP1"):
        return False
    ver, count, file_size = struct.unpack_from("<IIQ", data, 8)
    if ver != 1 or count != 2 or file_size == 0 or file_size > 0x7FFFFFFFFFFFFFFF:
        return False
    payload = 24 + 16 * count
    lens = []
    for i in range(count):
        off, length, reserved = struct.unpack_from("<QII", data, 24 + 16 * i)
        if reserved != 0 or length == 0 or length > 4096 or off + length > file_size:
            return False
        lens.append(length)
        payload += length * 2
    return payload == len(data) and lens[0] == 4 and lens[1] > 4


def classify_cp_image(data: bytes, spec: dict) -> tuple[str, int, list[dict]]:
    """Classify full cpapp-b.bin bytes against pinned baseline & patched SHA-256.

    Returns (state, window_mask, per_window_details):
      - 'baseline'             (mask=0x0):  full-image SHA-256 == target.sha256
      - 'patched'              (mask=0x3):  full-image SHA-256 == target.patched_sha256
      - 'mixed_safe'           (mask=0x2):  W1 handler patched, W0 branch stock, non-window bytes 100% stock
      - 'mixed_danger_w0_only' (mask=0x1):  W0 branch armed while W1 is stock zeros (DANGER: DO NOT POWER OFF!)
      - 'corrupt_windows'      (mask=0xFF): >=1 window corrupted, BUT all non-window bytes match stock SHA-256
                                            (eligible for emergency --repair-restore --yes!)
      - 'unknown'              (mask=0xFF): size mismatch or non-window bytes differ from stock
    """
    target = spec["target"]
    expected_size = int(target["size_bytes"])
    if len(data) != expected_size:
        return "unknown", 0xFF, []

    windows = iter_cp_windows(spec)
    mask = 0
    has_corrupt = False
    details: list[dict] = []
    reconstructed = bytearray(data)
    for i, (wid, offset, before, after) in enumerate(windows):
        cur = data[offset : offset + len(before)]
        if cur == after:
            mask |= 1 << i
            w_state = "patched"
            reconstructed[offset : offset + len(before)] = before
        elif cur == before:
            w_state = "baseline"
        else:
            has_corrupt = True
            w_state = "corrupt"
            reconstructed[offset : offset + len(before)] = before
        details.append({"id": wid, "offset": offset, "length": len(before), "state": w_state})

    # Always prove EVERY byte outside the 2 windows matches stock A6700 2.00!
    non_window_stock = (sha256(bytes(reconstructed)) == target["sha256"])
    if not non_window_stock:
        return "unknown", 0xFF, details
    if has_corrupt:
        return "corrupt_windows", 0xFF, details

    full_sha = sha256(data)
    if mask == 0:
        if full_sha != target["sha256"]:
            return "unknown", 0xFF, details
        return "baseline", 0, details
    if mask == 3:
        if full_sha != target["patched_sha256"]:
            return "unknown", 0xFF, details
        return "patched", 3, details
    if mask == 2:
        return "mixed_safe", 2, details
    return "mixed_danger_w0_only", 1, details


def print_wbi_remediation(reason: str, *, context: str = "rejected") -> None:
    if context == "repaired":
        header = (
            f"\n[WBI2 后续提示] 紧急修复已成功将 /cp/cpapp-b.bin 恢复为原厂 17.89 MB 字节！\n"
            f"但检测到当前 {reason}（/dev/nflasha5 首 512 字节非全零）；为确保下次开机从冷启动加载原厂 CP，建议："
        )
    elif context == "check":
        header = (
            f"\n[WBI2 预检提示] 当前机内检测到 {reason}（/dev/nflasha5 首 512 字节非全零；正常关机后会重新生成休眠快照）。\n"
            f"在执行正式 CP 安装或恢复之前，请先用 AP 工具单独执行一次上电周期清理 WBI2："
        )
    else:
        header = (
            f"\n[WBI2 门禁提示] CP 写入因 {reason} 被拒绝（/dev/nflasha5 首 512 字节非全零；正常关机后会重新生成休眠快照）。\n"
            f"请先用 AP 工具单独执行一次上电周期清理 WBI2，再继续 CP 安装或恢复："
        )
    print(
        f"{header}\n"
        f"  1) python tools/pilot.py flash clear-wbi --yes\n"
        f"  2) 拔 USB → 关机 → 取出电池 >= 10 秒 → 装回电池开机并保持 USB MSC 模式\n"
        f"     （若正常拨电源开关关机后仍反复报 wbi2，可在 --clear-wbi --yes 完成后不拨电源开关，\n"
        f"       拔掉 USB 静置 2 秒后直接扣出电池 >= 10 秒强制冷断电再开机）\n"
        f"  3) 重新执行本 CP 命令\n",
        file=sys.stderr,
    )


def print_state_advisory(state: str, details: list[dict], *, in_repair: bool = False) -> None:
    if state == "mixed_danger_w0_only":
        if in_repair:
            print(
                "\n[REPAIR] 检测到高危混合态 window_mask=0x1，窗口外全部 17.89 MB 与原厂逐字节一致，正执行强制恢复原厂字节...\n",
                file=sys.stderr,
            )
        else:
            print(
                "\nCRITICAL WARNING: 检测到高危混合态 window_mask=0x1（窗口 0 跳转已激活，窗口 1 处理函数仍为全零）！\n"
                "切勿此时关机或断电！请立刻执行 `cp-repair-restore --yes` 退回原厂。\n",
                file=sys.stderr,
            )
    elif state == "corrupt_windows":
        bad = [d["id"] for d in details if d.get("state") == "corrupt"]
        if in_repair:
            print(
                f"\n[REPAIR] 检测到窗口字节损坏（{', '.join(bad)}），窗口外全部 17.89 MB 与原厂逐字节一致，正执行强制恢复原厂字节...\n",
                file=sys.stderr,
            )
        else:
            print(
                f"\nCRITICAL WARNING: 检测到窗口字节损坏（{', '.join(bad)}），但窗口外全部 17.89 MB 与原厂逐字节一致！\n"
                f"切勿关机或断电！请立刻执行紧急修复恢复原厂字节：\n"
                f"  python tools/pilot.py flash cp-repair-restore --yes\n",
                file=sys.stderr,
            )


def save_device_state(state: dict, state_dir: Path) -> None:
    payload = (json.dumps(state, indent=2, ensure_ascii=False) + "\n").encode("utf-8")
    write_bytes_atomic(state_dir / "cp-device-state.json", payload)


def begin_attempt(operation: str, state_dir: Path) -> dict:
    attempt = {
        "schema": "pilot.cp-device-state.v1",
        "attempt_id": str(uuid4()),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "last_operation": operation,
        "status": "started",
        "full_recovery_verified": False,
    }
    save_device_state(attempt, state_dir)
    return attempt


def backup_cp_image(image: bytes, state_label: str, attempt: dict, state_dir: Path) -> Path:
    backup_root = state_dir / "cp-backups"
    backup_root.mkdir(parents=True, exist_ok=True)
    backup_dir = Path(tempfile.mkdtemp(prefix=f"before-cp-{state_label}-", dir=backup_root))
    img_sha = sha256(image)
    out_file = backup_dir / "cpapp-b.bin"
    write_bytes_atomic(out_file, image)
    # Readback verify host backup before touching camera
    if sha256(out_file.read_bytes()) != img_sha:
        raise RuntimeError("host pre-write backup readback SHA-256 mismatch")
    meta = {
        "component": "cpapp-b.bin",
        "state_before": state_label,
        "size_bytes": len(image),
        "sha256": img_sha,
        "recorded_at_utc": datetime.now(timezone.utc).isoformat(),
    }
    write_bytes_atomic(
        backup_dir / "metadata.json",
        (json.dumps(meta, indent=2, ensure_ascii=False) + "\n").encode("utf-8"),
    )
    attempt.update({"backup_dir": str(backup_dir), "backup_sha256": img_sha, "status": "backup_recorded"})
    save_device_state(attempt, state_dir)
    return backup_dir


def load_writer() -> bytes:
    writer = WRITER.read_bytes()
    if sha256(writer) != WRITER_HASH:
        raise RuntimeError("CP writer hash mismatch")
    if not is_valid_aarch64_elf64(writer) or b"pilot_cp_hardened_v2\0" not in writer:
        raise RuntimeError("the hardened AArch64 CP writer is required")
    return writer


def read_cp_image(session) -> bytes:
    data = session.read_path(CP_PATH, missing=True)
    if data is None:
        raise RuntimeError("cannot read /cp/cpapp-b.bin over Jiritsu")
    return data


def read_flash_files(session) -> dict[str, bytes | None]:
    return {k: session.read_path(p, missing=True) for k, p in FLASH_FILES.items()}


def remove_device_file(session, path: str) -> None:
    session.select(path)
    session.jiritsu(18, b"\0\xff", missing=True)


def is_owned_cp_writer(data: bytes, writer: bytes) -> bool:
    if data == writer:
        return True
    return is_valid_aarch64_elf64(data) and b"pilot_cp_hardened_v2\0" in data


def classify_owned_files(files: dict[str, bytes | None], writer: bytes, plan: bytes) -> list[str]:
    problems: list[str] = []
    for key, data in files.items():
        if data is None:
            continue
        if key in ("trigger", "staged") and is_owned_cp_writer(data, writer):
            continue
        if key == "plan" and (data == plan or is_valid_cp_plan(data)):
            continue
        if key == "mode" and data in (b"check\n", b"apply\n", b"restore\n", b"force-restore\n"):
            continue
        if key == "result" and len(data) <= FLASH_RESULT_MAX_BYTES and data.startswith(FLASH_HEADER):
            continue
        if key == "result_tmp" and len(data) <= FLASH_RESULT_MAX_BYTES and (
            len(data) == 0 or FLASH_HEADER.startswith(data) or data.startswith(FLASH_HEADER)
        ):
            continue
        problems.append(f"{FLASH_FILES[key]} contains an unrecognized file")
    return problems


def clean_owned_files(session, writer: bytes, plan: bytes, *, allow_foreign: bool = False) -> None:
    files = read_flash_files(session)
    if not allow_foreign:
        problems = classify_owned_files(files, writer, plan)
        if problems:
            raise RuntimeError("; ".join(problems))
    for key, data in files.items():
        if data is not None:
            remove_device_file(session, FLASH_FILES[key])
            label = "staging file (foreign-override)" if allow_foreign else "owned staging file"
            print(f"removed {label}: {FLASH_FILES[key]}")


def disarm_trigger_files(session, writer: bytes, plan: bytes) -> None:
    """Immediately disarm trigger/staged/mode/plan right after writer returns result,
    so libJit.so is NEVER left armed during long readback or after failure."""
    for key in ("trigger", "staged", "mode", "plan"):
        path = FLASH_FILES[key]
        cur = session.read_path(path, missing=True)
        if cur is None:
            continue
        if key in ("trigger", "staged") and not is_owned_cp_writer(cur, writer):
            continue
        if key == "plan" and cur != plan and not is_valid_cp_plan(cur):
            continue
        if key == "mode" and cur not in (b"check\n", b"apply\n", b"restore\n", b"force-restore\n"):
            continue
        remove_device_file(session, path)
        print(f"disarmed staging file: {path}")


def ensure_clean_staging(session) -> None:
    files = read_flash_files(session)
    present = [FLASH_FILES[k] for k, v in files.items() if v is not None]
    legacy = [path for path in LEGACY_STAGING if session.read_path(path, missing=True) is not None]
    if legacy:
        raise RuntimeError(f"legacy CP staging remains ({', '.join(legacy)}); inspect before using cp-clean-foreign --yes")
    if present:
        raise RuntimeError(
            f"CP flash staging files already exist ({', '.join(present)}); inspect them, then run cp-clean"
        )


def stage_file(session, path: str, data: bytes, label: str) -> None:
    session.select(path)
    session.jiritsu(16, b"\0\xff\0" + data)
    if session.read_path(path, missing=True) != data:
        raise RuntimeError(f"CP writer {label} staging readback mismatch")


def run_cp_flash_module(session, writer: bytes, plan: bytes, mode: str) -> tuple[bytes, dict[str, str]]:
    ensure_clean_staging(session)
    if session.read_path(PRELOAD_PATH, missing=True) is not None:
        raise RuntimeError("/etc/ld.so.preload exists on camera; refusing to run")

    result: bytes | None = None
    try:
        stage_file(session, FLASH_FILES["plan"], plan, "plan")
        stage_file(session, FLASH_FILES["mode"], (mode + "\n").encode("ascii"), "mode")
        stage_file(session, FLASH_FILES["staged"], writer, "staged writer")
        session.select(FLASH_FILES["trigger"])
        session.jiritsu(16, b"\0\xff\0" + writer)
        deadline = time.monotonic() + FLASH_POLL_SECONDS
        while time.monotonic() < deadline:
            result = session.read_path(FLASH_FILES["result"], missing=True)
            if result is not None:
                break
            time.sleep(0.5)
    finally:
        # C3 fix: ALWAYS disarm libJit.so / staged / mode / plan even on 120s timeout
        # or exception so no armed trigger can remain on persistent /tmp/jiritsu!
        try:
            disarm_trigger_files(session, writer, plan)
        except Exception as exc:
            print(f"WARNING: immediate disarm failed: {exc}", file=sys.stderr)

    if result is None:
        raise RuntimeError(f"CP writer timed out waiting for {FLASH_FILES['result']}")
    if len(result) > FLASH_RESULT_MAX_BYTES or not result.startswith(FLASH_HEADER):
        raise RuntimeError(f"CP writer returned unrecognized result: {result!r}")
    fields = parse_writer_result(result)
    if fields.get("RESULT") == "OK" and fields.get("mode") != mode:
        raise RuntimeError(f"CP writer mode mismatch: expected {mode}, got {fields.get('mode')}")
    return result, fields


def cmd_check(session, spec: dict) -> None:
    image = read_cp_image(session)
    state, mask, details = classify_cp_image(image, spec)
    files = read_flash_files(session)
    leftovers = [FLASH_FILES[k] for k, v in files.items() if v is not None]
    leftovers.extend(path for path in LEGACY_STAGING if session.read_path(path, missing=True) is not None)
    print(f"CP image (/cp/cpapp-b.bin): {len(image)} bytes sha256={sha256(image)}")
    print(f"CP state: {state} (window_mask=0x{mask:x})")
    for d in details:
        print(f"  window {d['id']} @ {d['offset']} ({d.get('length', '?')}B): {d['state']}")
    print_state_advisory(state, details)
    if leftovers:
        print("staging leftovers:", ", ".join(leftovers))
    else:
        print("staging files: clean")


def cmd_writer_check(session, writer: bytes, plan: bytes, state_dir: Path) -> None:
    attempt = begin_attempt("writer-check", state_dir)
    try:
        result, fields = run_cp_flash_module(session, writer, plan, "check")
    except Exception as exc:
        attempt.update({"status": "error", "error": str(exc)})
        save_device_state(attempt, state_dir)
        raise
    print(result.decode("ascii", "replace"), end="")
    try:
        clean_owned_files(session, writer, plan)
    except Exception as exc:
        attempt.update({"status": "clean_failed", "error": str(exc), "writer_fields": fields})
        save_device_state(attempt, state_dir)
        raise
    if fields.get("RESULT") != "OK":
        attempt.update({"status": "failed", "writer_fields": fields})
        save_device_state(attempt, state_dir)
        raise RuntimeError(f"CP writer-check failed: {fields}")
    if fields.get("wbi_status") and fields.get("wbi_status") != "clear":
        print_wbi_remediation(f"wbi_status={fields.get('wbi_status')}", context="check")
    attempt.update({"status": "ok", "writer_fields": fields})
    save_device_state(attempt, state_dir)


def readback_after_failure(session, spec: dict, attempt: dict, state_dir: Path, *, suppress_advisory: bool = False) -> str | None:
    """S2: Re-read full CP image after any writer failure/timeout and report exact per-window state."""
    try:
        post_img = read_cp_image(session)
        post_state, post_mask, post_details = classify_cp_image(post_img, spec)
        post_sha = sha256(post_img)
        print(f"[POST-FAILURE READBACK] state={post_state} window_mask=0x{post_mask:x} sha256={post_sha}", file=sys.stderr)
        for d in post_details:
            print(f"  window {d['id']} @ {d['offset']}: {d['state']}", file=sys.stderr)
        if not suppress_advisory:
            print_state_advisory(post_state, post_details)
        attempt.update({
            "post_failure_state": post_state,
            "post_failure_window_mask": f"0x{post_mask:x}",
            "post_failure_sha256": post_sha,
            "post_failure_windows": post_details,
        })
        save_device_state(attempt, state_dir)
        return post_state
    except Exception as rb_exc:
        print(f"[POST-FAILURE READBACK] failed to re-read CP image: {rb_exc}", file=sys.stderr)
        return None


def attempt_in_session_auto_repair(session, spec: dict, writer: bytes, plan: bytes, attempt: dict, post_state: str, state_dir: Path) -> bool:
    """N1 fix: When a failure leaves a dangerous state (corrupt_windows or mixed_danger_w0_only),
    immediately attempt force-restore INSIDE THE SAME USB SERVICE SESSION so recovery does not
    depend on opening a second service session in the same power cycle."""
    print(
        f"\n[AUTO-REPAIR] 检测到高危状态 {post_state}，立即在当前服务会话内自动执行 force-restore 紧急恢复原厂字节（切勿拔线或断电）...",
        file=sys.stderr,
    )
    attempt["auto_repair_attempted"] = True
    attempt["auto_repair_trigger_state"] = post_state
    save_device_state(attempt, state_dir)
    try:
        try:
            clean_owned_files(session, writer, plan)
        except Exception as clean_exc:
            print(f"[AUTO-REPAIR] pre-repair cleanup note: {clean_exc}", file=sys.stderr)
        rep_result, rep_fields = run_cp_flash_module(session, writer, plan, "force-restore")
        print(rep_result.decode("ascii", "replace"), end="")
        try:
            clean_owned_files(session, writer, plan)
        except Exception as clean_exc:
            print(f"[AUTO-REPAIR] post-repair cleanup note: {clean_exc}", file=sys.stderr)
        rep_img = read_cp_image(session)
        rep_state, rep_mask, rep_details = classify_cp_image(rep_img, spec)
        rep_sha = sha256(rep_img)
        stock_sha = spec["target"]["sha256"]
        ok = (rep_fields.get("RESULT") == "OK" and rep_state == "baseline" and rep_sha == stock_sha)
        attempt.update({
            "auto_repair_ok": ok,
            "auto_repair_fields": rep_fields,
            "auto_repair_after_state": rep_state,
            "auto_repair_after_window_mask": f"0x{rep_mask:x}",
            "auto_repair_after_sha256": rep_sha,
            "full_recovery_verified": ok,
        })
        save_device_state(attempt, state_dir)
        if ok:
            print(
                f"\n[AUTO-REPAIR VERIFIED OK] 同会话自动修复已成功将 /cp/cpapp-b.bin 恢复至原厂 17.89 MB (sha256={rep_sha})！\n"
                f"请保留回执，并按 CP 恢复流程核对 WBI2 和冷启动状态。\n",
                file=sys.stderr,
            )
            if rep_fields.get("wbi_status") and rep_fields.get("wbi_status") != "clear":
                print_wbi_remediation(f"wbi_status={rep_fields.get('wbi_status')}", context="repaired")
            return True
        print(
            f"\n[AUTO-REPAIR FAILED] 同会话自动修复未完全收敛：state={rep_state} sha256={rep_sha} fields={rep_fields}",
            file=sys.stderr,
        )
        print_state_advisory(rep_state, rep_details)
        return False
    except Exception as rep_exc:
        attempt.update({"auto_repair_ok": False, "auto_repair_error": str(rep_exc)})
        save_device_state(attempt, state_dir)
        print(f"\n[AUTO-REPAIR FAILED] 同会话自动修复异常：{rep_exc}", file=sys.stderr)
        return False


def cmd_mutate(session, spec: dict, writer: bytes, plan: bytes, state_dir: Path, *, is_install: bool, repair_restore: bool = False) -> None:
    op = "repair-restore" if repair_restore else ("install" if is_install else "restore")
    mode = "force-restore" if repair_restore else ("apply" if is_install else "restore")
    target_state = "patched" if is_install else "baseline"
    target_sha = spec["target"]["patched_sha256"] if is_install else spec["target"]["sha256"]

    attempt = begin_attempt(op, state_dir)

    # Preflight staging & preload BEFORE downloading 17.89 MB or creating backup dirs
    try:
        ensure_clean_staging(session)
        if session.read_path(PRELOAD_PATH, missing=True) is not None:
            raise RuntimeError("/etc/ld.so.preload exists on camera; refusing to run")
    except Exception as exc:
        attempt.update({"status": "refused", "error": str(exc)})
        save_device_state(attempt, state_dir)
        raise

    before_img = read_cp_image(session)
    before_state, before_mask, details = classify_cp_image(before_img, spec)
    print(f"Before {op}: state={before_state} window_mask=0x{before_mask:x} sha256={sha256(before_img)}")
    for d in details:
        print(f"  window {d['id']} @ {d['offset']}: {d['state']}")
    print_state_advisory(before_state, details, in_repair=repair_restore)

    if before_state == "unknown":
        attempt.update({"status": "refused", "reason": "cp_image_unknown", "before_sha256": sha256(before_img)})
        save_device_state(attempt, state_dir)
        raise RuntimeError("CP image is unrecognized (size or non-window bytes do not match stock A6700 2.00); refusing even repair-restore")

    if before_state == "corrupt_windows" and not repair_restore:
        attempt.update({"status": "refused", "reason": "cp_corrupt_windows", "before_sha256": sha256(before_img)})
        save_device_state(attempt, state_dir)
        raise RuntimeError("CP patch windows are corrupted (non-window bytes are stock); run cp-repair-restore --yes to force-restore stock bytes")

    # S3: Already at exact target state -> idempotent 0-write short-circuit (no WBI2 gate needed!)
    if before_state == target_state:
        attempt.update({
            "status": "ok",
            "idempotent": True,
            "before_state": before_state,
            "after_state": before_state,
            "after_window_mask": f"0x{before_mask:x}",
            "after_sha256": sha256(before_img),
            "full_recovery_verified": (not is_install and before_state == "baseline"),
        })
        save_device_state(attempt, state_dir)
        print(f"CP {op} IDEMPOTENT OK: already {before_state} ({sha256(before_img)}), 0 writes performed.")
        return

    expected = bytearray(before_img)
    for _, offset, before, after in iter_cp_windows(spec):
        expected[offset:offset + len(before)] = after if is_install else before
    if sha256(expected) != target_sha:
        raise RuntimeError("CP patch windows do not produce the configured target SHA-256")

    backup_dir = backup_cp_image(before_img, before_state, attempt, state_dir)
    print(f"Full CP image pre-write backup verified: {backup_dir / 'cpapp-b.bin'}")

    try:
        result, fields = run_cp_flash_module(session, writer, plan, mode)
    except Exception as exc:
        attempt.update({"status": "error", "before_state": before_state, "error": str(exc)})
        save_device_state(attempt, state_dir)
        post_state = readback_after_failure(session, spec, attempt, state_dir, suppress_advisory=not repair_restore)
        if not repair_restore and post_state in ("corrupt_windows", "mixed_danger_w0_only"):
            repaired = attempt_in_session_auto_repair(session, spec, writer, plan, attempt, post_state, state_dir)
            if repaired:
                raise RuntimeError(f"CP {op} failed ({exc}), AUTO-REPAIRED IN-SESSION back to verified stock baseline") from exc
        raise
    print(result.decode("ascii", "replace"), end="")

    if fields.get("RESULT") != "OK":
        reason = fields.get("reason", "unknown")
        attempt.update({
            "status": "writer_failed",
            "before_state": before_state,
            "writer_fields": fields,
        })
        save_device_state(attempt, state_dir)
        if reason in ("wbi_not_clear", "wbi_nonzero_other", "wbi_probe_fail"):
            print_wbi_remediation(reason, context="rejected")
        if fields.get("rollback") in ("failed", "skipped_danger_w0_armed"):
            print("CRITICAL WARNING: on-camera writer did not restore a safe state! Inspecting post-failure readback...", file=sys.stderr)
        try:
            clean_owned_files(session, writer, plan)
        except Exception as exc:
            print(f"WARNING: post-failure staging cleanup failed: {exc}", file=sys.stderr)
        post_state = readback_after_failure(session, spec, attempt, state_dir, suppress_advisory=not repair_restore)
        if not repair_restore and post_state in ("corrupt_windows", "mixed_danger_w0_only"):
            repaired = attempt_in_session_auto_repair(session, spec, writer, plan, attempt, post_state, state_dir)
            if repaired:
                raise RuntimeError(f"CP {op} failed on camera ({fields}), AUTO-REPAIRED IN-SESSION back to verified stock baseline")
        raise RuntimeError(f"CP {op} failed on camera: {fields}")

    # Full 17.89 MB readback verification over Jiritsu (with 1 transient USB retry)
    after_img = read_cp_image(session)
    after_sha = sha256(after_img)
    after_state, after_mask, _ = classify_cp_image(after_img, spec)
    if after_state != target_state or after_sha != target_sha:
        print("WARNING: first full-image readback mismatched; retrying once in same session ...", file=sys.stderr)
        after_img = read_cp_image(session)
        after_sha = sha256(after_img)
        after_state, after_mask, _ = classify_cp_image(after_img, spec)

    if after_state != target_state or after_sha != target_sha:
        attempt.update({
            "status": "readback_mismatch",
            "expected_sha256": target_sha,
            "actual_sha256": after_sha,
            "after_state": after_state,
            "writer_fields": fields,
        })
        save_device_state(attempt, state_dir)
        raise RuntimeError(f"CP full-image readback mismatch: got {after_state} ({after_sha}), expected {target_state} ({target_sha})")

    try:
        clean_owned_files(session, writer, plan)
    except Exception as exc:
        attempt.update({
            "status": "clean_failed",
            "before_state": before_state,
            "after_state": after_state,
            "after_window_mask": f"0x{after_mask:x}",
            "after_sha256": after_sha,
            "writer_fields": fields,
            "error": str(exc),
        })
        save_device_state(attempt, state_dir)
        raise

    attempt.update({
        "status": "ok",
        "before_state": before_state,
        "after_state": after_state,
        "after_window_mask": f"0x{after_mask:x}",
        "after_sha256": after_sha,
        "writer_fields": fields,
        "full_recovery_verified": (not is_install and after_state == "baseline"),
    })
    save_device_state(attempt, state_dir)
    print(f"CP {op} VERIFIED OK: state={after_state} sha256={after_sha}")
    if repair_restore and fields.get("wbi_status") and fields.get("wbi_status") != "clear":
        print_wbi_remediation(f"wbi_status={fields.get('wbi_status')}", context="repaired")
    print("Next step: unplug USB, power OFF, remove battery >= 10s, then power ON for cold boot.")


def run(session, spec: dict, action: str, state_dir: Path) -> None:
    if action == "check":
        cmd_check(session, spec)
        return
    if action == "clean-foreign":
        clean_owned_files(session, b"", b"", allow_foreign=True)
        for path in LEGACY_STAGING:
            if session.read_path(path, missing=True) is not None:
                remove_device_file(session, path)
                print(f"removed legacy CP staging file: {path}")
        return
    writer = load_writer()
    plan = build_cp_plan(spec)
    if action == "writer-check":
        cmd_writer_check(session, writer, plan, state_dir)
    elif action == "clean":
        clean_owned_files(session, writer, plan)
    elif action in ("install", "restore", "repair-restore"):
        cmd_mutate(session, spec, writer, plan, state_dir,
                   is_install=action == "install", repair_restore=action == "repair-restore")
    else:
        raise ValueError(f"unknown CP action: {action}")
