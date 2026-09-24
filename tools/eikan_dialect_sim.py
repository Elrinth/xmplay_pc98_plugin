#!/usr/bin/env python3
"""ArtDink Eikan/HSB3 NTL dialect simulator — match against Unicorn .reg captures."""
from __future__ import annotations
import argparse, struct
from pathlib import Path

FNUM = [0x269,0x28E,0x2B4,0x2DE,0x30A,0x338,0x369,0x39C,0x3D3,0x40E,0x44B,0x48D]
SSG_PER = [0xEE8,0xE12,0xD48,0xC88,0xBD4,0xB2A,0xA8A,0x9F2,0x964,0x8DC,0x85E,0x7E6]
PIT_COUNT = 0x2A00
PIT_CLOCK = 1996800  # PC-98 8253

class Ch:
    __slots__ = ("on","end","ssg","cond","id","pc","start","end_pc","wait","gate",
                 "keyed","slur","oct","vol","det","def_len","eff","tl_extra","did_loop")
    def __init__(self):
        self.on=0; self.end=0; self.ssg=0; self.cond=0; self.id=0
        self.pc=0; self.start=0; self.end_pc=0; self.wait=1; self.gate=0
        self.keyed=0; self.slur=0; self.oct=4; self.vol=10; self.det=0
        self.def_len=0x30; self.eff=0; self.tl_extra=0; self.did_loop=0

class Eikan:
    def __init__(self, data: bytes):
        self.file = bytearray(data)
        self.file0 = bytes(data)
        self.ch = [Ch() for _ in range(9)]
        self.writes = []  # (tick, page, reg, val)
        self.tick = 0
        self.rate = 0x5000
        self.acc = 0
        self.tempo = 0x5000
        self.ssg_mix = 0xB8
        self.ended = 0
        self._parse()

    def _wr(self, page, reg, val):
        self.writes.append((self.tick, page, reg & 0xFF, val & 0xFF))

    def _parse(self):
        d = self.file
        n = len(d)
        cnt = d[3]
        parts = []
        for i in range(cnt):
            if 4+(i+1)*3 > n: break
            off = d[4+i*3] | (d[4+i*3+1]<<8)
            typ = d[4+i*3+2]
            off += 3  # eikan
            if off <= 0 or off >= n: continue
            raw = typ
            cond = 1 if (typ & 0x80) else 0
            cid = typ & 0x7F
            parts.append((off, cid, cond, raw))
        # assign by channel id
        used = []
        for off, cid, cond, raw in parts:
            hi = cid & 0xF0
            lo = cid & 0x0F
            if hi == 0x10 and lo < 6:
                slot = lo  # FM 0..5
            elif hi == 0x20 and lo < 3:
                slot = 6 + lo  # SSG
            else:
                continue
            ch = self.ch[slot]
            ch.on = 1
            ch.id = cid
            ch.cond = cond
            ch.ssg = 1 if hi == 0x20 else 0
            ch.pc = off
            ch.start = off
            used.append((slot, off))
        for slot, off in used:
            nxt = n
            for _, o2 in used:
                if o2 > off and o2 < nxt: nxt = o2
            self.ch[slot].end_pc = nxt

    def key_id(self, c):
        return (c - 3 + 4) if c >= 3 else c

    def keyoff(self, c):
        ch = self.ch[c]
        if ch.ssg:
            ssgc = c - 6
            self._wr(0, 0x08+ssgc, 0)
        else:
            self._wr(0, 0x28, self.key_id(c))
        ch.keyed = 0

    def play_note(self, c, name):
        ch = self.ch[c]
        # conductor (typ&0x80) still plays FM; flag only marks tempo master
        n = name & 0x0F
        oct = ch.oct
        while n > 11:
            n -= 12; oct += 1
        if oct < 0: oct = 0
        if oct > 7: oct = 7
        if ch.ssg:
            per = SSG_PER[n] if n < 12 else 0xEE8
            for _ in range(oct):
                per >>= 1
            per = max(1, min(0xFFF, per + ch.det))
            ssgc = c - 6
            self._wr(0, ssgc*2, per & 0xFF)
            self._wr(0, ssgc*2+1, (per>>8) & 0x0F)
            self._wr(0, 0x08+ssgc, ch.vol & 15)
            self.ssg_mix &= ~(1 << ssgc)
            self._wr(0, 0x07, self.ssg_mix)
            ch.keyed = 1
            return
        fn = FNUM[n if n < 12 else 0] + ch.det
        fn = max(0, min(0x7FF, fn))
        blk = oct << 3
        ext = 1 if c >= 3 else 0
        slot = c - 3 if ext else c
        if ch.keyed and not ch.slur:
            self.keyoff(c)
        self._wr(ext, 0xA4+slot, blk | ((fn>>8)&7))
        self._wr(ext, 0xA0+slot, fn & 0xFF)
        if not ch.slur or not ch.keyed:
            self._wr(0, 0x28, 0xF0 | self.key_id(c))
        ch.keyed = 1
        ch.slur = 0

    def fetchb(self, ch):
        if ch.pc < 0 or ch.pc >= len(self.file): return None
        b = self.file[ch.pc]; ch.pc += 1
        return b

    def do_cmd(self, c, cmd, measure):
        ch = self.ch[c]
        if cmd == 0x80:
            self.keyoff(c)
            ch.end = 1
            if measure: ch.did_loop = 1
            return
        if cmd == 0x81:  # rest with length
            a = self.fetchb(ch)
            if a is None: return
            ch.wait = a if a > 0 else 1
            # keyoff unless next is slur 82 — approximated: always keyoff for rest
            if not measure: self.keyoff(c)
            return
        if cmd == 0x82:  # nop / slur marker (lookahead handles slur)
            return
        if cmd == 0x83:
            a = self.fetchb(ch)
            if a is not None:
                ch.det = a - 0x20  # heuristic; live stores [di+0xb]
            return
        if cmd == 0x84:
            a = self.fetchb(ch)
            if a is not None:
                ch.vol = a & 15
            return
        if cmd == 0x85:
            n = self.fetchb(ch)
            if n is None: return
            ch.pc += n  # skip instrument/MIDI bytes
            return
        if cmd == 0x86:
            a = self.fetchb(ch)
            if a is not None: ch.oct = a & 7
            return
        if cmd == 0x87:
            a = self.fetchb(ch)
            if a is not None: ch.def_len = a if a > 0 else 1
            return
        if cmd == 0x88:  # tempo word → rate
            lo = self.fetchb(ch); hi = self.fetchb(ch)
            if lo is None or hi is None: return
            self.tempo = lo | (hi << 8)
            # 1383: dh=[1d02] (+[1d03]), dl=[1d01]
            r = self.tempo
            if r < 1: r = 1
            if r > 0xFFFF: r = 0xFFFF
            self.rate = r
            return
        if cmd in (0x89, 0x8A):  # word → [di+8] effect select
            lo = self.fetchb(ch); hi = self.fetchb(ch)
            if lo is None: return
            ch.eff = lo | ((hi or 0) << 8)
            return
        if cmd == 0x8B:  # unconditional relative jump: sub si,[si]
            if ch.pc + 1 >= len(self.file): return
            off = self.file[ch.pc] | (self.file[ch.pc+1] << 8)
            # si points at off16; si := si - off
            new_pc = ch.pc - off
            if measure:
                ch.did_loop = 1
                ch.end = 1
                return
            if new_pc < ch.start: new_pc = ch.start
            ch.pc = new_pc
            return
        if cmd == 0x8C:  # rest with default length
            ch.wait = ch.def_len if ch.def_len > 0 else 1
            self.keyoff(c)
            return
        if cmd == 0x8D:
            # 8D <val> <off16>: es:[si+off] = val (si at off16)
            v = self.fetchb(ch)
            if v is None or ch.pc + 1 >= len(self.file): return
            off = self.file[ch.pc] | (self.file[ch.pc+1]<<8)
            addr = ch.pc + off
            if 0 <= addr < len(self.file):
                self.file[addr] = v & 0xFF
            ch.pc += 2
            return
        if cmd == 0x8E:  # loop: dec count; je skip; else jump back by off16
            cpos = ch.pc
            if cpos >= len(self.file): return
            if measure:
                # body already played once; skip count+off16
                ch.pc = cpos + 3
                return
            cnt = self.file[cpos]
            if cnt == 0:
                ch.pc = cpos + 3
                return
            self.file[cpos] = (cnt - 1) & 0xFF
            if self.file[cpos] == 0:
                ch.pc = cpos + 3
            else:
                # inc si; sub si,[si] with si at off16 (=cpos+1)
                off = self.file[cpos+1] | (self.file[cpos+2] << 8)
                ch.pc = (cpos + 1) - off
            return
        if cmd == 0x8F:
            a = self.fetchb(ch)
            if a is not None: ch.vol = a & 15
            return
        if cmd == 0x90:
            # 90 <off16>: if es:[si+off]==1 then si += off+3 else si += 2
            # (si at off16). Used to skip final note+8E when loop count hits 1.
            if ch.pc + 1 >= len(self.file): return
            off = self.file[ch.pc] | (self.file[ch.pc+1]<<8)
            addr = ch.pc + off  # si+bx with si at off16
            val = self.file[addr] if 0 <= addr < len(self.file) else 0
            if val == 1:
                ch.pc = ch.pc + off + 3
            else:
                ch.pc += 2
            return
        if cmd == 0x91:
            a = self.fetchb(ch)
            if a is not None:
                # signed add to detune word
                if a >= 128: a -= 256
                ch.det += a
            return
        if cmd in (0x92, 0x93, 0x94, 0x95, 0x96, 0x9A):
            self.fetchb(ch)
            return
        if cmd in (0x97, 0x98, 0x99):
            ch.det = 0
            return
        if cmd == 0x9B:
            return
        if cmd == 0x9C:
            ch.oct = (ch.oct + 1) & 7
            return
        if cmd == 0x9D:
            ch.oct = (ch.oct - 1) & 7
            return
        if cmd == 0x9E:
            if ch.pc + 1 >= len(self.file): return
            ch.pc += 2  # skip
            return
        # A0-A2 gate helpers — set gate from [di+9]/wait; approximate no-op
        if cmd in (0xA0, 0xA1, 0xA2):
            return
        # unknown: skip 1 arg if in 80-BF
        if 0x80 <= cmd < 0xC0:
            self.fetchb(ch)

    def fetch(self, c, measure=False):
        ch = self.ch[c]
        guard = 0
        while guard < 256 and not ch.end:
            guard += 1
            if ch.pc < 0 or ch.pc >= len(self.file) or (ch.end_pc and ch.pc >= ch.end_pc):
                if measure:
                    ch.did_loop = 1; ch.end = 1; return
                if ch.start > 0:
                    ch.pc = ch.start; continue
                ch.end = 1; return
            # gate keyoff: when wait-1 == gate (handled in step)
            b = self.file[ch.pc]; ch.pc += 1
            if b < 0x40:
                ch.wait = ch.def_len if ch.def_len > 0 else 1
                self.play_note(c, b)
                return
            if b < 0x80:
                ln = self.fetchb(ch)
                if ln is None: ch.end = 1; return
                if ln < 1: ln = 1
                ch.wait = ln
                # live does NOT update def_len on length-byte notes
                self.play_note(c, b)
                return
            self.do_cmd(c, b, measure)
            if ch.wait > 0 and b in (0x81, 0x8C):
                return

    def music_step(self, measure=False):
        live = 0
        for i, ch in enumerate(self.ch):
            if not ch.on or ch.end: continue
            live += 1
            # dec wait
            ch.wait -= 1
            if ch.wait == 0:
                # keyoff unless next is 82
                if ch.pc < len(self.file) and self.file[ch.pc] == 0x82:
                    pass
                elif ch.keyed:
                    self.keyoff(i)
                self.fetch(i, measure)
            elif ch.gate and ch.wait == ch.gate and ch.keyed:
                if ch.pc < len(self.file) and self.file[ch.pc] == 0x82:
                    pass
                else:
                    self.keyoff(i)
        if not live:
            self.ended = 1

    def pit_tick(self):
        self.acc = (self.acc + self.rate) & 0xFFFF
        # overflow = carry from 16-bit add
        # Actually: add rate to acc; if CF then step. So:
        # prev = acc; acc = (acc+rate)&0xffff; if acc < prev or (acc+rate)>0xffff without wrap check
        # Better: sum = acc + rate; if sum > 0xffff: step; acc = sum & 0xffff
        pass

    def run_pit(self, n_ticks):
        for t in range(n_ticks):
            self.tick = t
            s = self.acc + self.rate
            if s > 0xFFFF:
                self.music_step()
            self.acc = s & 0xFFFF

    def measure_music_ticks(self, max_ticks=400000):
        # reset
        self.file = bytearray(self.file0)
        for ch in self.ch:
            if not ch.on: continue
            ch.end=0; ch.pc=ch.start; ch.wait=1; ch.keyed=0; ch.oct=4
            ch.def_len=0x30; ch.det=0; ch.did_loop=0; ch.gate=0
        self.ended=0; self.rate=0x5000; self.acc=0; self.tempo=0x5000
        ticks = 0
        while not self.ended and ticks < max_ticks:
            self.music_step(measure=True)
            en = sum(1 for ch in self.ch if ch.on)
            looped = sum(1 for ch in self.ch if ch.on and ch.did_loop)
            live = sum(1 for ch in self.ch if ch.on and not ch.end)
            if en and looped >= en: self.ended = 1
            if not live: self.ended = 1
            ticks += 1
        return ticks


def load_ref_keyons(path):
    keys = []
    with open(path) as f:
        for line in f:
            line=line.strip()
            if not line or line.startswith('#'): continue
            parts=line.split()
            if len(parts)<3: continue
            try:
                tick=int(parts[0])
            except ValueError:
                continue
            # "OPNA p0 r28=F0" or "p0 r28=F0"
            for p in parts[1:]:
                if p.startswith('r') and '=' in p:
                    reg,val = p[1:].split('=')
                    reg=int(reg,16); val=int(val,16)
                    if reg==0x28 and (val & 0xF0):
                        keys.append((tick, val & 0x0F, val))
                elif 'r28=' in p:
                    val=int(p.split('r28=')[1],16)
                    if val & 0xF0:
                        keys.append((tick, val & 0x0F, val))
    return keys


def match_keyons(sim_keys, ref_keys, tol=2):
    """Match (tick, ch) with timing tolerance. Return pct."""
    if not ref_keys: return 0.0, 0, 0
    used = [False]*len(sim_keys)
    matched = 0
    for rt, rc, rv in ref_keys:
        best = -1; best_d = 10**9
        for i,(st,sc,sv) in enumerate(sim_keys):
            if used[i]: continue
            if sc != rc: continue
            d = abs(st - rt)
            if d < best_d:
                best_d = d; best = i
        if best >= 0 and best_d <= tol:
            used[best] = True
            matched += 1
    return 100.0 * matched / len(ref_keys), matched, len(ref_keys)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ntl', required=True)
    ap.add_argument('--ref', required=True)
    ap.add_argument('--ticks', type=int, default=3000)
    ap.add_argument('--tol', type=int, default=3)
    args = ap.parse_args()
    data = Path(args.ntl).read_bytes()
    eng = Eikan(data)
    eng.run_pit(args.ticks)
    sim = [(t,v&0xF,v) for (t,p,r,v) in eng.writes if r==0x28 and (v&0xF0)]
    ref = load_ref_keyons(args.ref)
    pct, m, n = match_keyons(sim, ref, args.tol)
    print(f'key-on match: {m}/{n} = {pct:.1f}% (tol={args.tol})')
    print(f'sim keyons={len(sim)} ref={len(ref)} opna_writes={len(eng.writes)}')
    print('sim first', sim[:8])
    print('ref first', ref[:8])
    mt = eng.measure_music_ticks()
    # wall-clock estimate with average rate 0x8E30
    pit_hz = PIT_CLOCK / PIT_COUNT
    music_hz = pit_hz * 0x8E30 / 65536
    ms = int(mt / music_hz * 1000)
    print(f'one-loop music_ticks={mt} ~{ms}ms @ rate=8E30')
    return 0 if pct >= 95 else 1

if __name__ == '__main__':
    raise SystemExit(main())
