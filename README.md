# xmp-pc98 1.0.20

Native **32-bit** XMPlay input plugin for NEC PC-98 music.
Display name **PC-98 / S98**. DLL `xmp-pc98.dll`.

This is **not** a wrap of S98Amp / `in_s98.dll`, `in_fmpmd.dll`, or ZXTune.
Same rule as xmp-gamemusic (“does not wrap `in_nez.dll`”).

Classic XMPlay is **32-bit only**. This DLL is PE32 i386.
VERSIONINFO FILEVERSION is **1.0.20.0**; `PLUGIN_XMPVER` is **1002000**.

## Install

Copy `xmp-pc98.dll` next to `xmplay.exe` (or into XMPlay's plugin folder)
and restart XMPlay.

**Delete or disable S98Amp, in_fmpmd, and ZXTune’s S98 / `.M` claims**
so they do not fight this plugin. ZXTune `.M` is ZX Spectrum, not PMD —
we probe PMD magic so a Spectrum `.M` is not claimed.

Keep **xmp-gamemusic** for `.vgm` / `.vgz`. Keep **xmp-pokey** for `.sap`.
Keep XMPlay’s MIDI plugin for `.mid`.

Optional next to the DLL:

- `2608_*.WAV` — OPNA rhythm samples (same set as m_s98 / PMDWin)
- `WinFMP.dll` — C60 FMP player (`.OPI` / `.OVI` / `.OZI` / WinFMP `.FMD` / `.MMD`)
- `SC-55.sf2` (or SC-55mkII) — GS playback for packed FUGA `.GMD`
- `MT32 GS 2.51.sf2` / `CM-64.sf2` — CM-32L+CM-32P playback for packed `.MMD`
- `hootrip.exe` — [hootrip](https://github.com/TheWhyteWolf/hootrip) for Hoot rehost
- Hoot XML catalogue (`hoot.xml` + gamelists) — `make fetch-xml` or
  [Hoot](http://dmpsoft.s17.xrea.com/hoot/) / [Kurohane](https://kurohane.net/hoot/)

Config is written to `xmp-pc98.ini` next to the DLL (and XMPlay’s
GetConfig / SetConfig blob).

## What it plays

| Kind | How | Notes |
|------|-----|--------|
| **S98** | cisc fmgen (OPN / OPNA) | Same core as in_s98. Mix 55466 Hz, FM 0 / SSG −18 (PC-9801) or −8 (PC-8801). Tags, loop `FD`, seek, mute |
| **PMD** `.M` `.M2` `.MS` `.MP` `.MZ` `.LSP` | pmdmini / PMDWin 0.52 + ymfm | **Probe by header**, not extension. Playlist length is first play (not the short `L` tail). Mix halved vs raw ymfm to sit with fmgen S98. Sibling `.PPC` `.P86` `.PPS` `.PVI` `.PZI` |
| **FMP** `.OPI` `.OVI` `.OZI` `.FMD` `.MMD` | optional `WinFMP.dll` | Probe headers. Playback needs the DLL in the plugin folder |
| **BGMDRV** `.MUS` | ymfm (OPNA SSG+FM) | Kinpukurin / Faladia part table + sibling `TONE.DAT`. No hootrip |
| **NA** `.O` | ymfm (OPNA) | Packen MUAP98 / dangtoys. Header `0x0026` + sibling `TONES.DTA`. Not a generic `.o` |
| **MFD** `.USO` | ymfm (OPNA) | Melody Night Slave + Idol. Sibling `{name}.MFD` patches when present |
| **N3G** `.MDT` | ymfm (OPNA) | SOUND_B / n3gv2 (n3gpbl98 and cousins). Sibling `MUSIC.SDT` later |
| **Synthia** `.PAI` | ymfm (OPNA) | `PAI 3.00M` only (not `.pai` by name, not PAI 1.20, not `ylz`). FM3+SSG3 |
| **MBMUS** `.MSB` | ymfm (OPNA) | Kirishima (`KG##.MSB` / `KG##N.MSB`). Word-obfuscated; instruments in-file. CheckFile peeks 256 bytes (part table is accepted without the later tracks) |
| **OPNDRV** `.MD` | ymfm (OPNA) | Oerstedia FM (`OERS_NNN.MD`). 3 FM + 3 SSG + 86-board rhythm (part 7, GM drum notes). Needs `2608_*.WAV` next to the DLL |
| **FMD pack** `.GMD` `.MMD` | TinySoundFont + SF2 | FUGA XOR-A5 RCP ([Valley Bell](https://github.com/ValleyBell/MidiConverters)). `.GMD` = SC-55 GS; `.MMD` = CM-64 (CM-32L+CM-32P). Applies GS/MT reset, master volume, and reverb from RCP `DD`/`DE`/`DF`. Auto-finds `SC-55.sf2` / `MT32 GS 2.51.sf2` under XMPlay `midi soundfonts`. Config `gs_sf2` / `mt_sf2`. Not `.mid` |
| **NTL** `.NTL` | ymfm (OPNA) / TSF | Sekigahara `F_*` is FM/SSG (default YM voice; bank is in `SEK2.EXE`). `G_*` is GM/MIDI via SC-55 SF2 (same folder as Oerstedia). CheckFile peeks 256 bytes |
| **MsDRV** `.MS` | ymfm (OPN/OPNA/OPL3) / TSF | KENJI MSDRV4 light. `_N`=OPN, `_B2`=OPNA, `_GS`/`_88`=GS (SC-55 SF2; `_88` is SC-88 arrangement, not identical to `_GS`), `_SB`=OPL3 via ymfm ymf262 + `.OPN` type=1 voices. Probe by header so PMD `.MS` stays PMD. |
| **`set.pc98`** | hootrip → cached `.s98` | Multi-track (`GetSubSongs`, Shift+arrows). Skip `演奏停止` / `[STOP]`. Native sidecars play without hootrip |

A extracted [Hoot](http://dmpsoft.s17.xrea.com/hoot/) tree
(`letter\game_98`, e.g. `A\akasui_98`, `M\mime_98`) has thousands of
PMD `.M` files and almost no `.s98`. Those `.M` files play on day one.
Custom drivers (MAKO `AMUS.DAT`, `*_hoot.com`) still need Hoot rehost.

Local census of `D:\spel\pc98\pc98 music` (no rip): **1,712** game
folders; **209** with PMD-like files, **55** FMP-like, **1,448** other
(Hoot rehost / MAKO / custom). File-level PMD coverage is much higher
than 209 folders — those folders hold most of the ~5,500 `.M` files.

## What we never claim

- `.vgm` / `.vgz` — xmp-gamemusic
- `.sap` — xmp-pokey
- `.mid` — XMPlay MIDI (SC-55 sets stay there)
- `.mml` source, `.com` / `.exe` / `.sys` / `.tdf` / `.ins`

## Lengths and loops

Playlist time is **measured one loop**. Extra loops still play (config
1 / 2 / 3). GetFileInfo returns a FACE 4 seconds array
(`XMPIN_INFO_NOSUBTAGS`). We never ship a dummy **3:00**.

Hoot cache miss: GetFileInfo returns **1 second** (not 180) so adding
the library does not block on a multi-minute rip. First **Open** may
spawn `hootrip.exe` and write `cache\<set>\NN.s98`.

## Hoot archive

XMPlay cannot open a directory. Use the companion tool:

```
pc98-scan --root "D:\spel\pc98\pc98 music" --xml "C:\path\to\HootArchive"
```

That walks `letter\game_98`, matches folder names to the XML catalogue,
and writes a tiny `set.pc98` in each game folder. Adding the music tree
then picks up `.pc98` / `.s98` / PMD / FMP — not every `MAKO.COM`.

```
pc98-scan --root ... --xml ... --rip
```

runs `hootrip archive-rip` for a batch cache (optional; slow).

Config paths: Hoot XML, music root (default `D:\spel\pc98\pc98 music`),
S98 cache, `hootrip.exe`.

## Build

Needs `i686-w64-mingw32-g++` for the DLL. Host tests use the native
`g++` / `gcc`.

```
make          # tests + dll
make test
make dll
make scan
make pack     # dist/xmp-pc98-1.0.20.zip
make fetch-xml
```

Vendored: [gzaffin/pmdmini](https://github.com/gzaffin/pmdmini) (PMDWin
0.52 + ymfm) and cisc **fmgen** (S98 only; see `third_party/fmgen/readme.txt`).
Official `third_party/ymfm` is **not** linked (same `namespace ymfm` —
duplicate symbols). PMD / BGMDRV / NA / MFD / N3G / Synthia / MBMUS / OPNDRV / NTL use pmdmini’s ymfm.
Packed `.GMD`/`.MMD` use [TinySoundFont](https://github.com/schellingb/TinySoundFont) (MIT). Roland SF2 files are **user-supplied** and are not shipped.
Other S98 device types keep time and stay silent.

Do not vendor or commit the music folder. Rhythm WAVs and Hoot XML are
user-supplied or fetch-at-pack.

## Credits

- MsDRV 4 — KENJI (KAKERA); light sequence notes — Valley Bell

- XMPlay SDK — un4seen
- S98 spec — Mamiya / Ru^3
- ymfm — Aaron Giles (BSD-3)
- PMD — Kajihara; PMDWin — C60
- FMP / WinFMP — C60 / Guu
- Hoot — DMP Soft; XML — Hoot + Kurohane
- rehost — [hootrip](https://github.com/TheWhyteWolf/hootrip) (TheWhyteWolf, MIT)
- fmgen — cisc (custom freeware license; ship `fmgen/readme.txt`)
- Synthia 3.00 — Y. Yamada / STUDIO よしくん (`.PAI`)
- MBMUSic 1.03 — Kirishima `.MSB`
- OPNDRV / FMD — FUGA System (Oerstedia `.MD` / packed `.GMD` `.MMD`)
- TinySoundFont — Bernhard Schelling (MIT)
- fuga2rcp / RCP notes — Valley Bell
- ArtDink — Sekigahara `.NTL`
- House style — xmp-pokey / xmp-deepsid / xmp-gamemusic

## License

GPLv2 or later (pmdmini / PMDWin). ymfm is BSD-3. fmgen is cisc’s
custom freeware license (credit + unmodified readme; not GPL).
See `LICENSE`, `third_party/pmdmini/LICENSE.md`, and
`third_party/fmgen/readme.txt`.
