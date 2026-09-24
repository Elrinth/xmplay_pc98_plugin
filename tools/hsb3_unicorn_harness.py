#!/usr/bin/env python3
"""HSB3 ArtDink music driver ground truth via Unicorn.

Music module at image+0x6360 has a +0x6360 linker bias on absolute near
addresses. Relative calls are fine. We subtract the bias from immediates
and jump-table words, then drive INIT/LOAD/PLAY and the timer IRQ.
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path
from capstone import Cs, CS_ARCH_X86, CS_MODE_16

from unicorn import (
    Uc, UcError, UC_ARCH_X86, UC_MODE_16,
    UC_HOOK_INTR, UC_HOOK_INSN, UC_HOOK_CODE, UC_HOOK_MEM_UNMAPPED,
)
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_IP, UC_X86_REG_FLAGS,
    UC_X86_INS_IN, UC_X86_INS_OUT,
)

MEM_SIZE = 1 << 20
LOAD_SEG = 0x2000
PSP_SEG = LOAD_SEG - 0x10
SONG_SEG = 0x7000
STUB_SEG = 0x9000
BIAS = 0x6360


def p16(v: int) -> bytes:
    return struct.pack("<H", v & 0xFFFF)


def phys(seg: int, off: int) -> int:
    return ((seg & 0xFFFF) << 4) + (off & 0xFFFF)


class ChipLog:
    def __init__(self):
        self.addr = [0, 0]
        self.regs = [bytearray(256), bytearray(256)]
        self.writes: list[tuple[int, str, int, int, int]] = []
        self.midi: list[tuple] = []
        self.tick = 0
        self.ports: set[int] = set()

    def in_port(self, port: int, size: int) -> int:
        port &= 0xFFFF
        self.ports.add(port)
        # MPU-PC98 / MIDI: status ready (bit0=0 means Tx ready on some; FE=OK)
        if port == 0xE0D0:
            return 0xFE
        if port == 0xE0D2:
            return 0x00
        # PC-9801-86: A460h machine ID / sound — bit0=0 means 86 present (OPNA)
        # Common values: 0xFC (86), 0xFF (no 86). Return 0xFC so HSB3 keeps OPNA.
        if port == 0xA460:
            return 0xFC
        if port in (0x188, 0x18C, 0x088, 0x08C):
            # OPNA status: bit7 busy=0
            return 0x00
        if port == 0x18A:
            return self.regs[0][self.addr[0]]
        if port == 0x18E:
            return self.regs[1][self.addr[1]]
        return 0x00 if size == 1 else 0x0000

    def out_port(self, port: int, size: int, value: int):
        port &= 0xFFFF
        v8 = value & 0xFF
        self.ports.add(port)
        if port in (0x188, 0x088):
            self.addr[0] = v8
        elif port in (0x18A, 0x08A):
            self.regs[0][self.addr[0]] = v8
            self.writes.append((self.tick, "OPNA", 0, self.addr[0], v8))
        elif port in (0x18C, 0x08C):
            self.addr[1] = v8
        elif port in (0x18E, 0x08E):
            self.regs[1][self.addr[1]] = v8
            self.writes.append((self.tick, "OPNA", 1, self.addr[1], v8))
        elif port in (0xE0D0, 0xE0D2, 0xC8D2, 0xC8D3, 0xA4D2, 0xA4D3,
                      0x00F2, 0x7E0, 0x7E2, 0x7E8):
            self.midi.append((self.tick, port, v8))


class Harness:
    def __init__(self, exe_path: Path):
        self.raw = exe_path.read_bytes()
        self.mu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.mu.mem_map(0, MEM_SIZE)
        self.chip = ChipLog()
        self.music_cs = 0
        self.api: dict[str, tuple[int, int]] = {}
        self.timer_handler: tuple[int, int] | None = None
        self.ivt: dict[int, tuple[int, int]] = {}
        self._alloc = 0x6000
        self.img_size = 0
        self._unmap_count = 0
        self._load_mz()
        self._hooks()

    def r(self, reg):
        return self.mu.reg_read(reg)

    def w(self, reg, val):
        self.mu.reg_write(reg, val & 0xFFFF)

    def mem_r(self, seg, off, n) -> bytes:
        return bytes(self.mu.mem_read(phys(seg, off), n))

    def mem_w(self, seg, off, data: bytes):
        self.mu.mem_write(phys(seg, off), data)

    def _load_mz(self):
        raw = self.raw
        assert raw[:2] == b"MZ"
        (last, pages, nreloc, header_paras, _mina, _maxa,
         ss, sp, _ck, ip, cs, reloc_off, _ov) = struct.unpack_from(
            "<HHHHHHHHHHHHH", raw, 2)
        hdr = header_paras * 16
        img_size = pages * 512 - (512 - last if last else 0) - hdr
        if img_size < 0 or hdr + img_size > len(raw):
            img_size = len(raw) - hdr
        img = bytearray(raw[hdr:hdr + img_size])
        for i in range(nreloc):
            off, seg = struct.unpack_from("<HH", raw, reloc_off + i * 4)
            pos = seg * 16 + off
            if 0 <= pos < len(img) - 1:
                val = struct.unpack_from("<H", img, pos)[0]
                struct.pack_into("<H", img, pos, (val + LOAD_SEG) & 0xFFFF)
        self.mu.mem_write(LOAD_SEG << 4, bytes(img))
        self.img_size = len(img)
        psp = bytearray(0x100)
        psp[0:2] = b"\xCD\x20"
        struct.pack_into("<H", psp, 2, 0x9FFF)
        self.mu.mem_write(PSP_SEG << 4, bytes(psp))
        print(f"MZ @{LOAD_SEG:04X} img={self.img_size:#x} entry="
              f"{(cs+LOAD_SEG)&0xFFFF:04X}:{ip:04X} relocs={nreloc}")

    def _hooks(self):
        self.mu.hook_add(UC_HOOK_INTR, self._on_intr)
        self.mu.hook_add(UC_HOOK_INSN, self._on_out, None, 1, 0, UC_X86_INS_OUT)
        self.mu.hook_add(UC_HOOK_INSN, self._on_in, None, 1, 0, UC_X86_INS_IN)
        self.mu.hook_add(UC_HOOK_MEM_UNMAPPED, self._on_unmap)
        # Skip OPNA busy-wait delay loop (mov cx,[delay]; loop $)
        # Installed after music_cs known (see install_delay_skip).

    def _on_unmap(self, mu, access, address, size, value, data):
        self._unmap_count += 1
        if self._unmap_count <= 6:
            print(f"UNMAP access={access} addr={address:#x} "
                  f"CS:IP={self.r(UC_X86_REG_CS):04X}:{self.r(UC_X86_REG_IP):04X}")
        return False

    def _on_in(self, mu, port, size, data):
        return self.chip.in_port(port, size)

    def _on_out(self, mu, port, size, value, data):
        self.chip.out_port(port, size, value)

    def _on_intr(self, mu, intno, data):
        if intno == 0x21:
            self._int21()
        elif intno == 0x18:
            self.w(UC_X86_REG_AX, 0)
        elif intno == 0x1C:
            # PC-98 timer tick: bump a byte so calibration compares observe change
            self._tick1c = getattr(self, "_tick1c", 0) + 1
            # If caller uses [bx+5] as sense byte, bump it
            bx = self.r(UC_X86_REG_BX)
            ds = self.r(UC_X86_REG_DS)
            try:
                b = self.mem_r(ds, (bx + 5) & 0xFFFF, 1)[0]
                self.mem_w(ds, (bx + 5) & 0xFFFF, bytes([(b + 1) & 0xFF]))
            except Exception:
                pass

    def _clc(self):
        self.w(UC_X86_REG_FLAGS, self.r(UC_X86_REG_FLAGS) & ~1)

    def _int21(self):
        ax = self.r(UC_X86_REG_AX)
        ah, al = (ax >> 8) & 0xFF, ax & 0xFF
        if ah == 0x30:
            self.w(UC_X86_REG_AX, 0x0005)
            return
        if ah == 0x25:
            self.ivt[al] = (self.r(UC_X86_REG_DS), self.r(UC_X86_REG_DX))
            # also mirror into real IVT so direct reads see it
            self.mem_w(0, al * 4, p16(self.ivt[al][1]) + p16(self.ivt[al][0]))
            if al in (0x08, 0x1C, 0x0A, 0x07):
                self.timer_handler = self.ivt[al]
                print(f"  setvector INT{al:02X} -> "
                      f"{self.ivt[al][0]:04X}:{self.ivt[al][1]:04X}")
            return
        if ah == 0x35:
            # Prefer live IVT (driver may have installed via ES:[20])
            off, seg = struct.unpack("<HH", self.mem_r(0, al * 4, 4))
            if seg or off:
                self.w(UC_X86_REG_ES, seg)
                self.w(UC_X86_REG_BX, off)
            else:
                seg, off = self.ivt.get(al, (0, 0))
                self.w(UC_X86_REG_ES, seg)
                self.w(UC_X86_REG_BX, off)
            return
        if ah == 0x48:
            n = self.r(UC_X86_REG_BX)
            seg = self._alloc
            self._alloc = (self._alloc + max(n, 1) + 1) & 0xFFFF
            if self._alloc < seg:
                self._alloc = 0xA000
            self.w(UC_X86_REG_AX, seg)
            self._clc()
            return
        if ah in (0x49, 0x4A, 0x3E, 0x4B):
            self._clc()
            return
        if ah == 0x3D:
            self.w(UC_X86_REG_AX, 5)
            self._clc()
            return
        if ah == 0x3F:
            self.w(UC_X86_REG_AX, 0)
            self._clc()
            return
        if ah == 0x42:
            self.w(UC_X86_REG_AX, 0)
            self.w(UC_X86_REG_DX, 0)
            self._clc()
            return
        if ah == 0x4C:
            self.mu.emu_stop()
            return
        self._clc()

    def find_api(self) -> bool:
        img = self.mem_r(LOAD_SEG, 0, self.img_size)
        found = []
        i = 0
        while True:
            j = img.find(b"\x32\xE4\xE8", i)
            if j < 0:
                break
            if (j + 12 <= len(img)
                    and img[j + 5] == 0xCB
                    and img[j + 6:j + 8] == b"\xB4\x08"
                    and img[j + 8] == 0xE8
                    and img[j + 11] == 0xCB):
                rel = struct.unpack_from("<h", img, j + 3)[0]
                tgt = j + 5 + rel
                if 0 <= tgt + 0x14 < len(img):
                    if struct.unpack_from("<H", img, tgt + 0x12)[0] == 0x97FF:
                        found.append(j)
            i = j + 1
        if not found:
            print("API signature not found")
            return False
        sig = found[0]
        self.music_cs = (LOAD_SEG + (sig >> 4)) & 0xFFFF
        init_off = sig & 0xF
        print(f"API sig image+{sig:#x} -> CS={self.music_cs:04X} INIT={init_off:04X}")
        self.api["INIT"] = (self.music_cs, init_off)
        self.api["TRAMP"] = (self.music_cs, init_off + 2)
        self.api["LOAD"] = (self.music_cs, init_off + 0x0C)
        base = (self.music_cs - LOAD_SEG) << 4
        scan = base + init_off + 0x5D
        for k in range(16):
            if scan + k < len(img) and img[scan + k] == 0xCB:
                self.api["STOP"] = (self.music_cs, init_off + 0x5D + k + 1)
                break
        print("API:", {k: f"{s:04X}:{o:04X}" for k, (s, o) in self.api.items()})
        return True

    def debias_music_module(self):
        """Subtract +0x6360 linker bias via Capstone-guided patches."""
        cs = self.music_cs
        blob = bytearray(self.mem_r(cs, 0, 0x10000))
        B = BIAS

        def maybe(off: int, allow_ff: bool = True) -> bool:
            if off + 2 > len(blob):
                return False
            val = struct.unpack_from("<H", blob, off)[0]
            # Skip bitmask sentinel 0x80FF (allocate probe), not table ptrs like 0x6FFF
            if not allow_ff and val == 0x80FF:
                return False
            # FM/SSG voice RAM (DS:8064 / DS:8464) — real data addrs, not biased code
            if val in (0x8064, 0x8464):
                return False
            if B <= val <= B + 0x2800:
                struct.pack_into("<H", blob, off, (val - B) & 0xFFFF)
                return True
            return False

        # Jump tables of near ptrs (data already at unbiased addrs)
        for tb, nwords in (
            (0x08A5, 32), (0x142A, 128), (0x1468, 48),
            (0x13EA, 80), (0x02BB, 16),
        ):
            for i in range(nwords):
                maybe(tb + i * 2)

        md = Cs(CS_ARCH_X86, CS_MODE_16)
        patches = 0
        starts = [0x0000, 0x00F4, 0x017F, 0x01A0, 0x0242, 0x051C, 0x053B,
                  0x061E, 0x066F, 0x08C9, 0x0BBB, 0x0BFF, 0x11BB, 0x11F4,
                  0x1207, 0x2200]
        seen = set()

        def patch_modrm_disp(addr: int, b: bytes, pref: int) -> bool:
            """Patch abs16 or disp16 in modrm forms if biased."""
            nonlocal patches
            if len(b) < pref + 4:
                return False
            modrm = b[pref + 1]
            mod = (modrm >> 6) & 3
            rm = modrm & 7
            # abs: mod=0 rm=6 → disp16 at pref+2
            if mod == 0 and rm == 6:
                if maybe(addr + pref + 2):
                    patches += 1
                    return True
            # [reg+disp16]: mod=2 → disp16 at pref+2
            if mod == 2:
                if maybe(addr + pref + 2):
                    patches += 1
                    return True
            return False

        for start in starts:
            offset = start
            while offset < 0x2800 and offset not in seen:
                seen.add(offset)
                insns = list(md.disasm(bytes(blob[offset:offset + 16]), offset, count=1))
                if not insns:
                    offset += 1
                    continue
                insn = insns[0]
                b = bytes(insn.bytes)
                pref = 0
                op0 = b[0]
                if op0 in (0x26, 0x2E, 0x36, 0x3E, 0x64, 0x65) and len(b) > 1:
                    pref = 1
                    op0 = b[1]

                # mov r16, imm16 / push imm16
                if 0xB8 <= op0 <= 0xBF and len(b) >= pref + 3:
                    if maybe(insn.address + pref + 1, allow_ff=False):
                        patches += 1
                elif op0 == 0x68 and len(b) >= pref + 3:
                    if maybe(insn.address + pref + 1, allow_ff=False):
                        patches += 1
                # A0-A3 abs
                elif op0 in (0xA0, 0xA1, 0xA2, 0xA3) and len(b) >= pref + 3:
                    if maybe(insn.address + pref + 1):
                        patches += 1
                # C6/C7 06 abs imm
                elif op0 in (0xC6, 0xC7) and len(b) > pref + 1 and b[pref + 1] == 0x06:
                    if maybe(insn.address + pref + 2):
                        patches += 1
                # FF /r with abs or disp16 or [bx+disp]
                elif op0 == 0xFF and len(b) > pref + 1:
                    patch_modrm_disp(insn.address, b, pref)
                # ALU / MOV / TEST / OR / AND with modrm
                elif op0 in (
                    0x00, 0x01, 0x02, 0x03, 0x08, 0x09, 0x0A, 0x0B,
                    0x10, 0x11, 0x12, 0x13, 0x18, 0x19, 0x1A, 0x1B,
                    0x20, 0x21, 0x22, 0x23, 0x28, 0x29, 0x2A, 0x2B,
                    0x30, 0x31, 0x32, 0x33, 0x38, 0x39, 0x3A, 0x3B,
                    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
                    0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8E,
                    0xF6, 0xF7,
                ) and len(b) > pref + 1:
                    patch_modrm_disp(insn.address, b, pref)

                offset += insn.size

        self.mem_w(cs, 0, bytes(blob))
        ah = struct.unpack_from("<HHHH", blob, 0x08A5)
        opc = [struct.unpack_from("<H", blob, 0x142A + i * 2)[0] for i in range(16)]
        print(f"Debiased: {patches} immediates (capstone-guided)")
        print(f"  AH[0..3]={ah[0]:04X} {ah[1]:04X} {ah[2]:04X} {ah[3]:04X}")
        print(f"  OPC[0..15]={' '.join(f'{x:04X}' for x in opc)}")
        print(f"  timer @00FB={blob[0xFB:0x107].hex()}")
        print(f"  IRQ mov ax @0251={blob[0x251:0x254].hex()}")
        print(f"  LOAD @120E={blob[0x120E:0x1214].hex()}")
        print(f"  enable-or @01C7={blob[0x1C7:0x1CB].hex()} (expect 08160a03)")
        print(f"  cb-store @01BC={blob[0x1BC:0x1C0].hex()} (expect 89bf0003)")


    def install_delay_skip(self):
        """Skip OPNA I/O delays and PIT/INT1C calibration busy-waits."""
        cs = self.music_cs

        def on_io_delay(mu, address, size, data):
            mu.reg_write(UC_X86_REG_CX, 1)

        for off in (0x2258, 0x2231):
            a = phys(cs, off)
            self.mu.hook_add(UC_HOOK_CODE, on_io_delay, None, a, a)

        # 08C9 and 092E calibrate via IRQ / INT 1C — neither fires under Unicorn.
        # Replace each with: mov ax, imm; ret
        self.mem_w(cs, 0x08C9, bytes.fromhex("b87000c3"))  # mov ax,0x70; ret
        self.mem_w(cs, 0x092E, bytes.fromhex("b87000c3"))  # mov ax,0x70; ret

        self.mem_w(cs, 0x221E, p16(1))
        self.mem_w(cs, 0x221C, p16(1))
        print("Installed delay-skip + calibration stubs")


    def dump_table(self, label: str):

        raw = self.mem_r(self.music_cs, 0x08A5, 16)
        words = [struct.unpack_from("<H", raw, i)[0] for i in range(0, 16, 2)]
        print(f"  [{label}] AH@08A5: " + " ".join(f"{w:04X}" for w in words))

    def far_call(self, seg: int, off: int, ax: int = 0, ds: int | None = None,
                 timeout_us: int = 5_000_000) -> bool:
        hlt = 0x0100
        self.mem_w(STUB_SEG, hlt, b"\xF4\xF4\xCB")
        sp = 0xFF00
        self.mem_w(STUB_SEG, sp, p16(hlt) + p16(STUB_SEG))
        self.w(UC_X86_REG_SS, STUB_SEG)
        self.w(UC_X86_REG_SP, sp)
        self.w(UC_X86_REG_CS, seg)
        self.w(UC_X86_REG_IP, off)
        self.w(UC_X86_REG_AX, ax)
        dseg = ds if ds is not None else seg
        self.w(UC_X86_REG_DS, dseg)
        self.w(UC_X86_REG_ES, dseg)
        self.w(UC_X86_REG_FLAGS, 0x0202)
        try:
            self.mu.emu_start(phys(seg, off), phys(STUB_SEG, hlt) + 1,
                              timeout=timeout_us)
            return True
        except UcError as e:
            print(f"  far_call {seg:04X}:{off:04X} AX={ax:04X}: {e} "
                  f"@ {self.r(UC_X86_REG_CS):04X}:{self.r(UC_X86_REG_IP):04X}")
            return False

    def run_init(self):
        print("INIT AH=0...")
        ok = self.far_call(*self.api["INIT"], ax=0, timeout_us=8_000_000)
        ivt8 = struct.unpack("<HH", self.mem_r(0, 0x20, 4))
        print(f"  ok={ok} AX={self.r(UC_X86_REG_AX):04X} writes={len(self.chip.writes)} "
              f"IVT8={ivt8[1]:04X}:{ivt8[0]:04X}")
        if ivt8[1] or ivt8[0]:
            self.timer_handler = (ivt8[1], ivt8[0])
        self.dump_table("post-INIT")
        print("Config AX=0x0D07 via TRAMP...")
        self.far_call(*self.api["TRAMP"], ax=0x0D07, timeout_us=3_000_000)
        print(f"  AX={self.r(UC_X86_REG_AX):04X} writes={len(self.chip.writes)}")

    def load_song(self, song: bytes):
        self.mem_w(SONG_SEG, 0, song + b"\x00" * 32)
        s, o = self.api["LOAD"]
        stub = bytearray()
        stub += b"\x68" + p16(SONG_SEG)
        stub += b"\x68\x00\x00"
        stub += b"\x9A" + p16(o) + p16(s)
        stub += b"\x83\xC4\x04"
        stub += b"\xF4"
        self.mem_w(STUB_SEG, 0x0200, bytes(stub))
        self.w(UC_X86_REG_SS, STUB_SEG)
        self.w(UC_X86_REG_SP, 0xFF00)
        self.w(UC_X86_REG_CS, STUB_SEG)
        self.w(UC_X86_REG_IP, 0x0200)
        self.w(UC_X86_REG_DS, self.music_cs)
        self.w(UC_X86_REG_ES, SONG_SEG)
        self.w(UC_X86_REG_FLAGS, 0x0202)
        print(f"LOAD song ({len(song)} bytes)...")
        try:
            self.mu.emu_start(phys(STUB_SEG, 0x0200),
                              phys(STUB_SEG, 0x0200 + len(stub) - 1),
                              timeout=8_000_000)
        except UcError as e:
            print(f"  LOAD err: {e} @ "
                  f"{self.r(UC_X86_REG_CS):04X}:{self.r(UC_X86_REG_IP):04X}")
        print(f"  LOAD AX={self.r(UC_X86_REG_AX):04X} writes={len(self.chip.writes)}")

    def play(self):
        if "STOP" not in self.api:
            return
        s, o = self.api["STOP"]
        stub = bytearray()
        stub += b"\x6A\x00"  # push 0 = start (1 = stop)
        stub += b"\x9A" + p16(o) + p16(s)
        stub += b"\x83\xC4\x02"
        stub += b"\xF4"
        self.mem_w(STUB_SEG, 0x0300, bytes(stub))
        self.w(UC_X86_REG_SS, STUB_SEG)
        self.w(UC_X86_REG_SP, 0xFF00)
        self.w(UC_X86_REG_CS, STUB_SEG)
        self.w(UC_X86_REG_IP, 0x0300)
        self.w(UC_X86_REG_DS, self.music_cs)
        self.w(UC_X86_REG_FLAGS, 0x0202)
        print(f"PLAY via {s:04X}:{o:04X}...")
        try:
            self.mu.emu_start(phys(STUB_SEG, 0x0300),
                              phys(STUB_SEG, 0x0300 + len(stub) - 1),
                              timeout=3_000_000)
        except UcError as e:
            print(f"  PLAY err: {e} @ "
                  f"{self.r(UC_X86_REG_CS):04X}:{self.r(UC_X86_REG_IP):04X}")
        print(f"  PLAY AX={self.r(UC_X86_REG_AX):04X} writes={len(self.chip.writes)}")
        # show enable mask
        mask = self.mem_r(self.music_cs, 0x030A, 1)[0]
        print(f"  timer enable mask [030A]={mask:02X}")

    def fire_timer(self, ticks: int):
        if self.timer_handler:
            cs, ip = self.timer_handler
        else:
            ivt8 = struct.unpack("<HH", self.mem_r(0, 0x20, 4))
            if ivt8[1] or ivt8[0]:
                cs, ip = ivt8[1], ivt8[0]
            else:
                cs, ip = self.music_cs, 0x00F4
            print(f"Using timer {cs:04X}:{ip:04X}")
        print(f"Timer {cs:04X}:{ip:04X} x {ticks}")
        ret = 0x0400
        self.mem_w(STUB_SEG, ret, b"\xF4")
        for t in range(ticks):
            self.chip.tick = t
            sp = 0xF000
            self.mem_w(STUB_SEG, sp, p16(ret) + p16(STUB_SEG) + p16(0x0202))
            self.w(UC_X86_REG_SS, STUB_SEG)
            self.w(UC_X86_REG_SP, sp)
            self.w(UC_X86_REG_CS, cs)
            self.w(UC_X86_REG_IP, ip)
            self.w(UC_X86_REG_DS, cs)
            self.w(UC_X86_REG_ES, cs)
            self.w(UC_X86_REG_FLAGS, 0x0202)
            try:
                self.mu.emu_start(phys(cs, ip), phys(STUB_SEG, ret) + 1,
                                  timeout=500_000)
            except UcError as e:
                if t < 5:
                    print(f"  tick {t} err: {e} @ "
                          f"{self.r(UC_X86_REG_CS):04X}:{self.r(UC_X86_REG_IP):04X}")
            if t < 5 or t % 200 == 0 or t == ticks - 1:
                print(f"  tick {t}: opna={len(self.chip.writes)} midi={len(self.chip.midi)}")


def extract_song(pac: bytes, index: int) -> bytes:
    sizes = [struct.unpack_from("<I", pac, i * 4)[0] for i in range(105)]
    off = 105 * 4
    for i, sz in enumerate(sizes):
        if i == index:
            return pac[off:off + sz]
        off += sz
    raise IndexError(index)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--pac", required=True)
    ap.add_argument("--song", type=int, default=0)
    ap.add_argument("--ticks", type=int, default=500)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    h = Harness(Path(args.exe))
    if not h.find_api():
        return 1
    h.debias_music_module()
    h.install_delay_skip()
    h.dump_table("pre-INIT")
    h.run_init()
    song = extract_song(Path(args.pac).read_bytes(), args.song)
    h.load_song(song)
    h.play()
    h.fire_timer(args.ticks)

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        f.write(f"# HSB3 song={args.song} ticks={args.ticks} cs={h.music_cs:04X}\n")
        f.write(f"# timer={h.timer_handler} opna={len(h.chip.writes)} "
                f"midi={len(h.chip.midi)}\n")
        f.write(f"# ports={sorted(h.chip.ports)}\n")
        for tick, chip, page, reg, val in h.chip.writes:
            f.write(f"{tick:6d} {chip} p{page} r{reg:02X}={val:02X}\n")
        for item in h.chip.midi:
            if len(item) == 3:
                tick, port, b = item
                f.write(f"{tick:6d} MIDI p{port:04X} {b:02X}\n")
            else:
                tick, b = item
                f.write(f"{tick:6d} MIDI {b:02X}\n")
    print(f"Wrote {out} ({len(h.chip.writes)} OPNA, {len(h.chip.midi)} MIDI)")
    print(f"Ports: {sorted(h.chip.ports)}")
    return 0 if len(h.chip.writes) > 10 else 2


if __name__ == "__main__":
    sys.exit(main())
