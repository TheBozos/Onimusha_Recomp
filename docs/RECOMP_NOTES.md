# Recompilation notes: Onimusha: Warlords (NTSC-U, SLUS-20018)

How the port is brought up, every fix and how it was found, and what remains open. Built on
[BozoEngine](https://github.com/TheBozos/BozoEngine) (submodule `BozoEngine/`), on Dark Cloud's model: the closest
port (same CodeWarrior toolchain, symbols kept, code overlays, Sony's modular synthesizer).

## The disc (SLUS-20018, version 1.00)

- `SYSTEM.CNF`: `BOOT2 = cdrom0:\SLUS_200.18;1`, `VER = 1.00`, `VMODE = NTSC`. `SLUS_200.18` SHA-1
  `21166f0c7ec1ee2e6dfb87e81af242efaf521dc7` (checked by tools/oni_prepare.py).
- **The ELF is not stripped**: a CodeWarrior link (`MW MIPS C Compiler (2.3.1.01)`, `.mwcats`) that kept its symbol
  table (6,354 FUNC symbols with sizes, 4,210 objects) and its DWARF (`.debug`, `.line`). No symbol-map phase.
- `main` at `0x100000`: EE code `0x100000-0x1e9e00` (crt0 `_start` `0x100008`), VU microcode (`.vutext`)
  `0x1e9e00-0x1eeb40` (VU1 `Vu1mc_*`, VU0 `_$VU0_POLYCALC`, `_$VU0_B_CLIP`, `_$VU0_UN_CLIP`, `_$VU0_VOLUMECLIP`,
  `_$VU0_POLYDETECT`, `shadow_vu1`), data after, `.vudata` `0x20e700`, `_fbss` `0x214380`, `_gp` `0x21b170`,
  stack `0x1ff8000`. No CodeWarrior static initializers after the data.
- A developers' `test` section at `0x3800000` (past the 32 MB): its program header is dropped in the prepared ELF.
- **Code overlays** (`MWo3` modules, the header DC's TITLE/DUN use: +8 load, +0xC text, +0x10 data, +0x14 BSS, +0x20 the
  file name; loaded with the header, code from +0x80). Their ELF sections (`em000`..`em181`, `sb000/sb001`,
  `room000`..`room530`) are empty; the code is inside the disc's files:

  | slot | address | modules | where |
  |---|---|---|---|
  | enemy 0 | `0x4ba780` | `emXX0.bin` (18) | `IMAGE/LMR/EMMR_*.LMR`, first module of `LMR_*.LMR` |
  | enemy 1 | `0x4c9500` | `emXX1.bin` (12) | second module of `LMR_*.LMR` (two enemy types in a room) |
  | room | `0x4d8280` | `roomNNN.bin` (67) | `IMAGE/RDT/ROOMNNN.RDT` |
  | em090 | `0x2bf200` | `em090.bin` | `EMMR_090.LMR`: linked at another address than its ELF section; only 5 of 56 ELF symbols agree (another build), the rest found from the code |

  LMR files are `LINK` containers: a count, then (type, offset, size) entries: 3 `MDR` (models), 2 `OIM` (textures),
  14, 4 = the `MWo3` code. 98 distinct modules (by content); every header (first 64 bytes) is unique, so it is each
  module's identity. The ELF's overlay symbols agree with the disc's modules (checked: each function's last
  instructions are its return), except em090.
- IOP: `IOPRP202.IMG`, then SIO2MAN, PADMAN, MCMAN, MCSERV, LIBSD, MODHSYN, MODMIDI, MODMSIN, SDRDRV (paths at
  `0x211750`), and Capcom's `SNDDRVCD.IRX` ("Capcom Sound Driver for PS2(CD/DVD) by T,Masuda ver 1.75", 00/12/14),
  loaded by `Snd_module_set` `0x190720` with the arguments `"0x1FFFFF"`, `" 0x01FFFF"`. The HD/HL/CL variants and the
  network modules (INET, ILINK, AN986, USBD) are on the disc, not loaded.
- Media: backgrounds are IPU streams (`IMAGE/ETC/*.IPU`, `IMAGE/BGS/MOV*.IPU` animated backgrounds), movies are PSS
  (`IMAGE/MOVIE/*.PSS`), music `IMAGE/BGM/*.BGM`, sound effects `IMAGE/SE/*.SND`, voices `IMAGE/VOICE/ROOMnnnE|J`.

## Build

- `tools/oni_prepare.py` (run by CMake at configure time) writes `build/bozo/oni_prep/`: `oni.elf` (the main section
  described as its EE code only, the `test` segment dropped), one ELF per overlay module (tools/wrap_elf.py, from
  VP2S/DC) with its ELF section's FUNC symbols, its identity, and the module list.
- `tools/oni_overrides_list.py` lists `registerFunction(0x...)` overrides and `case 0x...u: // native site` lines of
  `runtime/`; `bozo-externals` adds the library and kernel stubs from the ELF's symbols. The overlays are generated
  with the same list minus the native sites (`tools/oni_overlay_externals.py`).
- `bozo-recomp` translates `oni.elf` into 32 files and each module into one (`build/bozo/oni_gen/`, prefix = module
  name, `--symbol-prefix`). Note: bozo-recomp deletes any other `<prefix>_*` file in its output folder, so nothing else
  lives in `oni_gen/`.
- `tools/oni_modules_gen.py` registers every module with `FunctionTable::addModule(slot start, slot end, ram + slot,
  identity, tables)`: a module's functions are callable only while its header is resident.
- `tools/oni_bozo_audio.py` relocates and translates the six sound modules (BozoEngine `tools/native_irx`): LIBSD
  `0x45a00`, SDRDRV `0x4a100`, MODMIDI `0x4b800`, MODHSYN `0x4f100`, MODMSIN `0x58500`, SNDDRVCD `0x58d00`
  (`0x39800` bytes); the native library heap starts at `0x93000`.
- `scripts/build_runtime.sh` (cmake + ninja, ~1 min from clean), `scripts/run.sh --frames N [--profile-io]`.
- `tools/oni_disasm.py <ELF> <name | 0xADDR> [bytes]`: R5900 disassembly with the ELF's symbols (capstone; lq/sq and
  MMI decoded by hand, MMI shown raw).

## Boot, step by step (2026-11-23)

Each was a stop of the headless runner.

1. `the ELF's image does not fit in guest RAM`: the `test` section at `0x3800000`. Dropped from the prepared ELF; the
   runner loads `oni_prep/oni.elf` (the CD library reads the ISO, not the ELF's folder).
2. `CpEEInit_RpcClient` `0x193c00` spins in `CpEEWait` (a call-free delay loop) on `sceSifBindRpc` to Capcom's driver,
   servers `0x77777777` and `0x77777778`. Fixes, in order:
   - the sound modules statically compiled and started natively (runtime/oni_audio.cpp, as DC);
   - `FreeOptions::advanceLibrariesWhileStalled`: no frame ends before the bind, so the IOP tasks must advance anyway;
   - SNDDRVCD's start (`0x58fd0`) returns 1 (not resident) unless `argc == 3`: `moduleLoaderWithArguments` passes
     `sceSifLoadModule`'s arguments to `startImage`;
   - its task then binds the **EE** service `0x77777779` (polling sifcmd 15): the game serves it from EE thread 2
     (`_CpEEComRecieveThread` `0x194380`, `sceSifRegisterRpc` + `sceSifRpcLoop`, handler `_CpDivideEECom` `0x1944b0`).
     BozoEngine answers IOP-to-EE RPC through `Libraries::sif.eeServers`: runtime/oni_audio.cpp serves `0x77777779`
     natively. Command `0x1xxxx`: send EE memory to the driver (`CpSifDmaTransEEToIOP` `0x1940e0`: words 2-6 of the
     request = EE source, IOP destination, size, attribute, mode); reply the transfer id (1), -1 for size >= 0xffff1.
3. `sceIpuInit` `0x10aeb0` writes the IPU's registers (from `Bg_disp_init`): a no-op override.
4. main's loop (`main` `0x12dd70`) polls the GS CSR itself: `ld` at `0x12ddb8`, `sd` at `0x12ddc4` (clear VSINT),
   FIELD at `0x12e0bc`, the vsync wait on VSINT at `0x12e0dc`. Native sites answer them (VSINT always set); the frame
   boundary is `sceGsSwapDBuffDc` `0x101868` (overridden: `port::frameEnd`), called at the top of every frame.
5. `Quake_move` `0x12fb00` writes DISPLAY1/DISPLAY2 (`0x12fba8`, `0x12fc20`; screen shake from the halfwords at
   `0x287350/52`): native sites keep the words for the renderer.
6. Frame 16: `getFlags` `0x123790` writes `IPU_CMD`: the first background decode (below, "Backgrounds").
7. Frame 508, in the title: syscall 120, `sceSifSetDChain` from `Snd_SdTransToIOP` / `Snd_SdTransToIOPBD` (sound
   data to the driver in 0x5000-byte chunks before their native `sceSifSetDma`): an override returning 0.
8. Pressing START: SNDDRVCD imports `sysclib` 13 (`memmove`) and 32 (`strrchr`), which BozoEngine's IRX host lacked.
9. Idle on the title (~2,540 frames): an RPC handler of SNDDRVCD (`0x59a94`) releases a voice and polls its
   `VOLXL`/`VOLXR` (`sceSdGetParam(voice | 0x600/0x700)`, `0x5aa18`) with `DelayThread(100)` until they are 0. Native
   libsd answered the volume as set, never 0. 8 and 9 are engine fixes (BozoEngine branch
   `onimusha/irx-sysclib-volx`: `sysclib` 13/32; VOLX = volume x ENVX as the SPU2 reports it).
10. **Current stop** (idle ~2,534 frames, the attract sequence): the PSS movie player (`Movie_task` `0x1bcf60`, Sony's
    sample player: `dispCreate` `0x1bb900` and its display chain, `videoDecMain`, `vblankHandler`, `pcmCallback`,
    IPU DMA set directly by `setD3_CHCR` / `setD4_CHCR`) writes DMA channel 2's registers.

## Tools for reading the game

- `tools/oni_dwarf.py <SLUS_200.18> build/oni_types.h`: the ELF's DWARF 1 (CodeWarrior) as C: every structure (field
  offsets, bit fields), enumeration, typedef, function (parameters with their registers or stack slots) and global
  (address), compile unit by compile unit (`D:\oni\prog\title.c` ...). Anonymous types are named `@anonN` per unit:
  read a function's types inside its own unit. Game-derived: keep it in `build/`.
- `tools/oni_disasm.py`: the resident code with the symbols.

## Backgrounds (runtime/oni_bg.cpp, 2026-11-23)

The rooms, logos and menus are pre-rendered `ipum` files (`IMAGE/ETC`, `IMAGE/RDT`; 640 x 448, one frame of MPEG-2
intra macroblocks: a flags byte, then the IPU's own bitstream). `Cut_set(work, cut)` `0x123490` reads the cut's file
to `0x7f3440` (`Cd_req`), its size to `0x214380`, builds the to-IPU DMA tags, resets the IPU (`IPU_CTRL`) and jumps to
`decodeIPU` `0x1221a0` (through `divFrameBuff` `0x122190`: a `j`, which runs the guest code, so both are overridden).
`decodeIPU` reads the flags (`getFlags`: FDEC), runs IDEC with CSC to RGB32 through the from-IPU DMA one macroblock
row at a time, and splits the picture into its fields: even lines at `0xa69c80`, odd lines at `0x9ddc80` (640 x 224
RGBA each). `Bg_disp` `0x122040` sends the shown field to the GS every frame.

Natively `decodeIPU` decodes the stream with FFmpeg's IPU decoder (`scripts/build_ffmpeg.sh` builds a static FFmpeg
with only `mpeg2video` and `ipu`) and the IPU's CSC (BT.601 studio range, coefficients in 128ths; alpha 0x80) into the
same two field buffers. FFmpeg's decoder wants exactly one end code after the picture: Onimusha's files end with
`0x000001b0` then `0x000001b1`, so the packet stops after the first. `Bg_disp` still runs (its camera, lights,
shadows, the BG movie) and marks the background shown; the frame boundary publishes the picture rebuilt from the two
field buffers as the renderer's backdrop. The `IPU_CTRL` resets in `Cut_set` / `Cut_set_onmemory` are native sites,
`sceIpuInit` and `sceIpuSync` return 0.

## Textures and the 2D (runtime/oni_tex.cpp, runtime/oni_sprite.cpp, 2026-11-23)

- **Uploads.** Every image goes through libgraph's `sceGsSetDefLoadImage(lp, dbp, dbw, psm, x, y, w, h)` and
  `sceGsExecLoadImage(lp, src)` (`Mapping_oim2` / `Mapping_oim3` for the OIM textures and palettes of the `LINK`
  files, which `Cd_link_read` loads). The port answers both: each upload is kept as given (destination block, format,
  size, texels copied from EE memory), keyed by its block. An upload at (x, y) of its buffer is keyed by the block it
  starts at (the palettes: four 8 x 2 CT32 palettes per block group, at (0,0), (8,0), (0,8), (8,8)). A draw's TEX0
  finds its texels (TBP0) and palette (CBP, CSA; CSM1 order) and is decoded to RGBA once (`ONI_TEX_TRACE=1`,
  `ONI_TEX_DUMP=<dir>` writes each decoded texture as PAM).
- **The ordering table.** `Ot_draw(pOt, length)` `0x12e7f0` (main, every frame; `G` at `0x27de30` is the table,
  0x1000 slots) walks `pOt[0 .. length)`: a slot holds 0 or the first primitive of a chain; a primitive's word 0 is
  the next one or a pointer back into the table. Each primitive: +4 `Attribute`, bits 0-10 the GS PRIM value, 0x800
  GS coordinates, 0x1000 point sampling, with ABE 0x2000 `(Cs - Cd)A + Cd` / 0x4000 `(0 - Cs)A + Cd` / else
  `Cs A + Cd` (A the source's alpha with 0x8000, else FIX = the colour's A byte), bits 24-31 the layout: 0 an ALPHA
  reset, 1 a vertex, 2 two untextured vertices, 3 a textured triangle (TEX0 pointer +0x40), 4 a textured sprite (the
  0x28-byte sprite record of title.c's `sprite` / `item_sprite`, TEX0 pointer +0x24), 5 a textured quad (+0x50), 6 a 3D
  model (`Md_draw_entry`), 7 another quad layout (+0x70). Coordinates: x 0..639, y 0..447 of the frame (`Ot_draw`
  halves y: the GS draws 640 x 224 fields at XYOFFSET (1728, 1936)).
  Natively the same walk turns each primitive into a frame draw in screen space (identity matrix, no depth, table
  order over the background): the frame pass has the GS blend equation, the alpha test and point sampling (the
  overlay pass reduces them to plain blends). The draw environment's `TEST_2` (alpha test: transparent texels are not
  drawn) is not changed by `Ot_draw`: textured draws test alpha > 0, which keeps the menu's frame under its items.
- **Pit_trans** `0x1af6f0`: the sprites `Pit_prim_entry(prim, priority)` queues (the fades: `fade.c`'s black sprite
  whose alpha `fade_in00` and friends animate; pointers at `0x40d690`, count `0x214604`), priorities 0-7, the sprite
  layout, offset by the origin at `0x28e420`. Its fixed-alpha blend scales by the destination's alpha (ALPHA C = Ad).
- `ONI_2D_TRACE=1` counts the primitives per frame, `=3` prints each sprite.

Result (2026-11-23): in the window, the Capcom logo and the violence notice fade in and out, the title shows "PRESS
START BUTTON", START opens the main menu (NEW GAME, LOAD GAME, OPTION) and the pad moves its frame; 60 frames/s.

## Next

- **Movies** (the attract sequence after ~40 s on the title; New Game's opening): Sony's sample player in
  `movie.c`. BozoEngine's libmpeg decodes with FFmpeg (`setMoviePictureHook`); its display chain (`dispCreate`,
  `dispSetTags`, `loadImage`, `dispSwitch`, `startDisplay`), the IPU FIFO it feeds by DMA (`viBuf*`, `setD3_CHCR`,
  `setD4_CHCR`), the vblank handler and the PCM path (`pcmCallback`, `voBuf*`) need native answers.
- The animated backgrounds (`IMAGE/BGS/MOV*.IPU`, `DecodeIPU_Movie` `0x197bc0`, `MoviePlay` from `Bg_disp`).
- The 3D: `Md_draw_entry` and the VU1 programs (`Vu1mc_*`), `Spr_draw`, `Esp_sp_draw`, `Screen_blur_draw`, the
  background's depth (`Bgz_disp`); the VU0 programs (`_$VU0_*`) translated to C++ (TTA's tools/tta_vu0_gen.py).
- The six 640 x 64 CT32 uploads into block 0 (the frame buffer) at boot: not drawn yet, origin to find.
