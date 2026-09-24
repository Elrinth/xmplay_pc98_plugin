# xmp-pc98 1.0.33

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

VERSIONINFO **1.0.33.0**; `PLUGIN_XMPVER` **1003300**.

## 1.0.33

- **Eikan instruments**: opcode `0x84` is FM program-change (was misread as volume).
  Load the NTL-embedded 32-byte YM2608 voice bank and apply DT/MUL/TL/…/FB-ALG
  like HSB3. Volume is `0x8F` with **0 = loudest**.
- Fixed Unicorn harness debias that turned `mov di,0x8064` into `0x1D04`, so
  reference captures used **code bytes as instruments**.
- User XMPlay recording matched song **1**; the “broken” sound was the single
  default patch on every FM channel.

## 1.0.32

- Eikan dialect from live HSB3 opcodes (`8E` loops, `8B` song loop, `89`/`8A`
  word→`[di+8]`, `88` tempo, PIT+rate timing, conductor `typ&0x80`).
- Eikan detect via stream preamble `8F`/`96` at off+3 (SJIS titles no longer break it).
- Key-on match vs Unicorn refs songs 1/2/3/10: 100% (tol=2). GM path deferred.

## 1.0.31

- First ArtDink Eikan dialect attempt (incorrect `0x8A` loop / muted conductor).
  Superseded by 1.0.32+.

## Install

Copy `xmp-pc98.dll` next to `xmplay.exe`.

**OPNA rhythm (optional):** `ym2608_adpcm_rom.bin` or `2608_*.WAV` beside the DLL.
**GS/SC-55 SF2:** for G_* NTL / PAC GM songs and MMD/FMD.

## Build

```bash
/usr/bin/make dll
/usr/bin/make pack   # → dist/xmp-pc98-1.0.33.zip (DLL + README + LICENSE)
```
