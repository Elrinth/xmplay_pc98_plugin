# xmp-pc98 1.0.32

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

VERSIONINFO **1.0.32.0**; `PLUGIN_XMPVER` **1003200**.

## 1.0.32

- **Eikan dialect rewrite from live HSB3 opcode table**: notes bit6=length /
  pitch nibble; part streams at `off+3`; `typ&0x80` is conductor (same FM
  slot, not a mute channel). Sekigahara NTL path unchanged.
- **Loops/tempo**: `0x8E` = in-place `count`+`off16` loop; `0x8B` = song loop
  (`sub si,[si]`); `0x89`/`0x8A` write a word to `[di+8]` (not loops);
  `0x88` sets software tempo rate; PIT `0x2A00` @ ~1.9968 MHz + rate accumulator.
- **Also**: `0x81`/`0x8C` rests, `0x82` slur marker (lookahead at key-off),
  `0x8D` poke, `0x90` conditional skip when loop count==1. Eikan detect uses
  stream preamble `8F`/`96` at `off+3` (SJIS title bytes no longer break it).
- Key-on timing vs Unicorn OPNA `.reg` refs (songs 1/2/3/10): **100%** (tol=2).
- GM/MIDI PAC songs (35+) still via GNTL/SF2; MPU-401 `0xE0D0`/`0xE0D2` path
  in HSB3 not wired to a new capture yet — left as-is.

## 1.0.31

- First ArtDink Eikan dialect attempt (incorrectly treated `0x8A` as count/off8
  loop and muted conductor as channel 5). Superseded by 1.0.32.

## 1.0.30

- **ArtDink MUSIC.PAC** (Eikan wa Kimi ni 3): multi-song archive of `.NTL`
  blobs (LE32 size table + packed songs). 105 subsongs via GetSubSongs /
  Shift+arrows, `XMPIN_INFO_NOSUBTAGS` like set.pc98.
- Strict `.PAC` probe only (extension + size table that sums to the file, and
  a majority of chunks looking like ArtDink NTL/GNTL). Common random `.PAC`
  files are not claimed.
- FM songs → ymfm YM2608 (existing NTL engine); GM songs → SC-55 SF2 (GNTL).
- Hoot stub `ARTDI_98.COM` loads `HSB3.EXE` and calls the in-EXE ArtDink
  driver; songs are plain NTL, so the native NTL/GNTL engines are used.

## 1.0.29

- **MIDI hang across song switch**: MMD / FMD / MsDRV GS shared one cached
  TinySoundFont; per-open `tsf_copy` fixes bleed without reloading SF2.

## Install

Copy `xmp-pc98.dll` next to `xmplay.exe`.

**OPNA rhythm (optional):** `ym2608_adpcm_rom.bin` or `2608_*.WAV` beside the DLL.
**GS/SC-55 SF2:** for G_* NTL / PAC GM songs and MMD/FMD.

## Build

```bash
/usr/bin/make dll
/usr/bin/make pack   # → dist/xmp-pc98-1.0.32.zip (DLL + README + LICENSE)
```
