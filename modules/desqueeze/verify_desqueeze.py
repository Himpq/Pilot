"""Offline verification for Pilot/modules/desqueeze/pilot_desqueeze.c (v2).

1. Parses fixed patch tables and kRatios[] directly from pilot_desqueeze.c.
2. Verifies every stock byte sequence and opcode skeleton against A6700 FW 2.00
   cpapp-b.bin, and verifies appFw.so VDF IPC function prologues.
3. Emulates ALL 7 active ratios x 2 source bases (16:9 and 3:2 OpenGate) in
   Unicorn on LCD (720x480), EVF (1024x768), HDMI 1080p, and HDMI 4K, plus
   Still Photo protection (3:2 / 4:3 / 1:1) for BOTH vdf_mode=1 AND vdf_mode=2,
   HDMI manual path (0x1F6564), and AF/Tracking frame pass checks.
"""
import argparse
import hashlib
import os
import re
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED
from unicorn import arm_const as A

HERE = Path(__file__).resolve().parent
SRC = (HERE / "pilot_desqueeze.c").read_text(encoding="utf-8")
CP_BASE = 0x108000
CP_SHA = "61b7fe5c0ce227ea865d799b81fe77ae359b7c5dcd39d13c9c2241d9cad23559"
APPFW_SHA = "e65fd595052be5d0e98ce8000dfcb94f90a2f8309de70ef140375052009b0ef3"


def resolve_input_paths():
    parser = argparse.ArgumentParser(description="Offline verification for pilot_desqueeze.c")
    parser.add_argument("--cp", type=Path, default=None, help="Path to A6700 FW 2.00 cpapp-b.bin (or set PILOT_A6700_CP)")
    parser.add_argument("--appfw", type=Path, default=None, help="Path to A6700 FW 2.00 lib_appFw.so (or set PILOT_A6700_APPFW)")
    args = parser.parse_args()

    cp_env = os.environ.get("PILOT_A6700_CP")
    appfw_env = os.environ.get("PILOT_A6700_APPFW")
    cp_path = args.cp or (Path(cp_env) if cp_env else None)
    appfw_path = args.appfw or (Path(appfw_env) if appfw_env else None)

    if cp_path is None or appfw_path is None:
        parser.error("provide --cp and --appfw (or set PILOT_A6700_CP and PILOT_A6700_APPFW)")
    return cp_path, appfw_path


def parse_defines(src):
    defs = {}
    for m in re.finditer(r"#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)[uUlL]*", src):
        defs[m.group(1)] = int(m.group(2), 0)
    return defs


def parse_byte_list(s):
    out = []
    for tok in s.split(","):
        tok = tok.strip()
        if tok:
            out.append(int(tok.rstrip("uUlL"), 0))
    return bytes(out)


def parse_fixed_patches(src):
    pat = re.compile(
        r'"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)u\s*,\s*(\d+)u\s*,\s*\{([^}]+)\}\s*,\s*\{([^}]+)\}',
        re.S,
    )
    patches = []
    for m in pat.finditer(src):
        name, va_s, ln_s, st_s, pt_s = m.groups()
        va, ln = int(va_s, 16), int(ln_s, 10)
        st, pt = parse_byte_list(st_s), parse_byte_list(pt_s)
        assert len(st) == ln and len(pt) == ln, (name, ln, len(st), len(pt))
        patches.append((name, va, ln, st, pt))
    assert len(patches) == 4, f"expected 4 fixed patches, got {len(patches)}"
    return patches


def parse_ratios(src):
    m = re.search(r"kRatios\[DSQ_COUNT\]\s*=\s*\{(.*?)\};", src, re.S)
    assert m
    rows = []
    for rm in re.finditer(r"/\*\s*(\w+)\s*\*/\s*\{\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u\s*\}", m.group(1)):
        name = rm.group(1)
        vals = [int(rm.group(i)) for i in range(2, 7)]
        rows.append((name, *vals))
    assert len(rows) == 8, f"expected 8 ratios, got {len(rows)}"
    return rows


class CpEmu:
    def __init__(self, cp_bytes, raw_writes=()):
        img = bytearray(cp_bytes)
        for va, pt in raw_writes:
            img[va - CP_BASE : va - CP_BASE + len(pt)] = pt
        self.uc = uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        try:
            uc.ctl_set_cpu_model(A.UC_CPU_ARM_CORTEX_A15)
        except Exception:
            pass
        uc.mem_map(0x100000, 0x2500000)
        uc.mem_write(CP_BASE, bytes(img))
        uc.reg_write(A.UC_ARM_REG_C1_C0_2, uc.reg_read(A.UC_ARM_REG_C1_C0_2) | (0xF << 20))
        uc.reg_write(A.UC_ARM_REG_FPEXC, 0x40000000)
        self.log, self.stubs = [], {}
        uc.hook_add(
            UC_HOOK_MEM_UNMAPPED,
            lambda u, acc, addr, sz, v, _: self.log.append(f"unmapped {addr:#x} pc={u.reg_read(A.UC_ARM_REG_PC):#x}") or False,
        )
        self.ctx = 0x2400000
        self.dev = 0x01214658
        uc.mem_write(self.ctx, struct.pack("<IIII", 0x00D06C04, 0x012059F8, self.dev, 0))
        uc.mem_write(self.dev + 0xB4, struct.pack("<I", 2))
        uc.mem_write(self.dev + 0xD8, struct.pack("<I", 3))
        uc.mem_write(self.dev + 0x6E80, struct.pack("<II", 0, 0))
        uc.mem_write(self.dev + 0x6EBC, struct.pack("<I", 0))

    def set_dsq(self, mode, en):
        self.uc.mem_write(self.dev + 0x6F79, bytes([mode, en, 0, 0]))

    def stub(self, addr, fn):
        if addr not in self.stubs:
            self.uc.hook_add(UC_HOOK_CODE, lambda u, ad, sz, _: self._run_stub(ad), begin=addr, end=addr)
        self.stubs[addr] = fn

    def _run_stub(self, addr):
        r0 = self.stubs[addr](self.uc)
        if r0 is not None:
            self.uc.reg_write(A.UC_ARM_REG_R0, r0 & 0xFFFFFFFF)
        self.uc.reg_write(A.UC_ARM_REG_PC, self.uc.reg_read(A.UC_ARM_REG_LR))

    def call(self, fn, regs, stack=()):
        RET, STK = 0x2410000, 0x2300000
        sp = STK - 0x800
        for i, v in enumerate(stack):
            self.uc.mem_write(sp + 4 * i, struct.pack("<I", v & 0xFFFFFFFF))
        for r, v in zip((A.UC_ARM_REG_R0, A.UC_ARM_REG_R1, A.UC_ARM_REG_R2, A.UC_ARM_REG_R3), regs):
            self.uc.reg_write(r, v & 0xFFFFFFFF)
        self.uc.reg_write(A.UC_ARM_REG_SP, sp)
        self.uc.reg_write(A.UC_ARM_REG_LR, RET | 1)
        self.log.clear()
        self.uc.emu_start(fn | 1, RET, count=200000)
        assert not self.log, self.log
        assert self.uc.reg_read(A.UC_ARM_REG_PC) == RET
        return self.uc.reg_read(A.UC_ARM_REG_R0)

    def dsq_calc(self, w, h, pc, aspect, fit=3, flags=0, oid=1):
        out = 0x2408000
        self.uc.mem_write(out, bytes(8))
        self.call(0x6BA392, (0, w | (h << 16), pc, aspect), (fit, flags, out, self.ctx, oid))
        return list(struct.unpack("<4H", bytes(self.uc.mem_read(out, 8))))

    def dsq_hdmi_manual(self, w, h, aspect=2, dev_mode=3):
        self.uc.mem_write(self.dev + 0xB4, struct.pack("<I", aspect))
        self.uc.mem_write(self.dev + 0xD8, struct.pack("<I", dev_mode))
        rect = 0x2408100
        self.uc.mem_write(rect, struct.pack("<4H", 0, 0, w, h))
        self.call(0x1F6564, (0, 0, w | (h << 16), rect), (self.ctx,))
        return list(struct.unpack("<4H", bytes(self.uc.mem_read(rect, 8))))

    def dsq_mode2_crop(self, src_w, src_h, aspect=2, crop_x=0):
        """Emulate Sony 0x6BBF1C (mode 2 source horizontal crop)."""
        rect = 0x2408200
        meta = 0x2408300
        self.uc.mem_write(rect, bytes(0x40))
        self.uc.mem_write(meta, bytes(0x40))
        self.uc.mem_write(rect + 0x08, struct.pack("<HH", src_w, src_h))
        self.uc.mem_write(rect + 0x0C, struct.pack("<H", crop_x))
        self.uc.mem_write(rect + 0x10, struct.pack("<HH", src_w, src_h))
        self.uc.mem_write(meta + 0x1E, bytes([aspect]))
        self.stub(0x6CED2E, lambda uc: 1)
        self.call(0x6BBF1C, (rect, meta, self.ctx, 1))
        cx = struct.unpack("<H", bytes(self.uc.mem_read(rect + 0x0C, 2)))[0]
        cw, ch = struct.unpack("<HH", bytes(self.uc.mem_read(rect + 0x10, 4)))
        return (cx, cw, ch)

    def check_af(self, live, ow, oh, kind=0, row_sel=1):
        m = 0x2440000
        self.uc.mem_write(m, bytes(0x3000))
        self.uc.mem_write(m + 0x1C10, struct.pack("<I", 4))
        x, y, w, h = live

        def lv(uc):
            sp = uc.reg_read(A.UC_ARM_REG_SP)
            p_h = struct.unpack("<I", bytes(uc.mem_read(sp, 4)))[0]
            uc.mem_write(uc.reg_read(A.UC_ARM_REG_R1), struct.pack("<4i", y, y + h, x, x + w))
            uc.mem_write(uc.reg_read(A.UC_ARM_REG_R3), struct.pack("<H", ow))
            uc.mem_write(p_h, struct.pack("<H", oh))
            return 1

        self.stub(0x63E13C, lambda uc: kind)
        self.stub(0x634B36, lv)
        self.stub(0x634DBC, lambda uc: row_sel)
        return self.call(0x128438, (m,))

    def check_trk(self, live, ow, oh, kind=0, row_sel=1):
        m = 0x2440000
        self.uc.mem_write(m, bytes(0x3000))
        x, y, w, h = live

        def lv(uc):
            y0, x0 = y * 480 // oh, x * 640 // ow
            uc.mem_write(uc.reg_read(A.UC_ARM_REG_R1), struct.pack("<4i", y0, y0 + h * 480 // oh, x0, x0 + w * 640 // ow))
            return 1

        def sz(uc):
            uc.mem_write(uc.reg_read(A.UC_ARM_REG_R2), struct.pack("<H", ow))
            uc.mem_write(uc.reg_read(A.UC_ARM_REG_R3), struct.pack("<H", oh))
            return None

        self.stub(0x63E13C, lambda uc: kind)
        self.stub(0x63CB2A, lv)
        self.stub(0x63CB94, sz)
        self.stub(0x63CF44, lambda uc: row_sel)
        self.stub(0x63CF52, lambda uc: 4)
        return self.call(0x129288, (m,))


def main():
    cp_path, appfw_path = resolve_input_paths()
    defs = parse_defines(SRC)
    fixed = parse_fixed_patches(SRC)
    ratios = parse_ratios(SRC)

    cp = cp_path.read_bytes()
    assert hashlib.sha256(cp).hexdigest() == CP_SHA

    # 1. Verify fixed patches against stock cpapp-b.bin
    for name, va, ln, st_b, pt_b in fixed:
        got = cp[va - CP_BASE : va - CP_BASE + ln]
        assert got == st_b, f"{name} @ {va:#x}: {got.hex()} != {st_b.hex()}"

    # 2. Verify all 5 dynamic ratio opcode skeletons against stock cpapp-b.bin
    assert cp[defs["CP_DSQ_M1_MAIN_VA"]  - CP_BASE : defs["CP_DSQ_M1_MAIN_VA"]  - CP_BASE + 6] == bytes([0x4f, 0xf0, 45, 0x09, 104, 0x26])
    assert cp[defs["CP_DSQ_M1_HDMI1_VA"] - CP_BASE : defs["CP_DSQ_M1_HDMI1_VA"] - CP_BASE + 4] == bytes([45, 0x24, 104, 0x25])
    assert cp[defs["CP_DSQ_M1_HDMI2_VA"] - CP_BASE : defs["CP_DSQ_M1_HDMI2_VA"] - CP_BASE + 4] == bytes([45, 0x24, 104, 0x25])
    assert cp[defs["CP_DSQ_M2_MAIN_VA"]  - CP_BASE : defs["CP_DSQ_M2_MAIN_VA"]  - CP_BASE + 6] == bytes([0x4f, 0xf0, 3, 0x09, 8, 0x26])
    assert cp[defs["CP_DSQ_M2_HDMI_VA"]  - CP_BASE : defs["CP_DSQ_M2_HDMI_VA"]  - CP_BASE + 4] == bytes([3, 0x24, 8, 0x25])

    # 3. Verify appFw.so prologues
    app = appfw_path.read_bytes()
    assert hashlib.sha256(app).hexdigest() == APPFW_SHA
    for sym, exp_b in (
        ("APPFW_REPLY_MBX_CTOR_OFF", "fd7bb6a9"),
        ("APPFW_REPLY_MBX_DTOR_OFF", "020c4029"),
        ("APPFW_VDF_SEND_OFF",       "fd7ba2a9"),
        ("APPFW_VDF_RECV_OFF",       "fd7ba2a9"),
    ):
        off = defs[sym]
        assert app[off : off + 4] == bytes.fromhex(exp_b), sym

    # 4. Emulate every single active ratio x both source bases (14 cases!)
    def expected_rect(w, h, num, den):
        oh = ((w * num + 2 * den) // den) & 0xFFFC
        oy = (h - oh) // 2
        return [0, oy, w, oh]

    print("=== Emulating all 7 active modes x 2 source bases (14 cases) ===")
    for name, vdf_mode, n169, d169, nog, dog in ratios[1:]:
        for src_label, num, den in (("16:9", n169, d169), ("3:2_OG", nog, dog)):
            m1_n, m1_d = (num, den) if vdf_mode == 1 else (45, 104)
            m2_n, m2_d = (num, den) if vdf_mode == 2 else (3, 8)
            writes = [(va, pt_b) for _, va, _, _, pt_b in fixed] + [
                (defs["CP_DSQ_M1_MAIN_VA"],  bytes([0x4f, 0xf0, m1_n, 0x09, m1_d, 0x26])),
                (defs["CP_DSQ_M1_HDMI1_VA"], bytes([m1_n, 0x24, m1_d, 0x25])),
                (defs["CP_DSQ_M1_HDMI2_VA"], bytes([m1_n, 0x24, m1_d, 0x25])),
                (defs["CP_DSQ_M2_MAIN_VA"],  bytes([0x4f, 0xf0, m2_n, 0x09, m2_d, 0x26])),
                (defs["CP_DSQ_M2_HDMI_VA"],  bytes([m2_n, 0x24, m2_d, 0x25])),
            ]
            emu = CpEmu(cp, writes)
            emu.set_dsq(vdf_mode, 1)

            lcd  = emu.dsq_calc(720, 480, 1, aspect=2, fit=3, oid=1)
            evf  = emu.dsq_calc(1024, 768, 0, aspect=2, fit=3, oid=2)
            hd   = emu.dsq_calc(1920, 1080, 2, aspect=2, fit=0, oid=4)
            uhd  = emu.dsq_calc(3840, 2160, 2, aspect=2, fit=0, oid=4)
            hdm  = emu.dsq_hdmi_manual(1920, 1080, aspect=2, dev_mode=3)

            assert lcd == expected_rect(720, 480, num, den), (name, src_label, lcd)
            assert evf == expected_rect(1024, 768, num, den), (name, src_label, evf)
            assert hd  == expected_rect(1920, 1080, num, den), (name, src_label, hd)
            assert uhd == expected_rect(3840, 2160, num, den), (name, src_label, uhd)
            assert hdm == expected_rect(1920, 1080, num, den), (name, src_label, hdm)

            # DCI 17:9 (aspect == 5) also uses our ratio; mode == 0 falls back cleanly to stock 16:9
            assert emu.dsq_calc(720, 480, 1, aspect=5, fit=3, oid=1) == expected_rect(720, 480, num, den), (name, src_label, "17:9")

            # Photo mode protection MUST pass for BOTH mode 1 AND mode 2!
            assert emu.dsq_calc(1024, 768, 0, aspect=1, fit=3, oid=2) == [0, 42, 1024, 684], (name, src_label, "evf 3:2")
            assert emu.dsq_calc(720, 480, 1, aspect=1, fit=0, oid=1)  == [0, 0, 720, 480],    (name, src_label, "lcd 3:2")
            assert emu.dsq_calc(720, 480, 1, aspect=0, fit=2, oid=1)  == [40, 0, 640, 480],   (name, src_label, "lcd 4:3")
            assert emu.dsq_calc(720, 480, 1, aspect=3, fit=2, oid=1)  == [120, 0, 480, 480],  (name, src_label, "lcd 1:1")

            # Verify mode == 0 fallback with guard installed
            emu.set_dsq(0, 0)
            assert emu.dsq_calc(720, 480, 1, aspect=2, fit=3, oid=1) == [0, 38, 720, 404], (name, src_label, "mode0 fallback")
            emu.set_dsq(vdf_mode, 1)

            # Verify mode 2 hardware horizontal crop (0x6BBF1C) on 16:9, 3:2 OG, and Photo
            if vdf_mode == 2:
                assert emu.dsq_mode2_crop(1920, 1080, aspect=2) == (240, 1440, 1080)  # 4:3 crop -> 4:3 * 2x = 2.67:1 (3/8)
                assert emu.dsq_mode2_crop(1920, 1280, aspect=2) == (106, 1708, 1280)  # 4:3 crop -> 4:3 * 2x = 2.67:1 (3/8)
                assert emu.dsq_mode2_crop(1920, 1280, aspect=1) == (0,   1920, 1280)  # Photo untouched!

            # AF & Tracking checks MUST pass
            assert emu.check_af(lcd, 720, 480, kind=0, row_sel=1) == 1
            assert emu.check_trk(lcd, 720, 480, kind=0, row_sel=1) == 1
            assert emu.check_af(evf, 1024, 768, kind=2, row_sel=0) == 1
            assert emu.check_trk(evf, 1024, 768, kind=2, row_sel=0) == 1

            print(f"  {name:14s} [{src_label:6s}] vdf={vdf_mode} {num:2d}/{den:3d}  LCD={lcd}  EVF={evf}  HDMI={hd}")

    print("ALL 14 RATIO MODES + MODE-1/MODE-2 PHOTO PROTECTION + AF/TRK CHECKS PASSED!")


if __name__ == "__main__":
    main()
