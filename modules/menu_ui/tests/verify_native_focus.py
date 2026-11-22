"""Execute the firmware's cursor type filter with stock/Pilot detail tables.

Only tree insertion and lock helpers are intercepted. Descriptor name pointers
are represented by distinct tokens; the native filter compares their identity.
"""
import argparse
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
    UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP, UC_ARM64_REG_X19,
)


def verify(path):
    with Path(path).open("rb") as stream:
        elf = ELFFile(stream)
        segments = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]

        def read(address, size):
            for segment in segments:
                offset = address - segment["p_vaddr"]
                if 0 <= offset and offset + size <= segment["p_filesz"]:
                    return segment.data()[offset:offset + size]
            raise ValueError(hex(address))

        # GOT entries used by 0x21874E4, including the list-container type.
        slots = {
            0x4E78648, 0x4F1BC88, 0x4ED6D00, 0x4F1BC98,
            0x4E60758, 0x4E90BE8, 0x4F0D4D0, 0x4E65068,
            0x4E704A0, 0x4EFE4C8, 0x4EBB020, 0x4F03EC8,
            0x4E6E730, 0x4E75CA8, 0x4E90FE0, 0x4F03AB0,
        }
        relocations = {}
        for section in elf.iter_sections():
            if section["sh_type"] == "SHT_RELA":
                for relocation in section.iter_relocations():
                    if relocation["r_offset"] in slots:
                        relocations[relocation["r_offset"]] = relocation["r_addend"]
        assert slots == relocations.keys()
        stock = read(0x4AE9210, 13 * 0x28)
        code = read(0x2187000, 0x1000)
        display_guard = read(0x28FCE64, 0x1C)
        display_layout = read(0x4FD7130, 13 * 0x60)

    def run(table):
        emulator = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        pages = set()

        def write(address, data):
            for page in range(address & ~4095, (address + len(data) + 4095) & ~4095, 4096):
                if page not in pages:
                    emulator.mem_map(page, 4096)
                    pages.add(page)
            emulator.mem_write(address, bytes(data))

        write(0x2187000, code)
        for address, target in relocations.items():
            write(address, struct.pack("<Q", target))
            write(target, struct.pack("<Q", target))
        for index in range(13):
            target = struct.unpack_from("<Q", table, index * 0x28)[0]
            write(target, struct.pack("<Q", target))
        write(0x18000000, table)
        write(0x18010000, bytes(4096))
        write(0x18020000, bytes(8192))
        accepted = []

        def hook(uc, address, size, user):
            if address in (0x2186ED0, 0x2187254):
                accepted.append(uc.reg_read(UC_ARM64_REG_X2))
            if address in (0x2186ED0, 0x2187254, 0x2183410, 0x2183434):
                uc.reg_write(UC_ARM64_REG_PC, uc.reg_read(UC_ARM64_REG_LR))

        # Intercepted callees still need mapped instruction memory.
        write(0x2183000, bytes(4096))
        write(0x2186000, bytes(4096))
        emulator.hook_add(UC_HOOK_CODE, hook)
        emulator.reg_write(UC_ARM64_REG_X0, 0x18010000)
        emulator.reg_write(UC_ARM64_REG_X1, 0x18000000)
        emulator.reg_write(UC_ARM64_REG_X2, 13)
        emulator.reg_write(UC_ARM64_REG_SP, 0x18022000)
        emulator.reg_write(UC_ARM64_REG_LR, 0x18030000)
        emulator.emu_start(0x21874E4, 0x18030000, count=10000)
        assert emulator.reg_read(UC_ARM64_REG_PC) == 0x18030000
        return accepted

    pilot = bytearray(stock)
    pilot[7 * 0x28:7 * 0x28 + 8] = stock[:8]
    assert run(stock) == [7, 8], run(stock)
    assert run(pilot) == [8], run(pilot)
    print("PASS native cursor filter: stock=[Update, OK], Pilot=[OK]")

    def display_type_passes(type_descriptor):
        # Execute the exact type check preceding the captured 0x28FCF0C
        # assertion. Widget runtime type tags preserve descriptor identity.
        emulator = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        for address in (0x28FC000, 0x4F1B000, 0x8343000, 0x30000000, 0x30001000):
            emulator.mem_map(address, 4096)
        emulator.mem_write(0x28FCE64, display_guard)
        button_descriptor = struct.unpack_from("<Q", display_layout, 7 * 0x60)[0]
        emulator.mem_write(0x4F1BC88, struct.pack("<Q", button_descriptor))
        emulator.mem_write(button_descriptor, struct.pack("<Q", button_descriptor))
        emulator.mem_write(0x30000010, struct.pack("<Q", 0x30001000))
        emulator.mem_write(0x300010A8, struct.pack("<Q", type_descriptor))

        def stop_at_branch(uc, address, size, user):
            if address in (0x28FCE80, 0x28FCF0C):
                uc.emu_stop()

        emulator.hook_add(UC_HOOK_CODE, stop_at_branch)
        emulator.reg_write(UC_ARM64_REG_X19, 0x30000000)
        emulator.emu_start(0x28FCE64, 0x28FD000, count=32)
        return emulator.reg_read(UC_ARM64_REG_PC) == 0x28FCF0C

    stock_button = struct.unpack_from("<Q", display_layout, 7 * 0x60)[0]
    background = struct.unpack_from("<Q", display_layout)[0]
    assert not display_type_passes(background), "failed v1 must reach assertion path"
    assert display_type_passes(stock_button), "preserved display type must pass"
    print("PASS native Version display check: v1 demotion asserts, preserved type passes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("appfw")
    verify(parser.parse_args().appfw)
