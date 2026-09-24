# xmp-pc98 1.0.27

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

VERSIONINFO **1.0.27.0**; `PLUGIN_XMPVER` **1002700**.

## 1.0.27

- **EC_10_SB drums**: OPL3 4-op CNT packing — primary CNT0 = tone byte0 bit1,
  pair CNT1 = bit0 (was both on primary; FB=7 hats became additive white noise).
- **EC_10_N**: PC-9801-26 SSG mix ≈ −8 dB (was OPNA −18 dB ÷8).
- **Rhythm source** shown in File Info; ROM preferred over WAV; neither → silent.
- Release zip is DLL+docs only (no `2608_*.WAV` / no ADPCM ROM).

- **Rhythm WAV path (EC_10_B2)**: volume law matches ymfm ADPCM-A
  (`atten=(level^0x1F)+(tl^0x3F)`, then mul/shift) plus a small gain so peak/RMS
  track the ROM render; playback at ADPCM-A rate (`clock/144`) with ROM-slot
  length truncate; step advances at chip rate. Pan bits from `0x18..0x1D`.
  Prefers `ym2608_adpcm_rom.bin` beside the DLL when present (ymfm ADPCM-A);
  otherwise `2608_*.WAV` (fmgen/PMDWin packs). ROM path unchanged.
- **EC_10_N**: SSG noise-period write `0x06=0` each update (matches MSDRV4L).
  Opening lead remains SSG at `/64` +12 (1.0.26). Unicorn “drums” on N are
  SSG tones (mixer keeps noise muted); the loud 1.0.25 “drums” were the
  same SSG pitched ~3 octaves low.


## Install

Copy `xmp-pc98.dll` next to `xmplay.exe`.

**OPNA rhythm (optional, not in the release zip):** `ym2608_adpcm_rom.bin` (preferred)
or `2608_{BD,SD,TOP,HH,TOM,RIM}.WAV` beside the DLL / Rhythm path. If neither is
found, OPNA drums are silent; File Info shows ROM / WAV / NONE.

## Build

```bash
/usr/bin/make dll
/usr/bin/make pack   # or zip dist/pack → xmp-pc98-1.0.27.zip
```
