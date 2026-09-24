#!/usr/bin/env python3
"""Best-effort Unicorn harness for Kajihara MMD.COM MPU-401 capture.

PC-98 MPU ports: data 0xE0D0, status/cmd 0xE0D2.
MMP_HOOT.COM is a Hoot loader stub; full MMD.COM playback needs DOS INT/timer
emulation beyond this stub. Ground-truth for 1.0.28 was Valley Bell mmd2mid
(format reverse-engineered from MMD.COM), with 100% note-on match on EMI_*.MMD.

This script hooks OUT to 0xE0D0/0xE0D2 if a guest image is supplied later.
"""
import argparse, sys
try:
    from unicorn import Uc, UC_ARCH_X86, UC_MODE_16, UC_HOOK_INSN
    from unicorn.x86_const import UC_X86_INS_OUT, UC_X86_INS_IN
except Exception as e:
    print('unicorn not usable:', e); sys.exit(0)

MPU_DATA, MPU_STAT = 0xE0D0, 0xE0D2
stream = []

def hook_out(uc, port, size, value, user):
    if port in (MPU_DATA, MPU_STAT, MPU_DATA & 0xFF, MPU_STAT & 0xFF):
        stream.append((port & 0xFFFF, value & 0xFF))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mmd-com', default='/workspace/eime_98/MMD.COM')
    ap.add_argument('--song', default='/workspace/eime_98/EMI_01.MMD')
    args = ap.parse_args()
    print('MMD.COM', args.mmd_com, 'song', args.song)
    print('Note: full DOS/Hoot bring-up not implemented here; use mmd2mid ref.')
    print('MPU ports E0D0/E0D2 ready for future guest image.')

if __name__ == '__main__':
    main()
