"""Offline unit tests covering the AP writer hardening in patch/on_camera.py."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

PATCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PATCH_DIR))

import on_camera as oc  # noqa: E402
from prepare_ap import WINDOW_IDS, plan_from_config  # noqa: E402


def make_synthetic_image(size: int = 8192) -> tuple[bytes, bytes, dict, bytes, bytes]:
    base = bytearray((i * 31 + 7) % 256 for i in range(size))
    offsets = (32, 256, 1024, 2048)
    lengths = (4, 16, 8, 8)
    patched = bytearray(base)
    patches = []
    for idx, (name, off, ln) in enumerate(zip(WINDOW_IDS, offsets, lengths)):
        before = bytes(base[off : off + ln])
        after = bytes([(b ^ (0x55 + idx)) for b in before])
        patched[off : off + ln] = after
        patches.append(
            {
                "id": name,
                "file_offset": hex(off),
                "length": ln,
                "original_bytes": before.hex(),
                "target_bytes": after.hex(),
            }
        )
    pilot = bytearray(256)
    pilot[:6] = b"\x7fELF\x02\x01"
    pilot[18:20] = (183).to_bytes(2, "little")
    writer = bytearray(256)
    writer[:6] = b"\x7fELF\x02\x01"
    writer[18:20] = (183).to_bytes(2, "little")
    writer[64:84] = b"pilot_ap_generic_v1\0"
    writer[96:117] = b"pilot_ap_hardened_v2\0"

    patch_map = {
        "schema": "pilot.patch-map.v1",
        "target": {
            "model": "Sony ILCE-6700",
            "firmware": "2.00",
            "binary_size": size,
            "binary_sha256": hashlib.sha256(base).hexdigest(),
            "patched_binary_sha256": hashlib.sha256(patched).hexdigest(),
        },
        "pilot_dso": {
            "size_bytes": len(pilot),
            "sha256": hashlib.sha256(pilot).hexdigest(),
        },
        "flash_writer": {
            "size_bytes": len(writer),
            "sha256": hashlib.sha256(writer).hexdigest(),
        },
        "patches": patches,
    }
    return bytes(base), bytes(patched), patch_map, bytes(pilot), bytes(writer)


class FakeSession:
    def __init__(self, initial_files: dict[str, bytes], on_trigger=None):
        self.files = dict(initial_files)
        self.selected = None
        self.on_trigger = on_trigger

    def select(self, path: str) -> None:
        self.selected = path

    def read_path(self, path: str, missing: bool = False) -> bytes | None:
        if path in self.files:
            return self.files[path]
        if missing:
            return None
        raise RuntimeError(f"missing {path}")

    def jiritsu(self, cmd: int, data: bytes = b"", missing: bool = False) -> bytes:
        assert self.selected is not None
        if cmd == 16:
            assert data.startswith(b"\0\xff\0")
            self.files[self.selected] = data[3:]
            if self.selected == oc.FLASH_FILES["trigger"] and self.on_trigger:
                self.on_trigger(self)
            return b""
        if cmd == 18:
            self.files.pop(self.selected, None)
            return b""
        raise AssertionError(f"unexpected cmd {cmd}")


class HardenedHostTests(unittest.TestCase):
    def test_attempt_isolation_never_leaks_prior_success(self):
        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            oc.save_device_state(
                {
                    "status": "restored_pending_power_cycle",
                    "camera_installation": {"after_sha256": "stale"},
                }
            )
            attempt = oc.begin_device_attempt("restore")
            backup = oc.backup_device_files({"example.bin": b"abc"}, attempt)
            receipt = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt["attempt_id"], attempt["attempt_id"])
            self.assertEqual(receipt["backup_dir"], str(backup))
            self.assertEqual(receipt["status"], "backup_recorded")
            self.assertNotIn("camera_installation", receipt)
            self.assertFalse(receipt["full_recovery_verified"])

    def test_full_file_sha256_gate_rejects_outside_window_modification(self):
        base, patched, patch_map, _, _ = make_synthetic_image()
        self.assertEqual(oc.ap_state(base, patch_map), "baseline")
        self.assertEqual(oc.ap_state(patched, patch_map), "patched")
        corrupted = bytearray(base)
        corrupted[-1] ^= 0xFF
        self.assertEqual(oc.ap_state(bytes(corrupted), patch_map), "unknown")

    def test_classify_owned_files_handles_force_plan_custom_pilot_wbi_and_512b_result(self):
        base, _, patch_map, pilot, writer = make_synthetic_image()
        alt_base = bytearray(base)
        alt_base[-1] ^= 0x01
        eff_map = oc.with_actual_windows(patch_map, bytes(alt_base), "write")
        custom_pilot = bytearray(pilot)
        custom_pilot[-1] ^= 0x77
        long_result = oc.FLASH_HEADER + b"RESULT=FAIL\nreason=" + b"x" * 400 + b"\n"
        files = {
            "trigger": writer,
            "staged": writer,
            "mode": b"purge\n",
            "result": long_result,
            "result_tmp": oc.FLASH_HEADER + b"RESULT=OK\n",
            "plan": plan_from_config(eff_map),
            "pilot": bytes(custom_pilot),
            "wbi_tmp": b"WBI2" + b"\x00" * 508,
        }
        self.assertEqual(oc.classify_owned_files(files, writer, pilot=pilot, plan=plan_from_config(patch_map)), [])

    def test_force_rejects_unknown_bytes_inside_patch_windows_m3(self):
        base, _, patch_map, _, _ = make_synthetic_image()
        bad = bytearray(base)
        bad[32] ^= 0x01  # corrupt window 0 (neither original nor target)
        with self.assertRaisesRegex(RuntimeError, "unknown bytes at patch window"):
            oc.with_actual_windows(patch_map, bytes(bad), "write")

    def test_writer_check_dry_run_m5(self):
        base, _, patch_map, _, writer = make_synthetic_image()

        def trigger_check_ok(sess: FakeSession):
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER
                + b"RESULT=OK\nmode=check\nstate=baseline\nwindow_mask=0x0\n"
                + b"usr_statfs_stage=0\nusr_magic=0xef53\nusr_flags=0x1021\n"
                + b"usr_free=0x1c00000\nwbi_status=wbi2\nwbi_hdr4=0x57424932\n"
            )

        sess = FakeSession({oc.AP_PATH: base}, on_trigger=trigger_check_ok)
        self.assertEqual(oc.run_writer_check(sess, patch_map, writer), 0)
        self.assertNotIn(oc.FLASH_FILES["result"], sess.files)

    def test_same_session_reread_recovers_transient_usb_glitch_m5(self):
        base, patched, patch_map, pilot, writer = make_synthetic_image()

        class FlakySession(FakeSession):
            def __init__(self, initial, on_trigger):
                super().__init__(initial, on_trigger=on_trigger)
                self.ap_reads = 0

            def read_path(self, path: str, missing: bool = False):
                if path == oc.AP_PATH:
                    self.ap_reads += 1
                    if self.ap_reads == 2:
                        # Transient corrupted first read after write
                        corrupted = bytearray(patched)
                        corrupted[-1] ^= 0xFF
                        return bytes(corrupted)
                return super().read_path(path, missing=missing)

        def trigger_write_ok(sess):
            sess.files[oc.AP_PATH] = patched
            sess.files[oc.PILOT_DEVICE_PATH] = pilot
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=OK\nmode=write\nstate=patched\nwbi=cleared\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FlakySession({oc.AP_PATH: base}, on_trigger=trigger_write_ok)
            self.assertEqual(oc.run_write(sess, patch_map, writer, pilot, "write"), 0)
            self.assertEqual(sess.ap_reads, 3)

    def test_wbi_backup_pulled_even_on_write_failure_m2_and_restore_wbi_failure_h1(self):
        base, patched, patch_map, pilot, writer = make_synthetic_image()
        wbi_blob = b"WBI2" + b"\x44" * 508

        def trigger_install_wbi_fail(sess: FakeSession):
            sess.files[oc.WBI_TMP_BACKUP_PATH] = wbi_blob
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=FAIL\nreason=wbi_clear_rolled_back\noperation_rc=-2004\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession({oc.AP_PATH: base}, on_trigger=trigger_install_wbi_fail)
            self.assertEqual(oc.run_write(sess, patch_map, writer, pilot, "write"), 6)
            self.assertEqual((Path(tmp) / "wbi_hdr.bak").read_bytes(), wbi_blob)

        def trigger_restore_wbi_fail(sess: FakeSession):
            sess.files[oc.AP_PATH] = base
            sess.files[oc.WBI_TMP_BACKUP_PATH] = wbi_blob
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=FAIL\nreason=restore_wbi_clear_failed\noperation_rc=-2004\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession({oc.AP_PATH: patched}, on_trigger=trigger_restore_wbi_fail)
            self.assertEqual(oc.run_write(sess, patch_map, writer, b"", "restore"), 6)
            receipt = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt["status"], "ap_restored_wbi_clear_failed")
            self.assertEqual((Path(tmp) / "wbi_hdr.bak").read_bytes(), wbi_blob)

    def test_clean_reports_foreign_sha256_and_clean_foreign_removes_l1(self):
        _, _, _, _, writer = make_synthetic_image()
        foreign = b"FOREIGN_PROBE_BYTES"
        sess = FakeSession({oc.FLASH_FILES["trigger"]: foreign})
        with self.assertRaisesRegex(RuntimeError, oc.sha256(foreign)):
            oc.clean_owned_files(sess, writer, allow_foreign=False)
        oc.clean_owned_files(sess, writer, allow_foreign=True)
        self.assertNotIn(oc.FLASH_FILES["trigger"], sess.files)

    def test_upstream_writer_rejected_when_hardened_required_h2(self):
        _, _, patch_map, _, writer = make_synthetic_image()
        upstream_writer = writer.replace(b"pilot_ap_hardened_v2\0", b"\0" * 21)
        patch_map_up = dict(patch_map)
        patch_map_up["flash_writer"] = {
            "size_bytes": len(upstream_writer),
            "sha256": oc.sha256(upstream_writer),
        }
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "upstream.so"
            p.write_bytes(upstream_writer)
            old_path = oc.FLASH_MODULE_PATH
            try:
                oc.FLASH_MODULE_PATH = p
                with self.assertRaisesRegex(RuntimeError, "pilot_ap_hardened_v2"):
                    oc.load_writer(patch_map_up, require_hardened=True)
            finally:
                oc.FLASH_MODULE_PATH = old_path

    def test_remount_ro_failure_after_rollback_never_misclassified_as_safe(self):
        base, _, patch_map, pilot, writer = make_synthetic_image()

        def trigger_remount_ro_fail(sess: FakeSession):
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER
                + b"RESULT=FAIL\nreason=wbi_clear_rolled_back_remount_ro\noperation_rc=-2004\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession({oc.AP_PATH: base}, on_trigger=trigger_remount_ro_fail)
            rc = oc.run_write(sess, patch_map, writer, pilot, "write")
            self.assertEqual(rc, 6)
            receipt = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt["status"], "failed_remount_ro_do_not_reboot")

    def test_force_write_verifies_effective_after_sha256_and_succeeds(self):
        base, _, patch_map, pilot, writer = make_synthetic_image()
        # Differ outside the 4 windows
        custom_base = bytearray(base)
        custom_base[-1] ^= 0xAB
        custom_patched = bytearray(custom_base)
        for p in patch_map["patches"]:
            off = int(p["file_offset"], 0)
            tgt = bytes.fromhex(p["target_bytes"])
            custom_patched[off : off + len(tgt)] = tgt

        def trigger_write_ok(sess: FakeSession):
            sess.files[oc.AP_PATH] = bytes(custom_patched)
            sess.files[oc.PILOT_DEVICE_PATH] = pilot
            sess.files[oc.WBI_TMP_BACKUP_PATH] = b"WBI2" + b"\x22" * 508
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=OK\nmode=write\nstate=patched\nwbi=cleared\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession({oc.AP_PATH: bytes(custom_base)}, on_trigger=trigger_write_ok)
            rc = oc.run_write(sess, patch_map, writer, pilot, "write", force=True)
            self.assertEqual(rc, 0)
            self.assertTrue((Path(tmp) / "wbi_hdr.bak").is_file())
            self.assertNotIn(oc.WBI_TMP_BACKUP_PATH, sess.files)

    def test_custom_pilot_allowed_for_update_pilot_only(self):
        _, _, patch_map, pilot, _ = make_synthetic_image()
        new_pilot = bytearray(pilot)
        new_pilot[100] ^= 0xFF
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "pilot_new.so"
            p.write_bytes(new_pilot)
            old_path = oc.PILOT_DSO_PATH
            try:
                oc.PILOT_DSO_PATH = p
                with self.assertRaisesRegex(RuntimeError, "differs from the DSO"):
                    oc.load_pilot(patch_map, allow_custom_pilot=False)
                loaded = oc.load_pilot(patch_map, allow_custom_pilot=True)
                self.assertEqual(loaded, bytes(new_pilot))
            finally:
                oc.PILOT_DSO_PATH = old_path

    def test_restore_wbi_refuses_when_patched(self):
        _, patched, patch_map, _, writer = make_synthetic_image()
        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession({oc.AP_PATH: patched})
            with self.assertRaisesRegex(RuntimeError, "refusing --restore-wbi"):
                oc.run_wbi_action(sess, patch_map, writer, "restore-wbi")

    def test_pull_wbi_backup_preserves_conflicting_headers(self):
        wbi1 = b"WBI2" + b"\x11" * 508
        wbi2 = b"WBI2" + b"\x22" * 508
        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            (Path(tmp) / "wbi_hdr.bak").write_bytes(wbi1)
            sess = FakeSession({oc.WBI_TMP_BACKUP_PATH: wbi2})
            oc.pull_wbi_backup(sess)
            self.assertEqual((Path(tmp) / "wbi_hdr.bak").read_bytes(), wbi1)
            alts = list(Path(tmp).glob("wbi_hdr.*.bak"))
            self.assertEqual(len(alts), 1)
            self.assertEqual(alts[0].read_bytes(), wbi2)

    def test_purge_skips_when_already_clean_and_preserves_full_uninstall_backup(self):
        base, patched, patch_map, pilot, writer = make_synthetic_image()
        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess_clean = FakeSession({oc.AP_PATH: base})
            self.assertEqual(oc.run_purge(sess_clean, patch_map, writer), 0)
            receipt = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt["status"], "already_clean")

            # Full-uninstall preserves restore backup_dir even if purge fails
            def trigger_restore_ok(sess: FakeSession):
                sess.files[oc.AP_PATH] = base
                sess.files[oc.FLASH_FILES["result"]] = (
                    oc.FLASH_HEADER + b"RESULT=OK\nmode=restore\nstate=baseline\nremount_ro=0\n"
                )

            def trigger_purge_fail(sess: FakeSession):
                sess.files[oc.FLASH_FILES["result"]] = (
                    oc.FLASH_HEADER + b"RESULT=FAIL\nreason=purge_pilot\noperation_rc=-5\n"
                )

            attempt = oc.begin_device_attempt("full-uninstall")
            sess = FakeSession(
                {oc.AP_PATH: patched, oc.PILOT_DEVICE_PATH: pilot},
                on_trigger=trigger_restore_ok,
            )
            self.assertEqual(
                oc.run_write(sess, patch_map, writer, b"", "restore", attempt=attempt), 0
            )
            restore_backup = attempt["backup_dir"]
            sess.on_trigger = trigger_purge_fail
            self.assertEqual(oc.run_purge(sess, patch_map, writer, attempt=attempt), 6)
            receipt2 = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt2["backup_dir"], restore_backup)
            self.assertEqual(receipt2["restore_backup_dir"], restore_backup)

    def test_mixed_window_classification_check_and_converge_without_force_r1(self):
        base, patched, patch_map, pilot, writer = make_synthetic_image()
        # Window 0 = baseline (offset 32, len 4), windows 1..3 = patched
        mixed = bytearray(patched)
        mixed[32:36] = base[32:36]
        mixed_bytes = bytes(mixed)
        self.assertEqual(oc.ap_state(mixed_bytes, patch_map), "mixed")

        # Non-window corruption in mixed image must still be classified as unknown
        corrupted_mixed = bytearray(mixed_bytes)
        corrupted_mixed[-1] ^= 0xFF
        self.assertEqual(oc.ap_state(bytes(corrupted_mixed), patch_map), "unknown")

        sess_chk = FakeSession({oc.AP_PATH: mixed_bytes})
        self.assertEqual(oc.run_check(sess_chk, patch_map), 3)

        def trigger_restore_mixed(sess: FakeSession):
            sess.files[oc.AP_PATH] = base
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER
                + b"RESULT=OK\nmode=restore\nstate=baseline\nrecovered_mixed=1\nwbi=cleared\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess_r = FakeSession({oc.AP_PATH: mixed_bytes}, on_trigger=trigger_restore_mixed)
            # Converges WITHOUT --force!
            self.assertEqual(oc.run_write(sess_r, patch_map, writer, b"", "restore", force=False), 0)

        def trigger_write_mixed(sess: FakeSession):
            sess.files[oc.AP_PATH] = patched
            sess.files[oc.PILOT_DEVICE_PATH] = pilot
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER
                + b"RESULT=OK\nmode=write\nstate=patched\nrecovered_mixed=1\nwbi=cleared\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess_w = FakeSession({oc.AP_PATH: mixed_bytes}, on_trigger=trigger_write_mixed)
            self.assertEqual(oc.run_write(sess_w, patch_map, writer, pilot, "write", force=False), 0)

    def test_install_on_already_patched_still_invokes_writer_for_wbi_s2(self):
        _, patched, patch_map, pilot, writer = make_synthetic_image()
        wbi_blob = b"WBI2" + b"\x55" * 508
        triggered = []

        def trigger_idempotent_write(sess: FakeSession):
            triggered.append(True)
            sess.files[oc.WBI_TMP_BACKUP_PATH] = wbi_blob
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER
                + b"RESULT=OK\nmode=write\nstate=patched\nalready=1\nwbi=cleared\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession(
                {oc.AP_PATH: patched, oc.PILOT_DEVICE_PATH: pilot},
                on_trigger=trigger_idempotent_write,
            )
            self.assertEqual(oc.run_write(sess, patch_map, writer, pilot, "write"), 0)
            self.assertEqual(len(triggered), 1)
            self.assertEqual((Path(tmp) / "wbi_hdr.bak").read_bytes(), wbi_blob)

    def test_purge_refuses_wbi_not_clear_and_enforces_enoent_verify_s3_l2(self):
        base, _, patch_map, pilot, writer = make_synthetic_image()

        def trigger_purge_wbi_not_clear(sess: FakeSession):
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=FAIL\nreason=wbi_not_clear\nmode=purge\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession(
                {oc.AP_PATH: base, oc.PILOT_DEVICE_PATH: pilot},
                on_trigger=trigger_purge_wbi_not_clear,
            )
            self.assertEqual(oc.run_purge(sess, patch_map, writer), 6)
            receipt = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt["status"], "aborted_wbi_not_clear")

        def trigger_purge_missing_verify(sess: FakeSession):
            sess.files.pop(oc.PILOT_DEVICE_PATH, None)
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=OK\nmode=purge\nstate=baseline\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession(
                {oc.AP_PATH: base, oc.PILOT_DEVICE_PATH: pilot},
                on_trigger=trigger_purge_missing_verify,
            )
            with self.assertRaisesRegex(RuntimeError, "ENOENT verification"):
                oc.run_purge(sess, patch_map, writer)

        def trigger_purge_ok(sess: FakeSession):
            sess.files.pop(oc.PILOT_DEVICE_PATH, None)
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER
                + b"RESULT=OK\nmode=purge\nstate=baseline\nbootstrap_removed=1\nwbi_bak_removed=1\nverify=enoent\nremount_ro=0\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = FakeSession(
                {oc.AP_PATH: base, oc.PILOT_DEVICE_PATH: pilot},
                on_trigger=trigger_purge_ok,
            )
            self.assertEqual(oc.run_purge(sess, patch_map, writer), 0)

    def test_failure_saves_receipt_even_if_wbi_pull_raises(self):
        base, _, patch_map, pilot, writer = make_synthetic_image()

        class DropOnWbiPullSession(FakeSession):
            def read_path(self, path: str, missing: bool = False):
                if path == oc.WBI_TMP_BACKUP_PATH and oc.FLASH_FILES["result"] in self.files:
                    raise RuntimeError("simulated USB drop during post-failure WBI pull")
                return super().read_path(path, missing=missing)

        def trigger_remount_ro_fail(sess: FakeSession):
            sess.files[oc.FLASH_FILES["result"]] = (
                oc.FLASH_HEADER + b"RESULT=FAIL\nreason=wbi_clear_and_remount_ro\noperation_rc=-2004\n"
            )

        with tempfile.TemporaryDirectory() as tmp:
            oc.STATE_DIR = Path(tmp)
            sess = DropOnWbiPullSession({oc.AP_PATH: base}, on_trigger=trigger_remount_ro_fail)
            rc = oc.run_write(sess, patch_map, writer, pilot, "write")
            self.assertEqual(rc, 6)
            receipt = json.loads((Path(tmp) / "device-state.json").read_text("utf-8"))
            self.assertEqual(receipt["status"], "failed_remount_ro_do_not_reboot")


if __name__ == "__main__":
    unittest.main(verbosity=2)
