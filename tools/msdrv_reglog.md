# MsDRV register-log harness

## Native dump (this build)

```bash
MSDRV_REGLOG=/tmp/ec10_b2_native.reg ./render_or_xmplay EC_10_B2.MS
# Format: tick CHIP port reg val
# CHIP = OPN | OPNA | OPL3
```

Compare two logs:

```bash
python3 tools/msdrv_regdiff.py native.reg reference.reg
```

## Reference from real MSDRV4L.EXE (planned / partial)

Goal: run `MSDRV4L.EXE` + `MSP_HOOT.COM` under a PC-98 DOS environment and
log I/O to OPN `0x188/0x18A`, OPNA `0x188..0x18E`, OPL3 `0x20D2` (+data),
with tick timestamps from the driver's INT timer.

Options:
1. Neko Project II / DOSBox-X with an I/O hook (preferred when available).
2. A minimal 16-bit real-mode emulator hosting only the services the hoot stub
   needs (INT 21h load, INT 61h MsDRV API, timer IRQ). Not shipped in 1.0.22 —
   scaffolding only.

Place captured reference logs under `tools/msdrv_ref_logs/`.

## What 1.0.22 already aligned without a full emu

- OPN `_N`: true `ym2203`, mono L=R (was hard-panned right via 9F/B4 bug).
- OPNA `_B2`: ADPCM-A key-on bit7, D1–D6 levels, rhythm WAV/ROM load path.
- OPL3 `_SB`: clear BD rhythm mode, refuse type=0 patches, cmd 81 mode 00,
  waveform select already enabled.
- CH3 special (A9) + F0–F2 extension fnum writes.
