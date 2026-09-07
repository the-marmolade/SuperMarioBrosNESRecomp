# nesrecomp-nds

A Nintendo DS runner for [nesrecomp](https://github.com/mstan/nesrecomp),
tested with [SuperMarioBrosNESRecomp](https://github.com/mstan/SuperMarioBrosNESRecomp).

Statically recompiled 6502 code running as native ARM on a Nintendo DS, with
the NES PPU mapped onto the DS's own 2D hardware rather than a software
renderer. Runs at full speed on original DS hardware.

![screenshot](docs/screenshot.png)

## Status

Playable. World 1 completed on hardware (DSpico flashcart, DSi).

| | |
|---|---|
| Binary size | 2.6 MB (ARM Thumb) |
| Frame time | 4–6 ms of a 16.7 ms budget |
| Frame rate | 60.0988 Hz, matching NTSC NES |
| Video | Hardware BG + OAM, status bar on the sub screen |
| Audio | NES pulse/triangle/noise on DS PSG channels |
| Input | D-pad, A, B, Start, Select |

Not implemented: DMC audio, 8×16 sprites, sprite-overflow flicker, save
states, mappers other than NROM.

## Building

You need [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`nds-dev` package:

```
sudo dkp-pacman -S nds-dev
```

Then, from the MSYS2 shell on Windows or a terminal elsewhere:

```
git clone https://github.com/mstan/SuperMarioBrosNESRecomp
cd SuperMarioBrosNESRecomp
./setup.sh
git clone https://github.com/the-marmolade/nesrecomp-nds runner-nds
cd runner-nds
make
```

That produces `runner-nds.nds`.

## Supplying the ROM

The build does not include a ROM and never will. Provide your own dump of a
game you own, named `smb.nes`. Two options:

**SD card** (recommended) — put `smb.nes` in the root of your flashcart's SD
card, next to the `.nds`.

**Embedded** — put `smb.nes` in `nitrofiles/` before running `make`. It gets
built into the binary, which avoids needing DLDI support. Do not redistribute
a binary built this way.

## Running

**Hardware:** copy `runner-nds.nds` to your flashcart's SD card and launch it
from your loader. Tested on DSpico with Pico Launcher. If you get
`fatInitDefault failed`, your loader isn't DLDI-patching — either patch it
yourself with `dlditool`, or use the embedded-ROM option above.

**Emulator:** melonDS works. Enable DLDI under Config → Emu settings and point
it at a folder containing `smb.nes`.

DeSmuME does **not** work — it predates the calico-based libnds and shows a
white screen. This is an emulator limitation, not a bug in this project.

## Controls

| DS | NES |
|---|---|
| D-pad | D-pad |
| A | A (jump) |
| B | B (run) |
| Start / Select | Start / Select |
| **L** | toggle fps / debug overlay |

## How it works

The NES PPU maps onto DS hardware more directly than you might expect:

- **Backgrounds.** NES nametables are written straight into a 512×256 DS text
  background, which is exactly two nametables side by side — the right shape
  for SMB's vertical mirroring. CHR tiles are converted from 2bpp planar to
  4bpp linear once at load.
- **Sprites.** The NES's 64 sprites map 1:1 onto DS OAM, which holds 128.
- **The 240 vs 192 line problem.** Rather than scaling, the playfield (NES
  lines 32–223) goes on the top screen unscaled and pixel-perfect, and the
  status bar goes on the bottom screen. No filtering, no dropped scanlines.
- **Audio.** NES pulse duty cycles land almost 1:1 on DS PSG channels; the
  triangle plays a 32-step wavetable as a looping sample, which is what the
  NES triangle physically is.

Everything expensive happens outside vblank into RAM shadow buffers, which are
then DMA'd to VRAM during vblank. Building directly into VRAM overruns the
~1.3 ms window and tears.

The DS panel refreshes at 59.8261 Hz against the NES's 60.0988 Hz, so one extra
NES frame is emulated every ~219 frames to keep game time accurate.

## Notes for anyone extending this

Two things the recompiler expects that are easy to miss:

- The runner must push the 6502 NMI hardware frame (PC high, PC low, status)
  before calling `func_NMI()`, because the generated handler ends in `RTI`
  which pops all three. Without it the stack pointer climbs 3 bytes per frame
  until it overwrites something that matters.
- `nes_interp_dispatch` is stubbed here. The recompiler normally inlines
  jump tables at call sites, but anything reaching `JumpEngine` indirectly
  falls through to the interpreter, which this build does not have.

`SPRITE0_CYC` in `nes_runtime_nds.c` approximates sprite 0 hit from position
within the frame rather than modelling the PPU dot clock. It works for SMB.

## Licence

This runner is MIT licensed. It contains no Nintendo code or assets. You must
supply your own ROM.
