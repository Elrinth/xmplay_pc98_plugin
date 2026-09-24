# xmp-pc98 1.0.26

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

VERSIONINFO **1.0.26.0**; `PLUGIN_XMPVER` **1002600**.

## 1.0.26

- **Plugin File Info** (right-click → Plugin file info) now shows the same MsDRV
  panel as Config channel mutes: Chip / Driver / Variant / SF2 plus live per-channel
  mute checkboxes (FM, SSG, rhythm BD–RIM, OPL3 1–18, MIDI 1–16). Uses
  `XMPIN_FLAG_OPTIONS` like xmp-gamemusic. Config → Channel mutes opens the same panel.
- **EC_10_B2 poppy/silent drums (WAV path)**:
  - ADPCM-A TL polarity matched ymfm (`atten = (vol^0x1F)+(tl^0x3F)`). Songs leave
    `0x11=0x3F` (loud after XOR); the old `(0x3F-tl)` gain made WAV drums silent.
  - Fixed `44100<<16` signed overflow → `step` was 0 so voices never advanced (attack
    fade kept output at 0).
  - Soft release (~8 ms) when a rhythm bit clears; short attack + end fades so WAV
    tails/retriggers don't click. Play at true WAV rate. Drums-only peak ~6k,
    jumps>8k = 0 (was silent / poppy).
- **EC_10_N missing opening lead**: SSG was ~3 octaves low (`period = clk/(16*f)` and
  no +12). Now `clk/(64*f)` plus MsDRV→SSG +12 octave so opening periods match
  Unicorn (189/238/119). CH3 special lead later in the song was already OK in 1.0.25.
- **EC_10_SB drums**: MSDRV4L writes `0xBD` once as **0** (OPL rhythm mode off). Kept.
  Melodic/perc on normal OPL channels; A+B match reported with duplicate-write collapse.

## 1.0.25

- CH3 special AA op masks; ADPCM-A key-on bit7 clear; OPL fine PB + held FNUM refresh.

## Install

Copy `xmp-pc98.dll` next to `xmplay.exe`. Place `2608_*.WAV` beside the DLL (or set Rhythm path).

## Build

```bash
/usr/bin/make dll
/usr/bin/make pack   # or zip dist/pack → xmp-pc98-1.0.26.zip
```
