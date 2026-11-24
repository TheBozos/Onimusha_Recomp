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
10. Idle ~2,534 frames, the attract sequence: the PSS movie player writes DMA channel 2's registers (below, "Movies").
11. Skipping a movie: `termAll` loops until `DeleteThread` returns the decoder's id (the PS2 kernel's result; BozoEngine
    returns 0): a native site at `0x1bd7a0`. Then SNDDRVCD sweeps the movie's PCM voice down (volume `0xe010`) and
    waits for VOLX to reach 0: BozoEngine#125 (VOLX follows the volume's sweep).
12. New Game, both movies skipped, frame ~2,470: the first room's 3D, `Md_tag_trans` `0x130e40` starting VIF1's DMA.
    The models' packets are queued (`Md_draw_entry` appends at `G + 0x5bdc`, `Md_tag_trans_cb` the DMA end handler
    moves `G + 0x5be0` on): runtime/oni_model.cpp completes the queue at once (nothing drawn yet).
13. Frame 2,471: `pCalcLoadGmMatrix` `0x1af490` sends the POLYCALC VU0 program by VIF0 DMA (`D0_TADR = 0x1ee990`, an
    MPG of 57 pairs at VU0 address 0). The only VU0 micro-program: `pCalcPolygon` (collision) starts it with VCALLMS 0.
    tools/oni_vu0_gen.py (TLOTR_TTA_Recomp's translator, with CLIP and FCAND) makes it C++ (`Registry::vu0Program`);
    the four DMA accesses are native sites.
14. `Md_draw_semi` / `Md_draw_semi_l` send the semi-transparent models' packets by GIF DMA: 14 native sites (not
    sent; the models are to be drawn natively).
15. The room's cuts: `Cut_change` `0x122720` puts a cut block at `0x7f3440` (the room's `IMAGE/BGS/ROOMnnn.BGS`): a
    run-length Z buffer (`Zbuff_extend_448`), then the IPU stream with its own 16-byte header. `decodeIPU` now takes
    the stream where the to-IPU chain (`0x90b440`) points.
16. Black room: the opening event's script (`Sce_scheduler_main` `0x134830`: 10 tasks at `0x28e860`, bytecode,
    handlers at `0x1f4060`) waits in `Wsleeping` until the CD manager (`cd_work` `0x3dc5c0`, `Cd_task` `0x196510`)
    is idle; `Cd_task` waits for the sound driver's voice stream (ADS) to end (`Snd_ads_ck`: `gEEStat.stat_1`
    `0x3dbb20`, written by SNDDRVCD by SIF DMA). SNDDRVCD streams through the SPU2 interrupt
    (`sceSdSetSpu2IntrHandler`, IRQA), which BozoEngine did not have: BozoEngine#126 (the SPU2 interrupt; IOP
    `sceSifCheckStatRpc`, `sceSifSetDChain`).
17. **Current stop** (frame ~3,900, the first room in play): nothing faults; the characters and enemies are not drawn
    (their VU1 models), the room's animated backgrounds (`DecodeIPU_Movie`) not yet answered.

## Tools for reading the game

- `tools/oni_dwarf.py <SLUS_200.18> build/oni_types.h`: the ELF's DWARF 1 (CodeWarrior) as C: every structure (field
  offsets, bit fields), enumeration, typedef, function (parameters with their registers or stack slots) and global
  (address), compile unit by compile unit (`D:\oni\prog\title.c` ...). Anonymous types are named `@anonN` per unit:
  read a function's types inside its own unit. Game-derived: keep it in `build/`.
- `tools/oni_disasm.py`: the resident code with the symbols.
- `ONI_SCE_TRACE=<n>`: every n frames, the event scripts' tasks (pc, opcode, handler), `cd_work` and the ADS state;
  `ONI_THREADS_AT=<frame>`: the threads; `ONI_BG_DUMP=<dir>`: every cut stream and decoded picture.

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

## Movies (runtime/oni_movie.cpp, 2026-11-23)

`movie.c` is Sony's ezmpeg sample player, as Twisted Metal: Black's (TMB_Recomp's `tmb_2d.cpp` was the model), with
captions added. `Movie_task_exe(no)` `0x1bd9b0` starts `Movie_task` `0x1bcf60` on its own thread and puts the main
thread to sleep until the movie ends. `readMpeg` streams the PSS (`strFileRead`), demuxes it with
`sceMpegDemuxPssRing`, sends the PCM to the sound driver (`audioDecSendToIOP`) and starts the display. `videoDecMain`
/ `decBs0` decode into the picture buffer `voBuf` `0x460ec0` (`sceMpegGetPicture`, `dispSetTags`). `vblankHandler`
`0x1bca40` shows each picture for two fields through `loadImage` `0x1bca10`, a GIF DMA whose end interrupt
(`handler_endimage`) frees the buffer.

Native:
- libmpeg is `{referenceLayout = false, paced = true, sdkNodata = false}`, and its picture hook keeps the last picture.
- `videoCallback` `0x1bfc20` returns 1: the decoder takes the video from the demuxed stream (BozoEngine answers TMB's
  the same way), so the IPU FIFO and its DMA (`viBuf*`, `setD3/D4_CHCR`) are never used.
- `dispCreate` keeps the frame size; `dispClear` does nothing; `dispDelete` (the task's end) gives the frame back to
  the game.
- `loadImage` completes at once.
- `vblankHandler` is native: it follows the tags (even field 2 → 1, odd field 1 → 0, the buffer freed) and publishes
  the frame with the picture as the backdrop. While the display is off it runs the fade countdown `readMpeg` waits for
  (`gp - 0x6ae8` type, `- 0x6afc` timer, `- 0x6b00` level), shows the last picture darkened by it, and ends the game
  frame (the main thread sleeps through the movie).
- The picture and its darkening are the frame's last draws (`drawOver`, from `publish`), and only on the frames
  `vblankHandler` publishes: once `termAll` removes the handler the game's frames are its own again (the fade level
  stays black after a movie; drawn on every frame, it had covered the room after stage 2 room 1's movie).
- `startDisplay` `0x1bcee0` is native: the game's spins on `sceGsSyncV(0)` until another field than its argument, but
  BozoEngine's `sceGsSyncV` answers the parity of the frames it ended itself, and during a movie the frames end in
  `vblankHandler`: the answer never changed, so a movie that started on the wrong parity never showed (stage 5 room 0's
  `LINKS01`, held on five buffered pictures). The display goes on at once.
- Some rooms play a movie from their script (SCE opcode `0x72` `Movie_play`, `72 nn`: `LINKSnn.PSS`): stage 2 room 1
  (`LINKS05`, which opens on 18 s of black in the file itself) and stage 5 room 0 (`LINKS01`, then its encounter).
  Stage 2 room 1 has no door and no fade-in after its movie: reached by the warp, out of the story's order, it stays
  black after it.
- `switchThread` `0x1bd4c0` (`RotateThreadReadyQueue(1)`, which the player's threads spin on while they wait for each
  other) costs 0.1 ms of EE time per pass: tens of thousands of passes a field dropped the window to 54 frames/s,
  steady 60 now.
- Captions (`Movie_cap_ptr`, the `caption*` tables) are not drawn yet; the US movies carry their English text in the
  picture.

Result (2026-11-23): the attract movie (the opening) and New Game's intro and letter play in the window with their
sound, 60 frames/s; START skips them; New Game reaches the first room's loading.

## Models (tools/oni_vu1_gen.py, runtime/oni_vu1.cpp, 2026-11-23)

The game draws every model through VU1. `Md_draw_vu0` / `Md_draw_entry_*` build a VIF1 source chain per object (the
model file's prebuilt chains linked, matrices and lights from `Md_first_trans_set`'s REF of 17 qwords at `0x27dc00`)
and queue it; `Md_tag_trans` `0x130e40` starts VIF1's DMA on it (chain mode, TTE: the tags' upper halves are VIF
codes). Each object's chain sends its program with a REF to one of the `Vu1mc_*` images (`Md_obj_vu1_type_set`, the
table at `0x1f3c80`): a VIF NOP, an MPG of the program to micro address 0, MSCAL 0 (the set-up: constants and the
matrices at VU address 0xb0..0xbf), then per batch of vertices an UNPACK and MSCNT (the main loop after the set-up's
E bit). The programs build GIF packets (PACKED: ST, RGBAQ, XYZF2 per vertex, ADC in XYZF2 for strips) and XGKICK them.
A vertex is 5 qwords: position, normal, UV (12.4, ITOF4), the joint matrices' addresses and the skinning weights
(x 1/248).

Natively, nothing of VIF1, VU1 or the GS is emulated as hardware; the game's own programs run as C++:
- `tools/oni_vu1_gen.py` translates the 15 images (14 `Vu1mc_*` and the shadows' program at `0x20e878`, two MPGs of
  256 + 201 pairs) with the VU0 translator's pair model (lower reads before the upper writes, Q's latency, the MAC
  pipeline), adding VU1's memory instructions (LQ / LQI / LQD / SQ / SQI / SQD / ILW / ILWR / ISW / ISWR), XTOP,
  XGKICK, the clip-flag tests, the signed branches, BAL / JR / JALR. Each program is straight-line C++ (a label per
  pair, jumps only at branches after their delay slot, Q checked where it is read); the programs that never read the
  MAC flags (all but one) use flag-free SSE arithmetic held to the PS2's range. A program is known at run time by the
  hash of the words its MPGs uploaded.
- `runtime/oni_vu1.cpp` walks the chain as the DMA does (refe / cnt / next / ref / refs / call / ret / end, the
  scratchpad bit), keeps each word's place on the qword grid (DIRECT's data starts on a qword), runs VIF1's commands
  (STCYCL, BASE / OFFSET and the double buffer swapped at each MSCAL / MSCNT, ITOP, STMOD, STMASK, STROW / STCOL,
  UNPACK in every format with masks and modes, MPG, DIRECT) on a VU1 state kept between chains, and turns each GIF
  packet (PACKED, REGLIST, IMAGE) into the frame's draws: triangles, strips, fans and sprites in screen space, the GS's
  Z, Q for the texture's perspective, the material from PRIM, TEX0, TEX1, CLAMP, ALPHA, TEST, ZBUF, FRAME and FOGCOL;
  the field's 224 lines doubled to the frame's 448, the scissor at the frame.
- Shadows: the shadow program clears an off-screen CT24 buffer (FRAME page 0xd2) to grey 0x80, draws the shadow
  volumes against the scene's depth (ZTST GREATER, no Z write) adding 0x14 for the front faces and subtracting it for
  the back faces (a stencil count in the colour), grades it to 0x14 where shadowed and 0 elsewhere with two
  full-screen sprites, then a last sprite subtracts it, read back as TEX0 block 0x1a40 scaled by 0x64 / 0x80, from the
  frame. Natively the buffer is a renderer auxiliary image sharing the frame's depth (`Material::target`,
  `frameDepth`), the last pass samples it (`fromFrame`, `source`); untextured colours are the GS's bytes
  (`directColour`); a blend on the CT24 frame leaves its alpha (the frame's alpha covers nothing on the GS; here it
  is what composites the 3D, and the darkening pass writing it made the models translucent). Any other off-screen
  target is not drawn. Drawn literally on the frame, those passes had blackened the models.

Cost: the first room runs ~1000 program calls a frame (1.44 million VU pairs, the PS2's VU1 busy ~9.6 ms of its
16.7). Pair by pair through a `switch` with exact flags it took ~22 ms a frame: the room fell to 37-50 frames/s and
the sound underran (heard as crackling). Straight-line code and the flag-free arithmetic bring it to ~5 ms (~1 ms of
it the GIF packets): 60 frames/s and no underrun. `ONI_AUDIO_STATS=1` prints the sound device's queue, underruns and
the frame rate; `ONI_VU1_TRACE=1..5` the frame's counts and times, then runs, kicks, DMA tags, VIF codes and GIF tags;
`ONI_VU1=0` turns the models off; `ONI_VU1_FLAT` / `ONI_VU1_NOTEX` / `ONI_VU1_NODEPTH` are diagnostics.

Save states (runtime/oni_state.cpp): the port's section `oni` adds what the engine's own sections do not hold: the
compiled sound modules' IOP host (`CompiledLibraries::serialize`: its memory, tasks, kernel objects, mixer), the
texture uploads (GS memory as the game filled it), VIF1 / VU1 (registers, data and micro memory, the resident program
by its index) and the GS registers the packets set; the room's picture and depth are rebuilt from EE memory after a
load. Without it a loaded state drew untextured models and faulted in `sceSifCallRpc` 11 frames later (the sound
driver's IOP side was the boot's). A movie being shown makes the state unsaveable. Checked: a state saved at frame 3600
of the first room and loaded runs to 3900 with the same VU1 work frame for frame as the run from the boot.

## Room depth (runtime/oni_bg.cpp, 2026-11-23)

`Bgz_disp` `0x123800` runs at the end of each frame (after the 2D, before `sceGsSyncPath`) and sends a prebuilt GIF
chain: BITBLTBUF to block 0x1180 (the ZBUF, page 0x8c), 640 wide, PSMZ24, then an IMAGE of 640 x 224 24-bit Z, one
field (`0x21c050`: the even lines, data at `0x90bc80`; `0x21bd30`: the odd lines, `0x974c80`; chosen by bit 0 of
`work+0x407c` as `Bg_disp` chooses the picture's field). The next frame's models then test against it.

Natively `Bgz_disp` keeps both fields as one 640 x 448 image (Z in the texels' R, G, B) and the next frame starts with
it as a depth-only draw (BozoEngine's `Material::depthFromTexture`, TheBozos/BozoEngine#128). `Bg_disp` then draws the
room's picture into the frame (no depth test or write) instead of a backdrop under it, so the shadows' darkening and
any other frame effect apply to it as on the GS. Checked by filling the image with the nearest Z: every model hides.
`ONI_BGZ=0` leaves the depth out, `ONI_SHADOWS=0` the shadows.

## Animated backgrounds (runtime/oni_bg.cpp, 2026-11-23)

A cut whose table entry (`work+0x489c`, 16 bytes a cut: bits 28-30 of word 2) has the movie flag sets `work+0x4a4d`;
`Cut_change` then calls `Movie_init`, which finds the cut's movie among those the CD manager loaded with the room
(`Get_bgmv_adrs`, `cd_work`: four for room 0), and every frame `Bg_disp` calls `MoviePlay`: `movie_ctrl_main` says when
a frame is due, `setToIPUtags` points the to-IPU chain (`0x7f2c00`, REF tags, a REFE last) at the frame's data, and
`DecodeIPU_Movie(params, entry, dest)` `0x197b80` decodes it (IPU_CTRL from params, SETTH from `entry+0x3a`, IDEC to
RGB32: `entry+0x18` x `entry+0x1a` macroblocks of 1 KB one after the other at dest). The module's own `loadImage`
`0x197f10` then sends one of the two GIF chains `Movie_init` built (`0xaf5c80`, `+0x1a500`: the even and odd field
lines) whose IMAGE transfers lay each macroblock on the frame (MOV000L: 192 x 256, 12 x 16 macroblocks, at (224, 47) of
the field). Natively: the frame is decoded by FFmpeg's IPU decoder at the patch's size (the data begins with its flags
byte, as the file's frames) and written as IDEC writes it; `loadImage` runs both chains' transfers (one GIF stream per
chain: an IMAGE tag's texels come through the next DMA tag) into the room picture's field buffers, from which the
picture drawn in the frame is rebuilt. Without them the game faulted on DecodeIPU_Movie's IPU_CMD write when the cut
was reached. A transfer to another place than the frame (BITBLTBUF.DBP neither 0 nor 0x8c0) is a texture: stage 5
room 19's water caustics, a 256 x 256 movie into block 0x1a40, go to the texture uploads (`oni::tex::upload`), not
the room's picture (they had covered its top left corner).

Room 0's only movie cut is 21 (the shrine, `MOV000L`); `ONI_FORCE_CUT=<frames>:<cut>` (a test) changes cut through
`cut_chg_main` after that many frames, `ONI_BGMV_TRACE=1` traces the movies, `ONI_CUT_TRACE=1` (oni_main) the stage,
room and cut. The picture and depth textures are new objects when their pixels change: BozoEngine's renderer knows a
texture by its object (changed in place, the movie's frames never reached the screen).

## Effects: the game's own GIF chains (runtime/oni_model.cpp, runtime/oni_vu1.cpp, 2026-11-23)

With native graphics BozoEngine's `sceDmaSend` completes a VIF1 / GIF transfer without reading it, so every chain the
game sends itself was dropped. In the first room, each frame: `Gifreg_set_dma` (`0x12e7a4`, GS registers),
`Screen_blur_draw` (`0x18ae74`), `Bg_disp`'s field upload (`0x122134` / `0x12214c`); in a fight also `Esp_sp_draw`
(`0x180de4`: the dust at the feet, context-2 sprites, additive at alpha 0x10) and `Spr_draw` (`0x180b5c`: glints).
The port's `sceDmaSend` now runs a GIF chain (channel 2, `0x1000a000`) through the same GIF interpreter as the VU1
packets (`oni::vu1::runGifChain`: the source chain's data as one stream: PACKED, REGLIST, IMAGE), except `Bg_disp`'s
(the picture is drawn natively), then hands the call to the engine's own (`bozo::port::callLibrary`).
`Screen_blur_draw`'s motion blur textures from the other display buffer (blocks 0 / 0x8c0: the previous frame) over
the new one: the renderer's previous frame (`fromFrame`, `kPreviousFrame`), u, v its field texels; at rest its alpha
is 0 and it changes nothing. `ONI_DMA_TRACE=1` lists the sends, `ONI_GIF_TRACE=1` the chains' GIF tags,
`ONI_GIF_CHAINS=0` leaves them out.

## Cheats (runtime/oni_cheats.cpp, runtime/app/oni_cheats_ui.cpp, 2026-11-23)

The window's settings (F2) have a Cheats tab, as the other ports'. Infinite health: the player's object control block
(`_OCW`) is Player_main's work at `0x287ed0`, `Max_life` at +0x6ce and `Life` at +0x6d0 (shorts, 128 at the start);
at each frame's end Life goes back to Max_life. Checked in the courtyard's zombie fight: standing still to frame 6400
Samanosuke is at 98 / 128 without it, 128 / 128 with it. `ONI_CHEAT_HEALTH=1` turns it on without the window.

## The models off the game's thread; the frame's draws in one order (runtime/oni_vu1.cpp, 2026-11-23)

Room 1's gate (half a dozen zombie samurai) costs ~11 ms of VIF1 / VU1 / GIF work a frame (1.8 million VU pairs, 30,000
triangles) on top of the game's own: the window fell to 57 frames/s and the sound underran. Nothing the game reads
depends on that work (it only makes draws), so in the window each chain is copied when sent (VIF1's words with their
places, or the GIF stream) and run in order on a thread of its own while the game goes on; the texture registry is
locked (the game thread uploads, the models' thread looks up). Every frame draw now goes through one ordered list:
the packets' draws, and those the game thread makes (the room's depth and picture, the 2D of the ordering table and
Pit_trans, the movies' fade) with `oni::vu1::queueDraw`, handed to the renderer at the frame's end (`drain`) in the
game's order: the event's cinema bars (Pit_trans) over the characters, as on the GS. The crowd fight then holds
60 frames/s with no underrun. The headless runner keeps one thread (deterministic); `ONI_VU1_THREAD=0/1` chooses.

SCISSOR_1 / _2 are now followed per draw (the game sets the whole field); a mis-encoded default and a case that fell
into SCISSOR from the MIPTBP registers (both found on the way) had clipped everything or nothing.

## More cheats and the room warp (runtime/oni_cheats.cpp, tools/oni_warps_gen.py, 2026-11-23)

The fields are the game's own, named by its DWARF in the work block G (`0x27de30`, `struct @anon28`): magic is
`Sv_magic[3]` (+0xa03e: the lightning, fire and wind orbs, as `pl_att_sh_l` / `_f` / `_w` spend it) against
`Sv_max_magic` (+0x988e); the souls the HUD counts and the gauntlet's upgrades spend (`kyouka_push` / `kyouka_pull`)
are `Exp_accumulater` (+0x9e84); the inventory is `Item_work[65]` (+0x9eac: Id, Num, Type, Status; Num 0xff for an
item not counted). Infinite items puts a count that went down back, and a counted item whose slot emptied back with one.

The room warp is the game's own door. A door is an AOT the room's event script sets with opcode 0x39 (slot, then the
`_SCE_AOT`: Id 1, the area; then the `_IN_DOOR_WORK`: next position, facing, stage, room, cut, floor, door type, key);
opening one, `Sce_Door` points `G.pSce_aot_on` (+0x986c) at it and raises `G.Doordemo_flg` (+0x9879), and
`game_sp_check` switches the game to `game_doordemo` (G's mode byte, +0x44b0: 3), which fades out, takes the stage,
room, cut, spawn point and floor from the door and loads the room. `tools/oni_warps_gen.py` reads every door out of the
disc's room files (`IMAGE/RDT/ROOMsrr.RDT`: 216 doors leading into 84 rooms; the other rooms are reached by events)
and keeps one with a spawn point per destination (83); the warp writes that door at `0x1ffd000` (EE memory nothing
uses) and raises the flag at the next frame in play. Checked: from room 2 to stage 1, room 0 (the castle's gate at
night) in one frame, back in play at the door's spawn point. `ONI_WARP=<stage>:<room>` and `ONI_CHEAT_MAGIC`,
`_SOULS`, `_ITEMS` for runs without the window.

## A sweep of every room, and what it found (2026-11-23)

With the room warp every room a door leads into can be tested: from a stage-0 save state, `ONI_WARP=<stage>:<room>`,
~1800 frames of walking and attacking headless (`ONI_CUT_TRACE`, `ONI_VU1_TRACE=1`), then two captures each in a
hidden window (`--hidden`: no window on the desktop, no sound; the captures from the offscreen picture). Found and
fixed:
- Every MWo3 module (rooms and enemies) was cut 0x40 bytes short: its header counts the text from offset 0x40 (the
  end address at +0x18 is load + 0x40 + text + data for all 176 modules on the disc), and `oni_prepare.py` took
  text + data from the module's start; a function ending in those bytes jumped to no generated code (rooms 0:5, 2:8,
  5:5, 5:18 faulted at once). `tools/wrap_elf.py` (VP2S's convention) counts from 0x40 too now.
- A door whose cut the destination has only in another story state (room 3:3's from 3:2 asks cut 5 of a 3-cut room:
  `cut_replace` maps it in the right state; otherwise the CD manager read a garbage LSN past the disc into past the
  RAM): the warp generator reads each room's cut table (the LINK section whose 16-byte entries chain the Z buffer and
  IPU picture through the room's BGS file) and keeps only doors with a cut the room has (82 destinations).
- Effects reading a display buffer while drawing into another target (room 4:1: the scene copied into the shadows'
  buffer): the one being drawn is the frame so far (`fromFrame`, source 0), the other the previous frame.
- `Md_draw_semi` / `Md_draw_semi_l` (the semi-transparent parts: hair, ...) start their GIF DMA themselves (D2_TADR,
  then CHCR 0x105); the native sites ignored it and those parts were never drawn (Samanosuke's and the princess's
  hair). The chain now goes through the GIF interpreter at the CHCR write.
All 82 rooms then ran clean headless. Left: rooms 2:1 and 5:0 show black, room 5:19 a misplaced icy texture.

The room files are named in decimal (`ROOM513.RDT` is stage 5 room 13; stage 2 runs to `ROOM244`), and the generator
read them as hex: rooms 10 to 15 of every stage were dropped (a door to one asked for `ROOMs0A`..`ROOMs0F`, which do not
exist), and a door to room 16 and up was checked against the wrong room's cut table. Decimal now: 116 destinations
(stage 0 room 6's hole into stage 1 room 13 among them), all 116 clean headless (BozoEngine e15775a).

One-hit kills (the Cheats tab): an active enemy's life (the enemies are G's list pEm_top..pEm_end, linked by pNext
+0x30) is kept at 1, the game's own damage code killing it at the next hit: room 1's gate zombies all dead by frame
26200 with it, all four standing without. Not in stage 0 room 0, whose scripted first fight refills its zombies' life
(dmg_ck checks the place) until its event.

## Saves on disk, fast forward (runtime/oni_main.cpp, 2026-11-23)

Without a card folder the engine keeps the memory cards in memory: a game saved in the window was lost at its end. The
window now puts them in the user folder (`bozo::app::userDirectory()`: `%APPDATA%\Onimusha_Recomp\mc0`, `mc1`), as
Dark Cloud and The Third Age do; headless and scripted runs keep fresh cards in memory (a test never touches the
player's saves), `--card-dir` chooses, `ONI_MEMORY_CARDS=1` keeps them in memory. Not yet exercised by an in-game
save (a save point is still to be reached).

Fast forward needs nothing of the port: the window raises the frame pacer's speed while the FastForward binding (`) is
held, and Onimusha's frames end in that pacer. Checked from room 6 with `PS2X_FAST_FORWARD_AT=6-26`: 3x is ~178
frames a second through the fight, the sound kept to real time (the host skips the surplus), no underrun.

## Next

- Movie captions; the effects of later scenes (magic, the souls), checked as they are reached.
- The six 640 x 64 CT32 uploads into block 0 (the frame buffer) at boot: not drawn yet, origin to find.

## The story bot (runtime/oni_bot.cpp, 2026-11-23)

To go through the story in its own order headless, `ONI_BOT=<script>` makes a bot player 1's controller: a script of
goals (`go`, `door S R`, `take`, `fight`, `play`, `face`, `save`, ...; oni_bot.h) carried out against the game's own
state each frame. What it reads, all named by the DWARF:
- The controls are tank controls on the D-pad (the pad stays digital: the stick is not read): LEFT / RIGHT turn the
  player's Dir_y (`_OCW +0xc2`, 4096 a turn, ~45 a frame), UP walks where it faces (x by its sine, z by its cosine).
- The way: the room's route graph (`G.pRdt_route` +0x488c, the enemies' own: 0x30-byte nodes, position and radius as
  floats, then the node's index, its links' count and the linked indices), shortest path from the node nearest the
  player to the one nearest the goal.
- The doors and items: the live AOTs (`G.Sce_aot[32]` +0x97e8: `_SCE_AOT` Id 1 door, 2 item, 5 event, 8 stairs, 9
  ladder, 10 save...; Type bit 7: a four-point AOT) and, for `door S R`, every door of the room files
  (`tools/oni_warps_gen.py`, Door_aot_set and Door_aot_set_4p: `kDoors`). The game checks an action against the point
  600 ahead of the player (`Sce_at_check`: `(0, 0, 600)` turned by Dir_y), so the bot stops near a door, turns to face
  its middle squarely and presses CROSS; a door that is a wall (a staircase) is entered that way. An item is taken
  when the item window (fade.c's `Fade_w`, type 5) has opened and closed again (its AOT's Id goes to 0).
- Enemies within reach are fought first (turned to, SQUARE), with the one-hit-kill cheat.

- In play means game_move and nothing stopped: `G.Stop_flg[0]` (+0x4a60) is set through an event, a message, the item
  window and the menus (and for a frame or two at a cut change); while it is, the bot presses CROSS now and then, which
  advances the messages. `examine` (an event AOT, Id 5) is done when what it started is over.

Reached with it, in story order from the forest cave's state: stage 0 room 6's herb and its hole into the underground
temple (stage 1 room 13), the temple's map box and its stairs into room 14, the mummified monk's event (its messages,
the Green Scroll) and the door it opens into room 15, the corridor and its far door into room 16 (the blue orb's
room). Every room drawn as it should be so far.

## The trick boxes' puzzle and the room's depth (runtime/oni_bg.cpp, 2026-11-24)

Stage 1 room 16's trick treasure box (set06.c: two dials, six numbered tiles, three turns) showed two of its six tiles,
cut off at odd lines. The tiles are 3D models (TEST GREATER against the Z buffer) seen through the puzzle's own
close-up camera (set06_camera: Set_camera from the box's table) over the box's picture; the puzzle reads no Z buffer
(only its 39-sector picture), so Bgz_disp kept sending the room's last cut (another camera: the corridor's corner)
and most of the tiles fell behind it. set06 raises Status_flg (0x2828ac) bit 30 (`Flg_on(.., 1)`) while the puzzle
is up; the room's depth is not drawn then. Solved by the story bot (left, right, left): the Rope Ladder.

## The story bot's way-finding (runtime/oni_bot.cpp, 2026-11-24)

Some rooms have no route graph (the moat's floor, stage 1 room 25), and a straight line hits the walls. The bot now
plans on the room's own collision mesh: `G.pRdt_sca` (+0x4880) and its size (0x27c638) hold the vertices (float
triples), the triangles (u16 index triples) and an attribute byte each, the counts in the last 6 bytes (as
Sca_data_set_sub reads them). Every tall steep triangle (a wall: over 600 high, so a staircase's risers are not) at
the player's height marks the cells along it on a 100-unit grid, widened by the player's radius; the way is A* through
the free cells to the free cell nearest the goal (a door is in a wall), straightened by line of sight. The route graph
remains the fallback. `door S R` takes the room's live door AOT to that place first (a door the script replaced by a
locked event is no longer live).

Played with it: the Thunder Orb's dark event, the crystal-sealed door, the castle gate's fight, the dry moat (the Rope
Ladder from the status screen) and its floor, and the first boss (the mace-wielding ogre: its introduction, the
fight, "How could I die..."). All drawn as expected.

## Cinema bars and the 24-bit frame (runtime/oni_sprite.cpp, runtime/oni_vu1.cpp, 2026-11-24)

Stage 2 room 30's event (the tentacled thing over a corpse) showed its top cinema bar only on the left. The bars are
Pit_trans sprites blended (Cs - Cd) Ad + Cd: by the destination's alpha. The display frame is 24-bit (FRAME.PSM 1):
it keeps no alpha and the GS reads Ad as 0x80 (as PCSX2's GS does: ALPHA.C = Ad on a 24-bit frame becomes FIX 128),
while the port blended with whatever alpha its own draws had left there (0 under the room's lit patch). Ad on a 24-bit
frame is FIX 0x80 now, for the GIF / VU1 draws and Pit_trans alike (the title's PRESS START fade and the menus
otherwise unchanged).

Found on the way: Screen_blur_draw's pass draws with context 2, whose FRAME the game never sets (0); taken as page 0,
it made the blur's read of the other display buffer (the previous frame) a read of the frame so far. An unset FRAME
no longer says which buffer the frame is drawn into. `ONI_DRAW_ORDER=1` lists each draw of a frame in the order the
renderer gets them (material, blend, bounds); `ONI_NO_BLUR=1` leaves out the draws that read a display buffer.

Played on: the ogre's room's box and the passage behind it, into stage 2 room 30's event.

## The keep, the Magic Mirror (story legs 13-21, 2026-11-24)

Played on in the story's order by the bot: the Fire Orb (stage 2 room 30's fire stand), back up the moat's rope ladder
(an event AOT, four-point), the red seal on stage 2 room 0's south door (the Fire Sword equipped) into the keep's hall
(stage 5 room 0: the LINKS01 movie in its place now), the save room, Yunemaru's event, the long talk with Tokichiro,
the red-orb door, Kaede and Yunemaru's event, the Bow's trick box (the dial puzzle again: 5 3 2 / 6 1 4, five turns,
solved left, right, left, right, right), the Magic Mirror's ENHANCE (Enryuu and Kouen to level 2: the double red-orb
door asks for Kouen's, "This will not open at your current level" below it) and the double red-orb door's room.

- The Magic Mirror (an AOT Id 10): "It's a Magic Mirror.", then SAVE / ENHANCE / EXIT. SAVE runs the memory card
  screens through: "A game data for Onimusha could not be found", "OK to create a file?" (NO selected first), then
  file 1 written ("The Keep: Sentry Box", save count 1). ENHANCE pours souls into the selected sword or orb while CROSS
  is held (the level when the bar is full).
- The bot: an event holds System_flg (+0x4a78) bit 18 through all its lines and cuts (the stop flags only now and
  then): in play only without it. Its timeouts count frames in play (an event on the way does not use them up). Held
  on the way (no nearer the next point in 90 frames: sliding along a wall still moves the player), it takes the place
  ahead for an obstacle the mesh does not hold and plans again; an item or event it cannot get closer to is faced and
  pressed from where it is held. Stage 5 room 15's U is held by something the mesh does not have: that leg takes
  shortcuts within the room.

Leg 22 goes on through the storeroom's pail, the hall's upper floor and its single red-orb door, rooms 13, 14 and 17
and the double red-orb door there into room 18: Kaede's event, the two of them trapped behind bars ("Samanosuke, we
must find a way out!"). Next: the lever puzzle (seven lever AOTs, Id 5: one by the start, a pair, a set of three, one
by the door; Message2 AOTs 15 and 16 are the bars between the sections). The characters swap there (Player_change:
Pl_swap_info, the HUD turns to Kaede's single bar), and the levers on the north wall were out of reach from where the
active one stood in the first tries: the bot needs this room's map (its collision mesh reads as garbage here).

Leg 23 is the lever trap: each pull hands control to the other character (the active one is always the player _OCW
at 0x287ed0); the levers stand behind bars from where each one is, so the leg takes in-room shortcuts to each. The
order: Samanosuke the lever by the start, Kaede the right one of the pair, Samanosuke the left one, Kaede the left one
of the three (the middle one is a trap), Samanosuke the right one, Kaede the lever by the door, Samanosuke the
door-side one (a Message2 AOT, 24), then the door into room 19. Room 19 is dark: the Fire Sword's special
(R1 + TRIANGLE, a chord in the bot's `press`) lights its candles, the fire effect and the lit room drawn as they
should be. The way on from there (the floor-tile puzzle, then the crest) is the next leg.

Legs 24-26: room 19's second candle set lights from the room's east side (then the door with no keyhole is a door);
room 20 is the fall-trap floor (room520.c's Om8c: 4 x 8 panels, `Om` objects 140 plain, 141 the rook "+", 142 the
bishop "X", 143 the king "O"; each panel's grid column and row at +0x7b0, its type at +0x20). Stepping on a piece
records it and fills a table of distances (0x4d9920) along its lines: the rook's row and column, the bishop's
diagonals; every panel there falls, the nearest first, and a character on it drops back to the start. The stepping
character is caged and the control passes to the other (the player _OCW is always the active one). Searched in
Python over that model and played by the bot (`piece X Z`: walk onto a tile until the control passes): Samanosuke and
Kaede take seven turns down to the king, the cages open, and the door to room 21 (the crest puzzle) is reached.
`tools/oni_disasm.py` reads a prepared module too (build/bozo/oni_prep/roomNNN.elf).
