#!/usr/bin/env python3
"""Host unit and fault-injection tests for the hardened CP flash flow."""

from __future__ import annotations

import hashlib
import struct
import sys
import tempfile
import unittest
from pathlib import Path

PATCH_DIR = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PATCH_DIR))
import cp_flash as cp  # noqa: E402


def make_test_spec(img_size: int = 4096):
    w0_b = bytes.fromhex("307818b1")
    w0_a = bytes.fromhex("6af389b7")
    w1_b = b"\x00" * 32
    w1_a = bytes(range(0xA0, 0xC0))
    base = bytearray((i * 13) & 0xFF for i in range(img_size))
    base[128:132] = w0_b
    base[1024:1056] = w1_b
    patched = bytearray(base)
    patched[128:132] = w0_a
    patched[1024:1056] = w1_a
    spec = {
        "schema": "pilot.cp-patch-map.v1",
        "id": "test-cp-uipc-v4",
        "target": {
            "model": "Sony ILCE-6700",
            "firmware": "2.00",
            "component": "cpapp-b.bin",
            "size_bytes": img_size,
            "sha256": hashlib.sha256(base).hexdigest(),
            "patched_sha256": hashlib.sha256(patched).hexdigest(),
        },
        "patches": [
            {"id": "receiver-hook", "offset": 128, "before": w0_b.hex(), "after": w0_a.hex()},
            {"id": "uipc-v4-handler", "offset": 1024, "before": w1_b.hex(), "after": w1_a.hex()},
        ],
    }
    return spec, bytes(base), bytes(patched)


class FakeSession:
    def __init__(self, cp_bytes: bytes, *, force_result: bytes | None = None,
                 leave_corrupt: bool = False, simulate_timeout: bool = False):
        self.files: dict[str, bytes] = {cp.CP_PATH: cp_bytes}
        self.selected: str | None = None
        self.force_result = force_result
        self.leave_corrupt = leave_corrupt
        self.simulate_timeout = simulate_timeout

    def select(self, path: str) -> None:
        self.selected = path

    def read_path(self, path: str, missing: bool = False) -> bytes | None:
        return self.files.get(path)

    def jiritsu(self, cmd: int, data: bytes, missing: bool = False) -> None:
        assert self.selected is not None
        if cmd == 18:
            self.files.pop(self.selected, None)
            return
        self.files[self.selected] = data[3:]
        if self.selected == cp.FLASH_FILES["trigger"]:
            self._simulate_writer()

    def _simulate_writer(self) -> None:
        if self.simulate_timeout:
            return
        if self.force_result is not None:
            self.files[cp.FLASH_FILES["result"]] = self.force_result
            return
        mode = self.files[cp.FLASH_FILES["mode"]].decode("ascii").strip()
        plan = self.files[cp.FLASH_FILES["plan"]]
        _, _, cnt, fsz = struct.unpack_from("<8sIIQ", plan, 0)
        assert cnt == 2 and len(self.files[cp.CP_PATH]) == fsz
        img = bytearray(self.files[cp.CP_PATH])
        pay = 24 + 16 * cnt
        wins = []
        for i in range(cnt):
            off, ln, _ = struct.unpack_from("<QII", plan, 24 + 16 * i)
            bef = plan[pay : pay + ln]
            aft = plan[pay + ln : pay + 2 * ln]
            pay += 2 * ln
            wins.append((off, ln, bef, aft))
        if mode == "check":
            self.files[cp.FLASH_FILES["result"]] = (
                b"A6700CPFLASH\nRESULT=OK\nmode=check\nstate=baseline\n"
                b"window_mask=0x0\ncp_magic=0x4d44\ncp_ro=0\nwbi_status=clear\n"
            )
            return
        if mode == "apply":
            for off, ln, _, aft in reversed(wins):
                img[off : off + ln] = aft
            if self.leave_corrupt:
                img[0] ^= 0xFF
            self.files[cp.CP_PATH] = bytes(img)
            self.files[cp.FLASH_FILES["result"]] = (
                b"A6700CPFLASH\nRESULT=OK\nmode=apply\nstate=patched\nbarrier=2stage\n"
            )
        elif mode in ("restore", "force-restore"):
            for off, ln, bef, _ in wins:
                img[off : off + ln] = bef
            self.files[cp.CP_PATH] = bytes(img)
            self.files[cp.FLASH_FILES["result"]] = (
                f"A6700CPFLASH\nRESULT=OK\nmode={mode}\nstate=baseline\nbarrier=2stage\n".encode("ascii")
            )


class CpOnCameraTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.state_dir = Path(self.tmp.name) / "state"
        self.spec, self.base, self.patched = make_test_spec()
        self.plan = cp.build_cp_plan(self.spec)
        # Minimal fake writer bytes (ELF header + marker)
        self.writer = b"\x7fELF\x02\x01" + b"\x00" * 12 + (183).to_bytes(2, "little") + b"\x00" * 44 + b"pilot_cp_hardened_v2\x00"

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def test_01_classify_states(self):
        st, m, _ = cp.classify_cp_image(self.base, self.spec)
        self.assertEqual((st, m), ("baseline", 0))
        st, m, _ = cp.classify_cp_image(self.patched, self.spec)
        self.assertEqual((st, m), ("patched", 3))
        # Safe mixed (W1 patched, W0 baseline -> mask 0x2)
        mixed_safe = bytearray(self.base)
        mixed_safe[1024:1056] = self.patched[1024:1056]
        st, m, _ = cp.classify_cp_image(bytes(mixed_safe), self.spec)
        self.assertEqual((st, m), ("mixed_safe", 2))
        # Dangerous mixed (W0 armed, W1 zeros -> mask 0x1)
        mixed_danger = bytearray(self.base)
        mixed_danger[128:132] = self.patched[128:132]
        st, m, _ = cp.classify_cp_image(bytes(mixed_danger), self.spec)
        self.assertEqual((st, m), ("mixed_danger_w0_only", 1))
        # Corrupted window byte (non-window bytes still stock -> corrupt_windows!)
        corrupt_w0 = bytearray(self.base)
        corrupt_w0[128] = 0xAA
        st, m, _ = cp.classify_cp_image(bytes(corrupt_w0), self.spec)
        self.assertEqual((st, m), ("corrupt_windows", 0xFF))
        # Non-window byte tampered -> unknown (even --repair-restore must refuse!)
        tampered = bytearray(self.base)
        tampered[10] ^= 0x55
        st, m, _ = cp.classify_cp_image(bytes(tampered), self.spec)
        self.assertEqual((st, m), ("unknown", 0xFF))

    def test_02_install_restore_roundtrip_and_backups(self):
        sess = FakeSession(self.base)
        cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        self.assertEqual(sess.files[cp.CP_PATH], self.patched)
        # Staging cleaned
        for p in cp.FLASH_FILES.values():
            self.assertNotIn(p, sess.files)
        # Backup persisted
        backups = list((self.state_dir / "cp-backups").glob("*/cpapp-b.bin"))
        self.assertEqual(len(backups), 1)
        self.assertEqual(backups[0].read_bytes(), self.base)
        # Restore roundtrip
        cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=False)
        self.assertEqual(sess.files[cp.CP_PATH], self.base)

    def test_03_mixed_convergence_install_and_restore(self):
        mixed = bytearray(self.base)
        mixed[1024:1056] = self.patched[1024:1056]
        sess1 = FakeSession(bytes(mixed))
        cp.cmd_mutate(sess1, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        self.assertEqual(sess1.files[cp.CP_PATH], self.patched)
        sess2 = FakeSession(bytes(mixed))
        cp.cmd_mutate(sess2, self.spec, self.writer, self.plan, self.state_dir, is_install=False)
        self.assertEqual(sess2.files[cp.CP_PATH], self.base)

    def test_04_writer_check_ro(self):
        sess = FakeSession(self.base)
        cp.cmd_writer_check(sess, self.writer, self.plan, self.state_dir)
        self.assertEqual(sess.files[cp.CP_PATH], self.base)
        for p in cp.FLASH_FILES.values():
            self.assertNotIn(p, sess.files)

    def test_05_refuses_unknown_and_readback_mismatch(self):
        tampered = bytearray(self.base)
        tampered[5] ^= 0x01
        with self.assertRaisesRegex(RuntimeError, "unrecognized"):
            cp.cmd_mutate(FakeSession(bytes(tampered)), self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        with self.assertRaisesRegex(RuntimeError, "readback mismatch"):
            cp.cmd_mutate(FakeSession(self.base, leave_corrupt=True), self.spec, self.writer, self.plan, self.state_dir, is_install=True)

    def test_06_refuses_foreign_staging_file_and_cleans_owned(self):
        sess = FakeSession(self.base)
        sess.files[cp.FLASH_FILES["mode"]] = b"foreign-junk"
        with self.assertRaisesRegex(RuntimeError, "already exist"):
            cp.cmd_writer_check(sess, self.writer, self.plan, self.state_dir)
        with self.assertRaisesRegex(RuntimeError, "unrecognized file"):
            cp.clean_owned_files(sess, self.writer, self.plan)
        sess.files[cp.FLASH_FILES["mode"]] = b"check\n"
        cp.clean_owned_files(sess, self.writer, self.plan)
        self.assertNotIn(cp.FLASH_FILES["mode"], sess.files)

    def test_07_real_production_artifact_and_plan(self):
        real_spec = cp.load_config(PATCH_DIR.parent / "patch.a6700.json")
        real_plan = cp.build_cp_plan(real_spec)
        self.assertTrue(cp.is_valid_cp_plan(real_plan))
        real_writer = cp.load_writer()
        self.assertIn(b"pilot_cp_hardened_v2\x00", real_writer)

    def test_08_immediate_disarm_on_writer_fail_and_clean_foreign(self):
        fail_res = b"A6700CPFLASH\nRESULT=FAIL\nmode=apply\nreason=wbi_not_clear\n"
        sess = FakeSession(self.base, force_result=fail_res)
        with self.assertRaisesRegex(RuntimeError, "failed on camera"):
            cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        # libJit.so and all staging files MUST be disarmed/removed immediately!
        for p in cp.FLASH_FILES.values():
            self.assertNotIn(p, sess.files)
        # Truncated .cpflash.result.tmp is safely cleaned without foreign override
        sess.files[cp.FLASH_FILES["result_tmp"]] = b""
        cp.clean_owned_files(sess, self.writer, self.plan)
        self.assertNotIn(cp.FLASH_FILES["result_tmp"], sess.files)
        # Foreign file can be cleared via --clean-foreign escape hatch
        sess.files[cp.FLASH_FILES["mode"]] = b"foreign-junk"
        cp.clean_owned_files(sess, self.writer, self.plan, allow_foreign=True)
        self.assertNotIn(cp.FLASH_FILES["mode"], sess.files)

    def test_09_cpload_session_context_manager_compatibility(self):
        # Verify real cpload_run.Session uses context manager (__enter__/__exit__) matching main()
        from on_camera import load_transport

        cr = load_transport(PATCH_DIR)
        self.assertTrue(hasattr(cr.Session, "__enter__") and hasattr(cr.Session, "__exit__"))
        self.assertFalse(hasattr(cr.Session, "open"))

    def test_10_c1_repair_restore_and_refuse_non_window_corruption(self):
        corrupt_w0 = bytearray(self.base)
        corrupt_w0[128] = 0xAA
        # Normal --restore must refuse corrupt_windows and point to --repair-restore
        with self.assertRaisesRegex(RuntimeError, "repair-restore"):
            cp.cmd_mutate(FakeSession(bytes(corrupt_w0)), self.spec, self.writer, self.plan, self.state_dir, is_install=False)
        # --repair-restore succeeds and restores exact stock baseline!
        sess = FakeSession(bytes(corrupt_w0))
        cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=False, repair_restore=True)
        self.assertEqual(sess.files[cp.CP_PATH], self.base)
        # Non-window byte tampered -> EVEN --repair-restore MUST refuse!
        tampered = bytearray(self.base)
        tampered[10] ^= 0x55
        with self.assertRaisesRegex(RuntimeError, "refusing even repair-restore"):
            cp.cmd_mutate(FakeSession(bytes(tampered)), self.spec, self.writer, self.plan, self.state_dir, is_install=False, repair_restore=True)

    def test_11_c3_timeout_disarms_trigger_files_and_s2_readback(self):
        orig_poll = cp.FLASH_POLL_SECONDS
        cp.FLASH_POLL_SECONDS = 0.05
        try:
            sess = FakeSession(self.base, simulate_timeout=True)
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
            # Trigger/staged/mode/plan MUST be disarmed even on timeout!
            for k in ("trigger", "staged", "mode", "plan"):
                self.assertNotIn(cp.FLASH_FILES[k], sess.files)
            # S2 post-failure readback must be saved into cp-device-state.json!
            import json
            st_doc = json.loads((self.state_dir / "cp-device-state.json").read_text(encoding="utf-8"))
            self.assertEqual(st_doc.get("post_failure_state"), "baseline")
        finally:
            cp.FLASH_POLL_SECONDS = orig_poll

    def test_12_s3_idempotent_short_circuit_skips_writer(self):
        # Already patched on --install -> 0 writer calls, succeeds even if writer would reject WBI2!
        fail_wbi = b"A6700CPFLASH\nRESULT=FAIL\nmode=apply\nreason=wbi_not_clear\n"
        sess = FakeSession(self.patched, force_result=fail_wbi)
        cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        self.assertEqual(sess.files[cp.CP_PATH], self.patched)
        # Already baseline on --restore -> 0 writer calls!
        sess2 = FakeSession(self.base, force_result=fail_wbi)
        cp.cmd_mutate(sess2, self.spec, self.writer, self.plan, self.state_dir, is_install=False)
        self.assertEqual(sess2.files[cp.CP_PATH], self.base)

    def test_13_n1_in_session_auto_repair_on_corrupt_failure(self):
        """N1: If apply fails leaving corrupted W0, host automatically executes force-restore
        in the SAME session and verifies full 17.89 MB restoration back to stock baseline."""
        import json

        class CorruptingFirstRunSession(FakeSession):
            def __init__(self, image: bytes, outer_self):
                super().__init__(image)
                self.outer = outer_self
                self.trigger_count = 0

            def jiritsu(self, cmd: int, payload: bytes, missing: bool = False):
                if cmd == 16 and self.selected == cp.FLASH_FILES["trigger"]:
                    self.files[self.selected] = payload[3:]
                    self.trigger_count += 1
                    mode = self.files.get(cp.FLASH_FILES["mode"], b"").decode("ascii").strip()
                    if self.trigger_count == 1:
                        # First call: simulate W0 corruption + rollback failure!
                        bad = bytearray(self.files[cp.CP_PATH])
                        bad[128] = 0xAA
                        self.files[cp.CP_PATH] = bytes(bad)
                        self.files[cp.FLASH_FILES["result"]] = (
                            b"A6700CPFLASH\nRESULT=FAIL\nmode=apply\nreason=pwrite_fail\nrollback=failed\n"
                        )
                    else:
                        # Second call (in-session auto-repair): MUST be force-restore!
                        assert mode == "force-restore", f"Expected force-restore, got {mode!r}"
                        self.files[cp.CP_PATH] = self.outer.base
                        self.files[cp.FLASH_FILES["result"]] = (
                            b"A6700CPFLASH\nRESULT=OK\nmode=force-restore\nstate=baseline\n"
                            b"repaired_unknown=1\nwbi_status=clear\n"
                        )
                    return b""
                return super().jiritsu(cmd, payload, missing=missing)

        sess = CorruptingFirstRunSession(self.base, self)
        with self.assertRaisesRegex(RuntimeError, "AUTO-REPAIRED IN-SESSION"):
            cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        self.assertEqual(sess.trigger_count, 2)
        self.assertEqual(sess.files[cp.CP_PATH], self.base)
        for k in ("trigger", "staged", "mode", "plan", "result"):
            self.assertNotIn(cp.FLASH_FILES[k], sess.files)
        st_doc = json.loads((self.state_dir / "cp-device-state.json").read_text(encoding="utf-8"))
        self.assertTrue(st_doc.get("auto_repair_attempted"))
        self.assertTrue(st_doc.get("auto_repair_ok"))
        self.assertTrue(st_doc.get("full_recovery_verified"))
        self.assertEqual(st_doc.get("auto_repair_after_state"), "baseline")

    def test_14_n2_repair_restore_wbi_wording_not_misleading(self):
        """N2: Post-repair active WBI2 notice must NOT claim write was rejected or repeat DO-NOT-POWER-OFF."""
        import io
        from contextlib import redirect_stderr

        corrupt = bytearray(self.base)
        corrupt[128] = 0xAA
        ok_wbi = (
            b"A6700CPFLASH\nRESULT=OK\nmode=force-restore\nstate=baseline\n"
            b"repaired_unknown=1\nwbi_status=wbi2\n"
        )
        sess = FakeSession(bytes(corrupt))
        orig_jiritsu = sess.jiritsu

        def patched_jiritsu(cmd: int, payload: bytes, missing: bool = False):
            res = orig_jiritsu(cmd, payload, missing=missing)
            if cmd == 16 and sess.selected == cp.FLASH_FILES["trigger"]:
                sess.files[cp.FLASH_FILES["result"]] = ok_wbi
            return res

        sess.jiritsu = patched_jiritsu
        err_buf = io.StringIO()
        with redirect_stderr(err_buf):
            cp.cmd_mutate(sess, self.spec, self.writer, self.plan, self.state_dir, is_install=False, repair_restore=True)
        stderr_text = err_buf.getvalue()
        self.assertIn("紧急修复已成功", stderr_text)
        self.assertNotIn("被拒绝", stderr_text)
        self.assertNotIn("切勿关机或断电", stderr_text)
        self.assertIn("不拨电源开关", stderr_text)

    def test_trigger_upload_failure_still_disarms_staging(self):
        class FailingTriggerSession(FakeSession):
            def jiritsu(self, cmd, data, missing=False):
                if cmd == 16 and self.selected == cp.FLASH_FILES["trigger"]:
                    self.files[self.selected] = data[3:]
                    raise OSError("trigger upload disconnected")
                return super().jiritsu(cmd, data, missing)

        session = FailingTriggerSession(self.base)
        with self.assertRaisesRegex(OSError, "trigger upload disconnected"):
            cp.cmd_mutate(session, self.spec, self.writer, self.plan, self.state_dir, is_install=True)
        self.assertEqual(session.files, {cp.CP_PATH: self.base})


if __name__ == "__main__":
    unittest.main(verbosity=2)
