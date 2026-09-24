# xmp-pc98 1.0.30

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

VERSIONINFO **1.0.30.0**; `PLUGIN_XMPVER` **1003000**.

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
/usr/bin/make pack   # → dist/xmp-pc98-1.0.30.zip (DLL + README + LICENSE)
```
