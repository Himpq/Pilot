"""Prepare a per-camera AP patch plan and writer without writing to the camera."""
from __future__ import annotations

import hashlib
import json
import subprocess
from pathlib import Path

WINDOW_IDS = ("post_fsys_mount_branch", "post_fsys_mount_dlopen_island",
              "pt_load0_p_filesz", "pt_load0_p_memsz")


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def c_array(name: str, data: bytes) -> str:
    lines = [f"static const unsigned char {name}[] = {{"]
    for offset in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{x:02x}" for x in data[offset:offset + 16]) + ",")
    return "\n".join(lines + ["};", ""])


def prepare(config: dict, image: bytes, pilot: bytes, output: Path, cc: str) -> dict:
    if config.get("schema") != "pilot.patch-map.v1":
        raise ValueError("unsupported patch map")
    target = config["target"]
    if len(image) != target["binary_size"] or image[:4] != b"\x7fELF":
        raise ValueError("target AP image size or ELF header mismatch")
    if len(pilot) < 64 or len(pilot) > 65536 or pilot[:6] != b"\x7fELF\x02\x01" or int.from_bytes(pilot[18:20], "little") != 183:
        raise ValueError("pilot.so must be an AArch64 ELF64 under 64 KiB")
    entries = {entry["id"]: entry for entry in config["patches"]}
    if len(entries) != len(config["patches"]) or any(name not in entries for name in WINDOW_IDS):
        raise ValueError("AP patch map is missing required windows")
    ranges, windows, report = [], [], []
    candidate = bytearray(image)
    for name in WINDOW_IDS:
        entry = entries[name]
        offset = int(entry["file_offset"], 0) if isinstance(entry["file_offset"], str) else entry["file_offset"]
        length = entry["length"]
        expected = bytes.fromhex(entry["original_bytes"])
        after = bytes.fromhex(entry["target_bytes"])
        if not isinstance(offset, int) or not isinstance(length, int) or offset < 0 or length <= 0 or length > 4096 or offset + length > len(image) or len(expected) != length or len(after) != length:
            raise ValueError(f"invalid patch window: {name}")
        if any(offset < end and start < offset + length for start, end in ranges):
            raise ValueError(f"overlapping patch window: {name}")
        ranges.append((offset, offset + length))
        before = image[offset:offset + length]
        changed = [i for i, (a, b) in enumerate(zip(before, expected)) if a != b]
        report.append({"id": name, "file_offset": hex(offset), "length": length,
                       "state": "original" if before == expected else "target" if before == after else "different",
                       "different_bytes": len(changed),
                       "differences": [{"file_offset": hex(offset + i), "actual": f"{before[i]:02x}",
                                        "declared_original": f"{expected[i]:02x}", "target": f"{after[i]:02x}"} for i in changed]})
        windows.append((name, offset, before, after))
        candidate[offset:offset + length] = after
    if candidate == image:
        return {"already_installed": True, "before_sha256": digest(image),
                "after_sha256": digest(candidate), "windows": report}
    output.mkdir(parents=True, exist_ok=False)
    (output / "appFw.before.so").write_bytes(image)
    (output / "pilot.so").write_bytes(pilot)
    headers = [f'#define PILOT_AP_BASELINE_SHA256 "{digest(image)}"',
               f'#define PILOT_AP_PATCHED_SHA256 "{digest(candidate)}"',
               f'#define PILOT_AP_FILE_SIZE {len(image)}ULL',
               f'#define PILOT_AP_WINDOW_COUNT {len(windows)}',
               f'#define PILOT_AP_MAX_WINDOW_SIZE {max(len(w[2]) for w in windows)}', ""]
    for i, (_, _, before, after) in enumerate(windows):
        headers.extend((c_array(f"pilot_before_{i}", before), c_array(f"pilot_after_{i}", after)))
    headers.append("static const struct pilot_patch_window pilot_patch_windows[] = {")
    for i, (_, offset, before, _) in enumerate(windows):
        headers.append(f"    {{ 0x{offset:x}ULL, sizeof(pilot_before_{i}), pilot_before_{i}, pilot_after_{i} }},")
    headers.append("};\n")
    (output / "pilot_ap_flash_data.h").write_text("\n".join(headers), encoding="utf-8", newline="\n")
    (output / "pilot_bootstrap_data.h").write_text(c_array("pilot_bootstrap", pilot), encoding="utf-8", newline="\n")
    writer = output / "pilot_ap_flash.so"
    source = Path(__file__).with_name("pilot_ap_flash.c")
    command = [cc, "--target=aarch64-linux-gnu", "-fuse-ld=lld", "-std=c11", "-Os", "-march=armv8-a", "-fPIC", "-fvisibility=hidden", "-fno-stack-protector", "-fno-builtin", "-Wall", "-Wextra", "-Werror", "-shared", "-nostdlib", "-Wl,-init,_init", "-Wl,--no-undefined", "-Wl,--build-id=none", "-I", str(output.resolve()), "-o", str(writer.resolve()), str(source.resolve())]
    subprocess.run(command, check=True)
    writer_data = writer.read_bytes()
    if len(writer_data) > 65536 or writer_data[:6] != b"\x7fELF\x02\x01" or int.from_bytes(writer_data[18:20], "little") != 183:
        raise ValueError("generated writer is not an AArch64 ELF64 under 64 KiB")
    plan = {"schema": "pilot.patch-map.v1", "target": {"model": target["model"], "firmware": target["firmware"],
            "binary_size": len(image), "binary_sha256": digest(image), "patched_binary_sha256": digest(candidate)},
            "pilot_dso": {"size_bytes": len(pilot), "sha256": digest(pilot)},
            "flash_writer": {"size_bytes": len(writer_data), "sha256": digest(writer_data)},
            "patches": [{"id": n, "file_offset": hex(o), "length": len(b), "original_bytes": b.hex(), "target_bytes": a.hex()} for n, o, b, a in windows]}
    (output / "patch.json").write_text(json.dumps(plan, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    (output / "preflight-report.json").write_text(json.dumps({"windows": report, "before_sha256": digest(image), "after_sha256": digest(candidate)}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return {"already_installed": False, "directory": str(output.resolve()), "before_sha256": digest(image), "after_sha256": digest(candidate), "windows": report}
