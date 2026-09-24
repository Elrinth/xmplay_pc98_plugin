# xmp-pc98 1.0.29

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

VERSIONINFO **1.0.29.0**; `PLUGIN_XMPVER` **1002900**.

## 1.0.29

- **MIDI hang across song switch**: MMD / FMD / MsDRV GS shared one cached
  TinySoundFont instance, so notes from song A kept ringing into song B.
  Soundfont samples stay cached (`fmd_font_get`); each open uses `tsf_copy`
  (`fmd_font_open`) and close/seek fully reset voices + channel state
  (`fmd_font_release` / fresh `tsf_copy` on seek). No 63 MB SC-55 reload per open.
- Host carry test (`tests/test_mmd_carry.cpp`): open A → render → close → open B;
  first 500 ms of B matches a fresh open; seek-to-0 matches cold open.
- Release zip is DLL+docs only (no `2608_*.WAV` / no ADPCM ROM).

## 1.0.28

- **Kajihara MMD.COM** `.MMD` (PMD MIDI / MC.EXE) via TinySoundFont GS.
  Distinct from FUGA XOR-A5 RCP `.MMD` / FMD pack.

## Install

Copy `xmp-pc98.dll` next to `xmplay.exe`.

**OPNA rhythm (optional, not in the release zip):** `ym2608_adpcm_rom.bin` (preferred)
or `2608_{BD,SD,TOP,HH,TOM,RIM}.WAV` beside the DLL / Rhythm path. If neither is
found, OPNA drums are silent; File Info shows ROM / WAV / NONE.

**GS SF2 (MMD / FMD / MsDRV GS):** set the GS soundfont path in plugin config
(e.g. SC-55.sf2). Samples are cached; song switches do not reload the bank.

## Build

```bash
/usr/bin/make dll
/usr/bin/make pack   # → dist/xmp-pc98-1.0.29.zip (DLL + README + LICENSE)
```
