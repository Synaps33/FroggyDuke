# FroggyDuke

**EDuke32 (Duke Nukem 3D)** ported to the **Data Frog SF2000** and **GB300** handheld game consoles (MIPS32 XBurst, no FPU, bare-metal multicore libretro).

---

## Features & Highlights

- **Bare-metal MIPS32 soft-float execution:** Tailored for the Ingenic JZ4775 SoC running without an FPU or hardware threads.
- **Classic Software Renderer:** Running at native 320x200 / 320x240 with low overhead and full palette support.
- **Direct Hardware Input:** Bypasses stock firmware emulator mappings to provide responsive, accurate pad controls on SF2000 and GB300 V2.
- **Coroutines (libco):** Seamless cooperatively-scheduled main loop integration with the libretro frontend.
- **MIPS Unaligned Memory Fixes:** Safe byte-level memory operations preventing hardware exception 4 (`AdEL`) crashes during sound bank and MIDI loading.

---

## Controls

| Button | In Game Action | In Menus |
|---|---|---|
| **D-Pad Up / Down** | Walk Forward / Walk Backward | Menu Navigation (Up / Down) |
| **D-Pad Left / Right** | Turn Left / Turn Right | Adjust Sliders / Navigate |
| **L (Left Shoulder)** | **Fire Weapon** | — |
| **R (Right Shoulder)** | **Strafe Modifier** (hold to strafe with Left/Right) | — |
| **A** | **Open / Use** (Doors, switches) | **Select / Enter** |
| **B** | **Jump** | **Back / Escape** |
| **X** | **Crouch** | — |
| **Y** | **Next Weapon** | — |
| **START** | **Pause / Main Menu** | Exit Menu |
| **SELECT** | **Toggle Map** | — |

---

## Installation

### Requirements
- A micro SD card prepared for SF2000 or GB300 (using FrogUI or Multicore).
- Duke Nukem 3D game data: `DUKE3D.GRP` (Atomic Edition v1.5 or Shareware v1.3d) and `DUKE.RTS`.

### Steps
1. Download the release archive `FroggyDuke-v1.0.0.zip` from the [Releases](https://github.com/Synaps33/FroggyDuke/releases) page.
2. Extract the package contents onto the root of your SD card:
   - For **GB300 V2**: copy `cores/gb300v2/eduke32.sf2k` to `/system/Deimos/cores/eduke32.sf2k`
   - For **SF2000**: copy `cores/sf2000/eduke32.sf2k` to `/system/Deimos/cores/eduke32.sf2k`
   - Copy `autoexec.cfg` to `/ROMS/eduke32/autoexec.cfg`
   - Place your `DUKE3D.GRP` and `DUKE.RTS` into `/ROMS/eduke32/`
   - Ensure `/configs/custom_cores/eduke32/games.opt` has:
     ```text
     DUKE3D.GRP|eduke32
     DUKE.RTS|eduke32
     ```
3. Insert the micro SD card into your console and launch Duke Nukem 3D from your game list!

---

## Building from Source

### Prerequisites
- MIPS GCC toolchain for MIPS32 little-endian (`mipsel-mti-elf-gcc` / `frog-toolchain`).
- `sf2000_multicore` tree.

### Compilation
```bash
# Build core for GB300 V2
make -f Makefile.sf2000 gb300-core -j$(nproc)

# Build core for SF2000
make -f Makefile.sf2000 sf2000-core -j$(nproc)
```

---

## Credits & License

- Based on **EDuke32** by Voidpoint and the Duke3D community (GPL-2.0).
- Duke Nukem 3D copyright (C) 1996, 2003 3D Realms Entertainment.
- Data Frog SF2000 / GB300 multicore and toolchain contributors.
