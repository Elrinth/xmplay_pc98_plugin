# xmp-pc98 1.0.25

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

This is **not** a wrap of S98Amp / `in_s98.dll`, `in_fmpmd.dll`, or ZXTune.
Same rule as xmp-gamemusic (“does not wrap `in_nez.dll`”).

Classic XMPlay is **32-bit only**. This DLL is PE32 i386.
VERSIONINFO FILEVERSION is **1.0.25.0**; `PLUGIN_XMPVER` is **1002500**.

## 1.0.25

- MsDRV (feedback was on **1.0.24**): CH3 special lead uses AA operator masks
  (`0x32` / `0xC2` / `0xF2` / `0x02`) — restores EC_10_N lead after FNUM/BLOCK fixes.
- MsDRV: YM2608 ADPCM-A key-on writes mask with bit7 **clear** (was dump/key-off);
  rhythm WAV voices stop on release and play at chip rate (`clock/432`) with TL.
- MsDRV: OPL fine pitch-bend + FNUM refresh while notes are held (EC_10_SB).
- File Info shows Chip / SF2 for MsDRV; Config → **Channel mutes** for live
  per-channel FM / SSG / Rhythm / OPL mute (does not break seek).

## 1.0.24

- MsDRV: FNUM table matched to MSDRV4L/MFD (`0x26A…0x48F`); OPNA uses same BLOCK as OPN
  (default /6 prescaler).
- OPN: skip YM2203-invalid B4 pan regs.
- OPL3 `_SB`: enable 4-op pairs (reg `0x104`), program all 4 operators + pair `C0`/`C3`.
- VERSIONINFO FileVersion/ProductVersion **strings** corrected to 1.0.24 (were stuck on 1.0.22).

## 1.0.23

- **Ground-truth MsDRV harness** (`tools/msdrv_unicorn_harness.py`): runs real
  `MSDRV4L.EXE` under Unicorn and logs OPN/OPNA/OPL3 writes. Refs in
  `tools/msdrv_ref_logs/EC_10_{N,B2,SB}_msdrv4l.reg`.
- **OPN/OPNA operator order**: patch bytes → HW ops `{0,4,8,12}` matching
  MSDRV4L (was docs-order `{0,8,4,12}`, which swapped OP2/OP3).

## Install

Copy `xmp-pc98.dll` next to `xmplay.exe` (or into XMPlay's plugin folder)
and restart XMPlay.

**Delete or disable S98Amp, in_fmpmd, and ZXTune’s S98 / `.M` claims**
so they do not fight this plugin. ZXTune `.M` is ZX Spectrum, not PMD —
we probe PMD magic so a Spectrum `.M` is not claimed.

Keep **xmp-gamemusic** for `.vgm` / `.vgz`. Keep **xmp-pokey** for `.sap`.
Keep XMPlay’s MIDI plugin for `.mid`.

## Formats

| Format | Core | Notes |
|--------|------|-------|
| **S98** | cisc fmgen (OPN / OPNA) | Same core as in_s98. Mix 55466 Hz, FM 0 / SSG −18 (PC-9801) or −8 (PC-8801). Tags, loop `FD`, seek, mute |
| **PMD** | pmdmini / PMDWin | `.M` / `.M2` / `.MZ` (+ `.PPC`/`.P86`/`.PPS`/`.PPZ` beside the song). Not Spectrum `.M` |
| **FMP** | WinFMP (via hootrip when needed) | `.OPI` / `.OPL` / `.OPN` |
| **BGMDRV** `.MUS` | ymfm (OPNA SSG+FM) | Kinpukurin / Faladia part table + sibling `TONE.DAT`. No hootrip |
| **MUTEN** `.NA` | ymfm | Packen |
| **USMD** `.USO` | fmgen | |
| **N3G** `.MDT` | ymfm | |
| **Synthia** `.PAI` | ymfm (OPNA) | `PAI 3.00M` only |
| **MBMUS** `.MSB` | ymfm | |
| **OPNDRV** `.MD` | ymfm (OPNA) | Oerstedia FM; needs `2608_*.WAV` next to the DLL |
| **NTL** `.NTL` | ymfm (OPNA) / TSF | `F_*` FM/SSG; `G_*` GM via SC-55 SF2 |
| **FMD** | ymfm / TSF | |
| **MsDRV** `.MS` | ymfm / TinySoundFont | Ekudorado `_N` OPN, `_B2` OPNA, `_GS`/`_88` GS, `_SB` OPL3 |

## Build

```bash
/usr/bin/make          # host tests + 32-bit DLL
/usr/bin/make dll      # dist/xmp-pc98.dll
/usr/bin/make test     # host render tests
/usr/bin/make pack     # dist/xmp-pc98-1.0.25.zip
```

Rhythm WAVs for OPNA/OPNDRV: place `2608_*.WAV` in `rhythm/` next to the DLL
(or set the Rhythm path in Config).

## Mute / File Info

- Config: group mutes (FM / SSG / Rhythm / ADPCM) and **Channel mutes** (live
  per-channel checkboxes; applied without restarting playback).
- File Info (XMPlay general info): Title, Format, Engine, Chip, SF2 (MsDRV GS), loops.

## License

See LICENSE. Third-party cores keep their own licenses (fmgen, pmdmini/ymfm, TinySoundFont).
