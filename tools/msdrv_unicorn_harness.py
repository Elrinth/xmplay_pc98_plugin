#!/usr/bin/env python3
"""
Ground-truth MsDRV4 register capture via Unicorn (16-bit real mode).

Loads MSDRV4L.EXE as a DOS TSR, provides INT 21h / chip I/O / timer services,
copies song + EC.OPN/EC.SSG into driver buffers, starts playback, fires INT8,
and logs every OPN/OPNA/OPL3 register write.

  python3 tools/msdrv_unicorn_harness.py \
    --exe /path/MSDRV4L.EXE --song EC_10_N.MS --bank-dir /path \
    --board opn|opna|opl --ticks 4000 --out ref.reg
"""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

from unicorn import (
    Uc, UcError, UC_ARCH_X86, UC_MODE_16,
    UC_HOOK_INTR, UC_HOOK_INSN, UC_HOOK_CODE,
)
from unicorn.x86_const import (
    UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
    UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP, UC_X86_REG_SP,
    UC_X86_REG_CS, UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS,
    UC_X86_REG_IP, UC_X86_REG_FLAGS,
    UC_X86_INS_IN, UC_X86_INS_OUT,
)

LOAD_SEG = 0x1000
PSP_SEG = LOAD_SEG - 0x10
STUB_SEG = 0xE000
MEM_SIZE = 1 << 20
DELAY_OFF = 0x6BB7
DELAY_RET = 0x6BBE
INT8_OFF = 0x4E98


class ChipBus:
    def __init__(self, board: str):
        self.board = board  # opn | opna | opl
        self.addr = [0, 0]
        self.regs = [bytearray(b"\xff" * 256), bytearray(b"\xff" * 256)]
        if board == "opna":
            self.regs[0][0xFF] = 0x00  # detect: read 0xFF != 0xFF → OPNA
            self.regs[1][0xFF] = 0x00
        self.opl_addr = [0, 0]
        self.opl_regs = [bytearray(256), bytearray(256)]
        self.sb_state = 0
        self.writes: list[tuple[int, str, int, int, int]] = []
        self.tick = 0

    def log(self, chip: str, port: int, reg: int, val: int):
        self.writes.append((self.tick, chip, port, reg & 0xFF, val & 0xFF))

    def in_port(self, port: int, size: int) -> int:
        port &= 0xFFFF
        if port in (0x00, 0x02, 0x71, 0x75, 0x77, 0x5F, 0x64):
            return 0x00
        # OPN/OPNA primary
        if port == 0x188:
            return 0x00  # status ready
        if port == 0x18A:
            if self.board not in ("opn", "opna"):
                return 0xFF
            return self.regs[0][self.addr[0]]
        if port == 0x18C:
            return 0x00 if self.board == "opna" else 0xFF
        if port == 0x18E:
            if self.board != "opna":
                return 0xFF
            return self.regs[1][self.addr[1]]
        # Alternate bases: absent
        if port in (0x088, 0x08A, 0x08C, 0x08E, 0x288, 0x28A, 0x28C, 0x28E):
            return 0xFF
        # Machine / PnP probes
        if port in (0xA460, 0x148E, 0x148F, 0xF40, 0xF4A, 0xF4B, 0xE0DA):
            return 0xFF
        # OPL / SB presence (PC-98 Speak Board style ports used by MsDRV)
        if self.board == "opl":
            # Presence: IN 20D2 != FF, then IN 81D2 bit6==0 → found
            if port == 0x81D2:
                return 0x00
            # SB16 DSP reset check (base 0x20D2):
            #   status = base+0x0E00 (0x2ED2) must have bit7 set
            #   data   = base+0x0A00 (0x2AD2) must read 0xAA
            if port in (0x2ED2, 0x2ED3) and self.sb_state == 2:
                return 0x80
            if port in (0x2AD2, 0x2AD3) and self.sb_state == 2:
                return 0xAA
            if port in (0x20D2, 0x20D4, 0x20D6, 0x20D8, 0x20DA, 0x20DC, 0x20DE,
                        0x21D2, 0x22D2, 0x23D2):
                return 0x00
            if 0x20D0 <= port <= 0x23FF:
                return 0x00
            if port in (0x26D2, 0x26D3):
                return 0x00
        else:
            if 0x20D0 <= port <= 0x2FFF:
                return 0xFF
        return 0xFF if size == 1 else 0xFFFF

    def out_port(self, port: int, size: int, value: int):
        port &= 0xFFFF
        v = value & (0xFF if size == 1 else 0xFFFF)
        v8 = v & 0xFF
        if port in (0x5F, 0x00, 0x02, 0x71, 0x75, 0x77, 0x64):
            return
        if self.board in ("opn", "opna"):
            if port == 0x188:
                self.addr[0] = v8
                return
            if port == 0x18A:
                self.regs[0][self.addr[0]] = v8
                chip = "OPN" if self.board == "opn" else "OPNA"
                self.log(chip, 0, self.addr[0], v8)
                return
            if port == 0x18C and self.board == "opna":
                self.addr[1] = v8
                return
            if port == 0x18E and self.board == "opna":
                self.regs[1][self.addr[1]] = v8
                self.log("OPNA", 1, self.addr[1], v8)
                return
        if self.board == "opl":
            if port == 0x20D2:
                self.opl_addr[0] = v8
                return
            if port == 0x21D2:
                self.opl_regs[0][self.opl_addr[0]] = v8
                self.log("OPL3", 0, self.opl_addr[0], v8)
                return
            if port == 0x22D2:
                self.opl_addr[1] = v8
                return
            if port == 0x23D2:
                self.opl_regs[1][self.opl_addr[1]] = v8
                self.log("OPL3", 1, self.opl_addr[1], v8)
                return
            if port in (0x26D2, 0x26D3):
                if v8 == 1:
                    self.sb_state = 1
                elif v8 == 0 and self.sb_state == 1:
                    self.sb_state = 2
                return


class Harness:
    def __init__(self, exe_path: Path, board: str, cmdline: str = ""):
        self.board = board
        self.chip = ChipBus(board)
        self.mu = Uc(UC_ARCH_X86, UC_MODE_16)
        self.mu.mem_map(0, MEM_SIZE)
        self.dos_prints: list[str] = []
        self.exited = False
        self.exit_code = None
        self.tsr_done = False
        self.tsr_paras = 0
        self._load_exe(exe_path, cmdline)
        self._hooks()
        # FD80 BIOS id — avoid special canbe path
        self.mu.mem_write(0xFD800 + 2, struct.pack("<HB", 0x0000, 0xFF))

    def phys(self, seg: int, off: int) -> int:
        return ((seg & 0xFFFF) << 4) + (off & 0xFFFF)

    def set_ivt(self, n: int, seg: int, off: int):
        self.mu.mem_write(n * 4, struct.pack("<HH", off & 0xFFFF, seg & 0xFFFF))

    def get_ivt(self, n: int) -> tuple[int, int]:
        off, seg = struct.unpack("<HH", bytes(self.mu.mem_read(n * 4, 4)))
        return seg, off

    def _load_exe(self, path: Path, cmdline: str):
        raw = path.read_bytes()
        assert raw[:2] == b"MZ"
        (last, pages, nreloc, header_paras, _mina, _maxa,
         ss, sp, _ck, ip, cs, reloc_off, _ov) = struct.unpack_from("<HHHHHHHHHHHHH", raw, 2)
        hdr = header_paras * 16
        img_size = pages * 512 - (512 - last if last else 0)
        img = bytearray(raw[hdr:hdr + img_size])
        for i in range(nreloc):
            off, seg = struct.unpack_from("<HH", raw, reloc_off + i * 4)
            pos = seg * 16 + off
            val = struct.unpack_from("<H", img, pos)[0]
            struct.pack_into("<H", img, pos, (val + LOAD_SEG) & 0xFFFF)

        psp = bytearray(0x100)
        psp[0:2] = b"\xCD\x20"
        struct.pack_into("<H", psp, 0x02, 0x9FFF)
        env_seg = 0x0F00
        self.mu.mem_write(env_seg << 4, b"\x00\x00\x01\x00MSDRV4L.EXE\x00")
        struct.pack_into("<H", psp, 0x2C, env_seg)
        cmd = cmdline.encode("ascii", "replace")[:126]
        psp[0x80] = len(cmd)
        psp[0x81:0x81 + len(cmd)] = cmd
        psp[0x81 + len(cmd)] = 0x0D
        self.mu.mem_write(PSP_SEG << 4, bytes(psp))
        self.mu.mem_write(LOAD_SEG << 4, bytes(img))

        self.mu.reg_write(UC_X86_REG_CS, (cs + LOAD_SEG) & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_IP, ip)
        self.mu.reg_write(UC_X86_REG_SS, (ss + LOAD_SEG) & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_SP, sp)
        self.mu.reg_write(UC_X86_REG_DS, PSP_SEG)
        self.mu.reg_write(UC_X86_REG_ES, PSP_SEG)
        for r in (UC_X86_REG_AX, UC_X86_REG_BX, UC_X86_REG_CX, UC_X86_REG_DX,
                  UC_X86_REG_SI, UC_X86_REG_DI, UC_X86_REG_BP):
            self.mu.reg_write(r, 0)
        self.mu.reg_write(UC_X86_REG_FLAGS, 0x0202)

        self.mu.mem_write(STUB_SEG << 4, b"\xCF")  # IRET
        self.set_ivt(0x21, STUB_SEG, 0x0100)
        self.set_ivt(0x08, STUB_SEG, 0x0000)
        self.set_ivt(0x61, 0, 0)

    def _hooks(self):
        self.mu.hook_add(UC_HOOK_INTR, self._on_intr)
        self.mu.hook_add(UC_HOOK_INSN, self._on_out, None, 1, 0, UC_X86_INS_OUT)
        self.mu.hook_add(UC_HOOK_INSN, self._on_in, None, 1, 0, UC_X86_INS_IN)
        dlin = (LOAD_SEG << 4) + DELAY_OFF
        self.mu.hook_add(UC_HOOK_CODE, self._on_delay, None, dlin, dlin)

    def _on_delay(self, uc, address, size, user):
        uc.reg_write(UC_X86_REG_IP, DELAY_RET)

    def _on_out(self, uc, port, size, value, user):
        self.chip.out_port(port, size, value)

    def _on_in(self, uc, port, size, user):
        return self.chip.in_port(port, size)

    def _read_dollar(self, seg: int, off: int) -> str:
        out = bytearray()
        for i in range(512):
            b = self.mu.mem_read(self.phys(seg, off + i), 1)[0]
            if b == ord("$"):
                break
            out.append(b)
        return out.decode("cp932", "replace")

    def _push_irq_frame(self):
        """Unicorn INT hook: IP already past INT; no frame pushed. Build one for IRET."""
        sp = self.mu.reg_read(UC_X86_REG_SP)
        ss = self.mu.reg_read(UC_X86_REG_SS)
        ip = self.mu.reg_read(UC_X86_REG_IP)
        cs = self.mu.reg_read(UC_X86_REG_CS)
        flags = self.mu.reg_read(UC_X86_REG_FLAGS)
        sp = (sp - 6) & 0xFFFF
        self.mu.mem_write(self.phys(ss, sp), struct.pack("<HHH", ip, cs, flags))
        self.mu.reg_write(UC_X86_REG_SP, sp)
        # Clear IF/TF like real INT
        self.mu.reg_write(UC_X86_REG_FLAGS, flags & ~0x0200)

    def _halt_stub(self):
        self.mu.mem_write((STUB_SEG << 4) + 0x200, b"\xF4")
        self.mu.reg_write(UC_X86_REG_CS, STUB_SEG)
        self.mu.reg_write(UC_X86_REG_IP, 0x0200)

    def _on_intr(self, uc, intno, user):
        # Unicorn: IP is already advanced past CD xx; stack frame NOT pushed.
        if intno == 0x21:
            self._int21()
            return
        if intno in (0x61, 0x08):
            seg, off = self.get_ivt(intno)
            if intno == 0x61 and seg == 0 and off == 0:
                uc.reg_write(UC_X86_REG_BP, 0x8000)
                return
            self._push_irq_frame()
            uc.reg_write(UC_X86_REG_CS, seg)
            uc.reg_write(UC_X86_REG_IP, off)
            return
        # ignore other INTs
        return

    def _int21(self):
        ax = self.mu.reg_read(UC_X86_REG_AX)
        ah, al = (ax >> 8) & 0xFF, ax & 0xFF
        flags = self.mu.reg_read(UC_X86_REG_FLAGS)
        if ah == 0x09:
            s = self._read_dollar(self.mu.reg_read(UC_X86_REG_DS),
                                  self.mu.reg_read(UC_X86_REG_DX))
            self.dos_prints.append(s)
            sys.stderr.write(s.replace("\r\n", "\n").replace("\r", "\n"))
            if not s.endswith("\n"):
                sys.stderr.write("\n")
            return
        if ah == 0x06:
            dl = self.mu.reg_read(UC_X86_REG_DX) & 0xFF
            if dl == 0xFF:
                self.mu.reg_write(UC_X86_REG_AX, ax & 0xFF00)
                self.mu.reg_write(UC_X86_REG_FLAGS, flags | 0x40)
            return
        if ah == 0x25:
            self.set_ivt(al, self.mu.reg_read(UC_X86_REG_DS),
                         self.mu.reg_read(UC_X86_REG_DX))
            return
        if ah == 0x35:
            seg, off = self.get_ivt(al)
            self.mu.reg_write(UC_X86_REG_ES, seg)
            self.mu.reg_write(UC_X86_REG_BX, off)
            return
        if ah == 0x49:
            self.mu.reg_write(UC_X86_REG_FLAGS, flags & ~1)
            return
        if ah == 0x31:
            self.tsr_done = True
            self.tsr_paras = self.mu.reg_read(UC_X86_REG_DX)
            self.exit_code = al
            self.exited = True
            self._halt_stub()
            return
        if ah == 0x4C:
            self.exited = True
            self.exit_code = al
            self._halt_stub()
            return
        sys.stderr.write(f"[dos] unhandled INT21 AH={ah:02X} AX={ax:04X}\n")
        return

    def run_install(self, max_insns: int = 80_000_000) -> bool:
        try:
            cs = self.mu.reg_read(UC_X86_REG_CS)
            ip = self.mu.reg_read(UC_X86_REG_IP)
            self.mu.emu_start(self.phys(cs, ip), 0xFFFFFFFF, count=max_insns)
        except UcError as e:
            if self.tsr_done:
                return True
            cs = self.mu.reg_read(UC_X86_REG_CS)
            ip = self.mu.reg_read(UC_X86_REG_IP)
            sys.stderr.write(f"[emu] install UcError at {cs:04X}:{ip:04X}: {e}\n")
            return False
        return self.tsr_done

    def call_int61(self, ax: int, bx: int = 0, cx: int = 0, dx: int = 0,
                   es: int | None = None, di: int = 0, si: int = 0,
                   max_insns: int = 8_000_000) -> dict:
        stub = 0x0300
        self.mu.mem_write((STUB_SEG << 4) + stub, b"\xCD\x61\xF4")
        self.mu.reg_write(UC_X86_REG_AX, ax & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_BX, bx & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_CX, cx & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_DX, dx & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_DI, di & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_SI, si & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_BP, 0)
        if es is not None:
            self.mu.reg_write(UC_X86_REG_ES, es & 0xFFFF)
        self.mu.reg_write(UC_X86_REG_DS, LOAD_SEG)
        self.mu.reg_write(UC_X86_REG_SS, STUB_SEG)
        self.mu.reg_write(UC_X86_REG_SP, 0x0F00)
        self.mu.reg_write(UC_X86_REG_CS, STUB_SEG)
        self.mu.reg_write(UC_X86_REG_IP, stub)
        self.mu.reg_write(UC_X86_REG_FLAGS, 0x0202)
        try:
            self.mu.emu_start(self.phys(STUB_SEG, stub),
                              self.phys(STUB_SEG, stub + 2),
                              count=max_insns)
        except UcError as e:
            if "hlt" not in str(e).lower() and "halt" not in str(e).lower():
                cs = self.mu.reg_read(UC_X86_REG_CS)
                ip = self.mu.reg_read(UC_X86_REG_IP)
                raise RuntimeError(
                    f"INT61 AX={ax:04X} at {cs:04X}:{ip:04X}: {e}") from e
        return {k: self.mu.reg_read(r) for k, r in (
            ("ax", UC_X86_REG_AX), ("bx", UC_X86_REG_BX), ("cx", UC_X86_REG_CX),
            ("dx", UC_X86_REG_DX), ("si", UC_X86_REG_SI), ("di", UC_X86_REG_DI),
            ("bp", UC_X86_REG_BP), ("es", UC_X86_REG_ES), ("ds", UC_X86_REG_DS),
        )}

    def write_mem(self, seg: int, off: int, data: bytes):
        self.mu.mem_write(self.phys(seg, off), data)

    def fire_int8(self, times: int):
        seg, off = self.get_ivt(0x08)
        if (seg, off) == (STUB_SEG, 0):
            seg, off = LOAD_SEG, INT8_OFF
            self.set_ivt(0x08, seg, off)
        stub = 0x0400
        for i in range(times):
            self.chip.tick = i
            self.mu.mem_write((STUB_SEG << 4) + stub, b"\xCD\x08\xF4")
            self.mu.reg_write(UC_X86_REG_SS, STUB_SEG)
            self.mu.reg_write(UC_X86_REG_SP, 0x0E00)
            self.mu.reg_write(UC_X86_REG_CS, STUB_SEG)
            self.mu.reg_write(UC_X86_REG_IP, stub)
            self.mu.reg_write(UC_X86_REG_FLAGS, 0x0202)
            try:
                self.mu.emu_start(self.phys(STUB_SEG, stub),
                                  self.phys(STUB_SEG, stub + 2),
                                  count=2_000_000)
            except UcError as e:
                if "hlt" in str(e).lower() or "halt" in str(e).lower():
                    continue
                cs = self.mu.reg_read(UC_X86_REG_CS)
                ip = self.mu.reg_read(UC_X86_REG_IP)
                raise RuntimeError(f"INT8@{i} {cs:04X}:{ip:04X}: {e}") from e


def cmdline_for(board: str) -> str:
    # Space-prefixed DOS cmdline tail. MA0/MB0 disable MIDI.
    if board == "opl":
        return " F0 MA0 MB0"
    return " MA0 MB0"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--song", type=Path, required=True)
    ap.add_argument("--bank-dir", type=Path, default=None)
    ap.add_argument("--board", choices=("opn", "opna", "opl"), required=True)
    ap.add_argument("--ticks", type=int, default=3000)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--prefix", default="EC")
    args = ap.parse_args()
    bank_dir = args.bank_dir or args.song.parent

    cl = cmdline_for(args.board)
    emu = Harness(args.exe, args.board, cmdline=cl)
    sys.stderr.write(f"[harness] install board={args.board} cmdline={cl!r}\n")
    if not emu.run_install():
        cs = emu.mu.reg_read(UC_X86_REG_CS)
        ip = emu.mu.reg_read(UC_X86_REG_IP)
        sys.stderr.write(
            f"[harness] TSR FAILED exited={emu.exited} code={emu.exit_code} "
            f"at {cs:04X}:{ip:04X}\n")
        for s in emu.dos_prints:
            sys.stderr.write(f"  print: {s!r}\n")
        return 2

    sys.stderr.write(f"[harness] TSR OK paras={emu.tsr_paras} INT61={emu.get_ivt(0x61)}\n")
    # Drive music from IRQ0/INT8 (not OPN-IRQ) so our harness can tick it.
    r = emu.call_int61(0x1700, bx=0)
    sys.stderr.write(f"[harness] set IRQ0 BP={r['bp']:04X} AH10={emu.call_int61(0x1000)['ax']:04X}\n")
    r = emu.call_int61(0x0000)
    sys.stderr.write(f"[harness] ver AX={r['ax']:04X} BP={r['bp']:04X}\n")
    r = emu.call_int61(0x1100)
    sys.stderr.write(f"[harness] board-info AX={r['ax']:04X}\n")

    song = args.song.read_bytes()
    r = emu.call_int61(0x2000)
    mus_es, mus_di = r["es"], r["di"]
    mus_sz = (r["dx"] << 16) | r["cx"]
    sys.stderr.write(f"[harness] music {mus_es:04X}:{mus_di:04X} cap={mus_sz} song={len(song)}\n")
    if mus_sz and len(song) > mus_sz:
        return 3
    emu.write_mem(mus_es, mus_di, song)

    # Banks: OPN file holds OPN and OPL (type@+0x2F) programs; SSG for FM boards.
    ssg_p = bank_dir / f"{args.prefix}.SSG"
    if ssg_p.exists():
        ssg = ssg_p.read_bytes()
        r = emu.call_int61(0x2020)
        sys.stderr.write(f"[harness] SSG {r['es']:04X}:{r['di']:04X} "
                         f"asz={r['ax']} n={r['cx']} file={len(ssg)}\n")
        emu.write_mem(r["es"], r["di"], ssg)
    opn_p = bank_dir / f"{args.prefix}.OPN"
    if opn_p.exists():
        opn = opn_p.read_bytes()
        r = emu.call_int61(0x2030)
        sys.stderr.write(f"[harness] OPN/OPL bank {r['es']:04X}:{r['di']:04X} "
                         f"asz={r['ax']} n={r['cx']} file={len(opn)}\n")
        emu.write_mem(r["es"], r["di"], opn)

    n0 = len(emu.chip.writes)
    r = emu.call_int61(0x2100)
    sys.stderr.write(f"[harness] start BP={r['bp']:04X} INT8={emu.get_ivt(0x08)}\n")
    emu.fire_int8(args.ticks)
    r = emu.call_int61(0x2001)
    sys.stderr.write(f"[harness] state AX={r['ax']:04X}\n")
    r = emu.call_int61(0x2002)
    steps = (r["dx"] << 16) | r["ax"]
    sys.stderr.write(f"[harness] steps={steps}\n")

    play = emu.chip.writes[n0:]
    if len(play) < 10:
        sys.stderr.write(f"[harness] WARN post-start writes={len(play)} total={len(emu.chip.writes)}\n")
        play = emu.chip.writes

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w") as f:
        f.write(f"# tick chip port reg val  (MSDRV4L Unicorn, board={args.board})\n")
        f.write(f"# song={args.song.name} ticks={args.ticks} steps={steps}\n")
        for tick, chip, port, reg, val in play:
            f.write(f"{tick} {chip} {port} {reg:02X} {val:02X}\n")
    sys.stderr.write(f"[harness] {len(play)} writes → {args.out}\n")
    return 0 if len(play) >= 10 else 4


if __name__ == "__main__":
    sys.exit(main())
