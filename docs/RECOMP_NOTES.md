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
  Natively the same walk turns each primitive into a frame draw in screen space (identity matrix, table order over
  the background): the frame pass has the GS blend equation, the alpha test and point sampling (the overlay pass
  reduces them to plain blends). `Ot_draw` starts by setting `TEST_2` to 0x7000d: alpha test GREATER 0 (transparent
  texels are not drawn, which keeps the menu's frame under its items) and the depth test, ZTST GREATER, against the
  frame's Z buffer; `ZBUF_2` is written by opaque primitives and not by blended ones. Each vertex's Z is in the
  primitive (layout 2 +0x10 per 12 bytes, layouts 3 and 5 +0x14 per 16 bytes, the sprite +0x14 and +0x20), so the
  native draws carry it and test it too (`ONI_2D_DEPTH=0` turns that off, a diagnostic). The 3D effects queued in
  the table are cut by the room's depth that way (Boss 3's lock wall below), and the Magic Mirror's ENHANCE page
  hides the Bullet row before the matchlock and shows the souls' count (both were wrong without it).
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
player's saves), `--card-dir` chooses, `ONI_MEMORY_CARDS=1` keeps them in memory. A hidden window (captures) and
the story bot keep them in memory too, unless given `--card-dir`.

In-game save and load (2026-11-25), checked with the bot and a test card folder (`--card-dir build/story/cards`):
the Magic Mirror's SAVE in stage 5's Sentry Box (after leg 49) checks the card, picks slot 1 and file 1 ("ERROR: A
game data for Onimusha could not be found." on an empty card, then "OK to create a file", YES), "Now saving...",
"Save completed." with the save count, play time, place and its picture. On disk: `mc0/BASLUS-20018/` with the
save (`BASLUS-20018`, 17184 bytes), `icon.sys` and the three `slime*.ico` models. A fresh boot's main menu then
opens on LOAD GAME; slot 1 lists the save, "Will you start the game with this data?", YES, and the game resumes
in the Sentry Box at the mirror. Its items are the same as the saved game's; the flags differ only in a room's own
word (word 8, cleared and set again at every door) and a room counter. Legs 50 (the rest: ENHANCE) to 58 chained
from the loaded game end as from the bot's own states. (Leg 55 first stopped there: in stage 2 room 7 the archer on
the wall knocked the bot into the north-west corner, where its way to the door to room 5 did not recover; with the
loaded game's other timing, 90 frames later failed and 30 passed. A teleport to the door's front now, both copies.)

What finishing earns (2026-11-25), from the branch's end state (`b_s_end`, twenty Fluorites) with a test card: after
the RESULT screen's tally, three lines ("If you save this game data, the "Special Trailer" / "Oni Spirits" game /
"Samanosuke Extra" will be available.", CROSS each), "Will you save the game data?" (YES is LEFT), GAME CLEAR DATA
on slot 1 ("OK to overwrite the data?": the card's system part, in the same `BASLUS-20018` file), "Save completed.".
`Title_check` reads it at boot (`sys_load`). The title then has NEW GAME / LOAD GAME / Special Feature / OPTION:
- Special Feature: Special Trailer (`LINKS08E.PSS`, the Onimusha 2 teaser, "10 years have passed...": 2100 field
  pictures, all shown) and Oni Spirits (its menu START / RESULT / RETURN, the scene with subtitles, the stage card
  "RELEASE ONI 28, BREAK VASES 20", play among the Oni pots with the RELEASE count; life drains on the holy ground,
  "Died" and its RESULT with a save prompt).
- NEW GAME: "Samanosuke NORMAL / EXTRA"; EXTRA is the panda costume (`pl_panda_head`, `pl_kopanda_*`), in the
  opening events and in play.
All native, nothing to fix. (The bot's `end` step stops a window run: a long `wait` keeps a capture going.)
- A save state brought its memory cards back without their folder, so a game saved after loading a state stayed in
  memory and was lost at exit: BozoEngine #141 (1da19c9) reads the cards again from their folders when a state is
  loaded with one; the engine is at b7298d5 (master) for it. Without a folder (tests) the state's cards are kept.

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

Leg 27: room 21, Kaede alone while Samanosuke is in the water prison. The control board by the iron door is a sliding
puzzle (room521.c's om8a: panels are `Om` 138, the grid cell at +0x7b0 and +0x7b2; the cursor is the one whose
+0x23 is 2). The board is 6 x 4: five 2 x 1 wave panels, two 1 x 2 waves, two 1 x 1, the two 1 x 2 halves of the
Saito crest at the far sides and a 2 x 2 hole in the middle; the crest whole in the middle opens the cage. Solved by
BFS (`tools/story/crest_puzzle.py`, 26 one-cell slides). Its controls, from the code and the game's pad words at
0x281e30 (held: CROSS 0x40, CIRCLE 0x20, TRIANGLE 0x10, SQUARE 0x80, UP 0x1000, RIGHT 0x2000) and 0x281e34 (pressed
this frame): the D-pad moves the cursor cell by cell to the next occupied cell, CROSS held grabs the panel and a fresh
direction press slides it (the bot's `cursor C R`, read back from RAM, and `slide CROSS DIR`). The water drains and
Samanosuke is free.

Legs 28-30: Samanosuke out of the drained cell (room 21's collision mesh does not read as walls: waypoints by the
corridor's west end) into room 22 with Kaede; the table's tab opens the way up to room 24 and its door with two crest
sockets; the right crest piece fits (used from the item menu). The left piece is next.

Leg 31: the left crest piece is item 72 (the right one 71: found by scanning every room file's Item_aot_set for
them), in room 16, reached through room 23's far door; back up, both pieces in, the door opens on Tokichiro's scene
in room 25 ("Princess Yuki's skull will be filled with her own blood...").

Legs 32-33: the arrows, rooms 26, 27 and the save room (29), its ladder to the rooftop (room 30); boss 2 (the
warrior with the wheel shield) beaten and the Wind Orb taken ("You got the Wind Orb.", the Wind Spear's ceremony),
every scene drawn as it should be.

## The orb doors and the keep's underground (story legs 34-40, 2026-11-24)

Legs 34-40, in the story's order: down from the roof to room 27's green-orb door (an event AOT over a disabled door: the
Wind Orb) and Nui's scene in room 8; the Magic Mirror's ENHANCE (Shiden, the thunder orb, to level 2: the double
blue-orb door in room 2 asks for it, "there seems to be a thunder inside the crystal..."; the wind pair too); the
Thunder Sword equipped from the status screen's EQUIP page (START, DOWN to the orb's row, CROSS, CROSS for YES,
TRIANGLE twice; the page keeps its cursor between visits); room 4's stand of three orbs, examined with each sword in
turn, opens the way down to stage 2 room 24; the save room's shelves; room 25's rope by the wheel slashed (the bridge
comes down) and its crossing for the Rosary; room 28's Purifier Bell down the bladed hall; the bell used from the item
menu at room 24's door the ghosts block; room 32's double green-orb door (the Wind Spear) and room 33's trick box; the
Blue Key on room 29's blue-ink door; room 43's event (a type 0x41 AOT, entered by walking: the demon's trap), and
Kaede wakes in stage 5 room 8.

- The trick box in room 33 is set06 again, its third board: 2 x 4 tiles, three knobs between the columns (dial24_tbl),
  CROSS turns the four tiles around the selected knob clockwise, LEFT / RIGHT select, three turns (REST). The boards and
  their answers are tables in the main ELF (set06_puz_Q 0x20bec0, set06_puz_A 0x20be40; read with the -1 cells skipped):
  2 4 3 8 / 1 6 5 7 to 1 2 3 4 / 5 6 7 8, found by BFS: middle, left, right.
- How the game takes a door: `Sce_at` calls `Sce_at_check(player, 1, 0x10)` only on the frame Status_flg (0x2828ac)
  bit 21 is up (the action button pressed in play), and an AOT whose Type has bit 4 (0x31: doors, items, events) is
  only looked at then; bit 5 tests the action point (600 ahead), bit 6 the player's own position (0x41: walked into).
  The hit slot goes to 0x2875b0, a door then raises 0x2876a9 with its data at 0x28769c. Room 27's door to room 28 lies
  ~500 units past the wall in front of it: the bot stopped to press only within 450 of a door, so it never pressed
  there. 560 now. The bot also closes a message a door raises ("You have used the Blue Key.").
- `ONI_WATCH=<addr>[,<addr>...]`: 32-bit words of the game's RAM logged when they change (frame ends).
- Some rooms' collision meshes hold no walls the bot can read (stage 5 rooms 2 and 29, as 18 and 21 before): waypoints
  or in-room shortcuts there.

## Kaede's part, Princess Yuki, Boss 3 (story legs 41-50, 2026-11-24)

Legs 41-49: Kaede from stage 5 room 8 (the dying soldier's Red Key) down to the courtyard and the moat's red-ink door
(key 176), room 10's yard (the archers knock her back from the partitions: a shortcut to the ladder's foot) and its
balcony, room 14 (the West Area Map, the Shinobi Kit's door), the trick shelf's room (6) and the furnace (stage 1 room
23: the pull tab, the flames, the Gold Plate), room 22's Gear by a corpse (an AOT Id 11) and its two holders by the
iron door (the Gold Plate, then the Silver Plate from the shelf's secret room), the burning room, room 38's trick box
(the knob board again, 6 8 4 7 / 2 5 1 3 in five turns: the Sacred Knife, equipped from Kaede's EQUIP page), the flesh
bars of room 39 cut down, Princess Yuki's scene in the prison, the vanishing ninja; Samanosuke's Boss 3 (his double),
stage 1 room 17's Evil Plate and Wood Ladder, and up the ladder into the keep. Leg 50: the save room's hanging head
offers the Dark Realm (optional, declined: "Shall we go?" opens on YES), and Shiden to level 3 at the Magic Mirror.

- The item menu keeps used items, greyed (the Gear, the Gold Plate once inserted); its cursor opens on the Herb, so a
  key item is UP from there by its place in the list.
- Bot: the door step also closes a message in the room it leads to ("You got the Red Key."); `go X Z R N` with a
  negative N ends quietly after |N| frames in play (a way that gives as the player runs into it, past which the point
  is behind); `engage off|on` (enemies on the way not fought: room 39's hanging flesh cannot die); `door S R enter`
  ends at the room's entry, leaving an event's question to the next steps (the door step otherwise presses CROSS on
  messages, which took the Dark Realm's YES). `ONI_DRAW_ORDER=2` lists each draw's vertices too.
- Boss 3's red cloud (fixed 2026-11-25): `esp080c_lock_wall` `0x1ce980`, the wall that seals a fight's doorway. Its 3 x
  3 grid in world space (4000 x 4000 at stage 2 room 42) is projected each frame into four layout-5 quads in the
  ordering table (attribute 0x05008b5c: a strip in context 2, blended by the texture's alpha), drawn twice with two
  128 x 128 noise textures (PSMT8, CT32 palettes whose alpha follows the grey) scrolled by `Texmove_entry`; the
  vertex alpha falls only from the middle row (the effect's +0xcc) to the top and bottom rows (a quarter of it), so the
  sides are hard by design. On the GS `Ot_draw`'s depth test hides them behind the doorway's rock; the native walk
  drew without depth, a rectangle over the room. It now tests each vertex's Z (see the ordering table above).

## The triple orb doors, the Statue of the West (story legs 51-54, 2026-11-24)

Legs 51-54: from the save room back out to stage 2 room 7 and its triple blue-orb door (Shiden at level 3, the Thunder
Sword equipped) into room 8, the Statue of the West's room (Genbu, the Vision Staff); back through room 10's yard and
balcony to room 14's triple red-orb door ("This will not open at your current level": Kouen to level 3 at room 14's own
Magic Mirror, the Fire Sword equipped), room 19 and room 20 (the ogres, the Statue Head, the bullets, the Decorated
Sword); back to the headless statue: its head from the item menu, the gate retracts, the Matchlock.

- The EQUIP page opened on its Sword row here (in leg 36 it had kept its last row; why is not known): a capture of
  the page before scripting the presses. The orbs are rows 2-4 (Shiden / Raizan, Kouen / Enryuu, Arashi / Shippuu).
- Room 19's garden (an L round a building) and room 10's yard (the archers) take waypoints / a shortcut; room 14's
  Magic Mirror is reached round its north wall. Room 8's two items are taken Vision Staff first (after Genbu the
  other is out of the bot's reach).

## The Evil Plate, stage 3, the pond's water (story legs 55-60, 2026-11-24)

Legs 55-60: the Evil Plate on stage 5 room 7's south door (item 0x44; found by scanning the room files for the script's
used-item check, `0b 00 <item> 00 01 00`, a flag AOT at the player's position) and the demon's scene at the Demon Gate
(stage 4 room 0); back through the keep in ruins (rooms 33 and 32 stand in for 7 and the hall) to the courtyard, the
Decorated Sword on its east door's panel (item 0x45), stage 3: room 0's double green-orb door, rooms 1-3 (a book in
demon script, the Bullets), room 6's triple green-orb door (Arashi to level 3 at stage 2 room 14's mirror, the only one
on this side being past that door), the garden (7), rooms 8-10, the boathouse (11), the small boat to the turret (12,
13), the scorpions' scene; Kaede in room 0 against two scorpions and through room 4's door (key 181). Next: Kaede's part
(the Great Arrow: a puzzle box and a replica-arrow weight puzzle, a Green Key, a second boat), then the Queen Bee.

### Stage 3 room 7's pond (runtime/oni_model.cpp, oni_vu1.cpp, oni_tex.cpp, oni_bg.cpp)

The garden's koi lay on a dry bed. The room's picture is the drained pond; the water is a background movie of type 2
(Movie_init's setLoadImageTags2: its frames go to GS block 0x1a40, 640 wide, as 16 x 16 macroblocks, not into the frame)
drawn each frame by the room module's set07_trans -> StencilMapPolygon: a strip over the pond's outline per cut, textured
from 0x1a40 (CT24) and blended Cd + (Cs - Cd) * 0x69 / 128. Three things kept it away:

- StencilMapPolygon sends its packet with sceDmaSendN (a normal-mode transfer), which BozoEngine's libdma completes
  without reading it under native graphics; only sceDmaSend was hooked. sceDmaSendN on the GIF channel is now drawn
  (oni::vu1::runGifData). Rooms 5:1, 5:19, 5:24 and 2:25 use it too.
- Block 0x1a40 is also the shadows' buffer (page 0xd2): every read of it was taken as the shadow target. On the GS it
  is a matter of order (the movie uploaded, the pond drawn from it, then the shadows drawn over it and read back): an
  IMAGE upload into a target's block now makes it a texture again until the next draw into it, and a type-2 movie's
  chain (only the one the game sends: the field shown) goes to the GS in its place among the frame's draws.
- The texture cache kept one upload per block: the movie's 800 macroblocks at their places became one 16 x 16 tile.
  32-bit uploads into a buffer at least 256 wide are now composed into one image at the buffer's first block.

`ONI_DRAW_ORDER=2` lists each draw's vertices; `[oni:gif] local->local` would log a GS-to-GS copy (none seen so far).

## Kaede's Great Arrow, the Queen Bee (story legs 61-64, 2026-11-24)

Legs 61-64: Kaede from stage 3 room 4 (room 5's vase broken: Apocalypse #2) back round the lake to the boathouse and
stage 2 room 13 into stage 1: room 4, room 5's demons, the save room's ladder to the attic (room 21: crates the mesh
does not hold; the book in demon script), room 18's ladder to room 19 and its trick box (the knob board: 7 5 4 2 /
8 1 3 6, seven turns, left, left, middle, right, right, middle, middle: the Decorated Arrow), room 6's dragon pedestal
(examined, it gives up the Great Arrow and the doors shut; the Decorated Arrow in its place reopens them) and the odd
box (the Green Key). The Green Key opens room 5's door to room 8 (key 177): the second dock, the boat to the turret
(stage 3 rooms 14, 12, 13), "Samanosuke, a monster is coming!" and the Queen Bee (one-hit kills); the turret burns
down, the escape by boat, Kaede hurt and resting in stage 5 room 8.

- The room files' item AOTs (opcode 0x3a / 0x3b, Id 2: the item number at +0x0e) and used-item checks (`0b 00 <item> 00
  01 00`) are how the next step's room is found when the walkthrough is vague.

## The Demon Realm, Fortinbras, the ending (story legs 65-67, 2026-11-24)

Legs 65-67 finish the game: Samanosuke back through the ruined keep to the Demon Gate (stage 4 room 0), the Great Arrow
from the item menu (a flag AOT over the room for item 0x4e) breaks the seal; the Demon Realm (room 1): Guildenstern's
"Come, Marcellus!!" and Marcellus; the Dark Fountain (room 2); room 3, a ramp spiralling down a ring (about 9000 lower
a turn: two and a half turns by waypoints S, E, N, W; the bot's planner works on one level, so it is not used there),
the king's door at its foot; room 4: Fortinbras, Princess Yuki and Yumemaru, Nobunaga's return, the fight (one-hit
kills), and the ending in mode 10 (the movie, the staff roll, "Presented by CAPCOM") up to the RESULT screen and its
save prompt, about 36000 frames. Every scene and the movie drawn as expected.

- Bot: `fight N M` also ends once the game is in mode M (Fortinbras stays in the enemies' list while the ending runs).
- The optional parts: the Dark Realm, the Bishamon Sword, the Great Armor and the twenty Fluorites are a branch of
  their own (below).

## The Dark Realm and the Bishamon Sword (scripts/story/bishamon, 2026-11-24)

The optional branch, played by the bot as a variant of the main run: `scripts/story/bishamon/run.sh` plays leg 68 from
the main state before leg 50, then the main legs 51-67 on states of its own (`STORY_PREFIX=b_`: leg.sh renames a
script's saves), a leg in the branch folder taking the place of the main one where the branch differs, and leg 69
after leg 65.

- Leg 68: the save room's hanging head offers the Dark Realm once (after Boss 3: "Shall we go?", YES); stage 4 room
  5, twenty floors of demons in one arena. When a floor's waves are done two pillars of light stand (event AOTs, type
  0x51, stood in and pressed): slot 0 down, slot 3 back up to the save room. An enemy that stays hidden is left after
  a while (`fight -N`: a negative limit ends the step without failing) and the round played again; forty rounds
  cover the twenty floors (after floor 20 the down pillar is gone). Then six rounds of fighting (floor 20's waves
  may still be coming when the forty rounds are done), the Bishamon Ocarina (item 0x50) and the pillar up; once back
  in the save room they do nothing. The leg's run varies (the floors' fights do), so it is written to tolerate it.
- The ocarina is a key item listed just above the Herb, so every later key-item use from the menu is one row further
  up: the branch's own legs 54, 55, 56, 62 (Kaede shares the inventory) and 65. Leg 53 approaches the ogres' yard's
  boxes from the east (where their bodies fall differs); leg 55 passes the hall's balcony archer by.
- Leg 59 (branch copy): the Great Armor's trick box behind stage 3 room 10's waterfall (event AOT slot 3): a 3 x 3
  board, four knobs moved between with the D-pad (DOWN from the top-left one to the bottom-left), CROSS a clockwise
  turn; 1 3 8 / 2 5 9 / 6 4 7 (the question table's third board) to 1 2 3 / 4 5 6 / 7 8 9 in five turns: bottom-left,
  top-right, bottom-right, bottom-left, bottom-right. Equipped from the EQUIP page's armor row (an empty row between
  Normal Armor and Great Armor).
- Leg 69: in the Dark Fountain's room (stage 4 room 2) the ocarina from the item menu (a flag AOT for item 0x50 over
  the room's west side), the wall opens, and the pedestal (event AOT slot 5): the Bishamon Sword. Legs 66-67 then
  finish the game with it.

### The twenty Fluorites

The branch also collects every Fluorite (item 7; the RESULT screen counts them): 17 lie in rooms, 3 are Dark Realm
floor rewards. The branch runs from leg 00 (`run.sh` with no argument; leg 0 from the main run's own start state), so
its copies of the early legs pick them up on the way.

- Most are glints: AOTs of Id 11 placed by the room script's opcode 0x7a, the item at +0x0e, that keep their AOT once
  taken. `take` now accepts them (Id 2 or 11) and calls the item taken when the inventory (G.Item_work[65], +0x9eac)
  changes. A glint of type 0x51 (bit 0x40) is checked at the player's own position, so `take` walks into it rather
  than next to it. `take K -N` is optional (none there, or not reached in N frames, goes on); `take -1` is the nearest.
- Where a glint can't be walked to (behind props, at a wall), the leg teleports next to it, faces it and presses:
  1:24, 5:14, 5:25, 1:21.
- 2:19's lies on a step in the path of the siege cart two ogres push up the yard: where the cart stands when the
  player comes in varies, and on the step it covers the glint (the player is pushed out of its box). Struck from the
  step's south side it rolls back, and the glint is taken from a teleport into it.
- 2:6's lies in its secret room (leg 44); 3:11's up the turret's stairs (leg 59).
- 3:15, the bell loft, is reached from stage 3 room 6's scaffold: up its internal stairs, round its loops (west side
  north, north side east, east side south, each a level higher) to the top walkway's west end, where a ladder
  ("There is a ladder.", CROSS facing -13610 7900) climbs to room 15. An archer out of reach shoots along that
  walkway and its arrows knock the player back east, so the last stretch is a teleport. Room 15's event 1 leads back
  down to room 6.
- The Dark Realm's three: room405's `Set0e` reads a table of ten entries (item, first floor, last floor, count, flag)
  at 0x4daa00 and draws a floor for each: Fluorites in floors 4-8, 9-13 and 14-18 (the counter at 0x287225), the
  Ocarina (0x50) at 19. On its floor `Item_replace` puts the item in AOT slot 6 by the arena's north wall
  (-7600, 13000), where the Ocarina is taken too. Leg 68 presses there after every floor; an empty spot does
  nothing.

## The window under load, texture scrolls, the profiler (2026-11-25)

Frame time in a hidden window (paced, rendered offscreen; no sound device, so underruns are not counted: a frame over
16.7 ms is what crackled before), the bot playing with the cheats, `PS2X_FRAME_STATS=1` (`build/story/perf.sh`):

| Scene (leg) | Seconds | Work per frame, average | Worst 2 s window, p95 | Frames over 16.7 ms |
| --- | --- | --- | --- | --- |
| Boss 3 (48) | 66 | 4.1 ms | 15.0 ms | 7 / 3921 |
| The ogre boss (10) | 112 | 5.6 ms | 13.0 ms | 6 / 5970 |
| The Queen Bee (64) | 98 | 5.0 ms | 12.8 ms | 1 / 5911 |
| Marcellus, the Demon Realm (65) | 98 | 3.6 ms | 7.3 ms | 6 / 5898 |
| The King's Chamber (66) | 98 | 4.5 ms | 9.3 ms | 5 / 5895 |
| Fortinbras (67) | 98 | 3.0 ms | 8.3 ms | 7 / 5908 |
| The Dark Realm's first floors (68) | 148 | 4.5 ms | 12.0 ms | 1-3 / 8925 |

Steady 60 fps; the frames over are single spikes at room loads. (A first Dark Realm run showed 35-45 fps stretches
that two later runs did not: something else loading the machine; it also took 172 s for the same frames.)

- Texture scrolls: `Texmove_entry` 0x197240 moves a texture's rows with GS local-to-local transfers (TRXDIR 2): the
  first rows saved to block 0x1a40, the rest moved up, the saved rows put back at the bottom; one or two rows a frame.
  The lock walls' two noise layers (`esp080c_lock_wall`, 8-bit 128 x 128: stage 2 rooms 39 and 42, stage 5 room 30)
  and the Demon Gate's energy (stage 4 room 0, 256 wide, 56 rows a move) scroll that way. The port had only printed
  each transfer (thousands of lines a fight, slow on a console) and drawn them still: `oni::tex::copyLocal` now makes
  the copy between the kept images of the same format (an 8-bit scratch image for 0x1a40, whose kept image is the
  background movie's CT32 canvas). In 300 frames of each of 105 story states, only those rooms move textures this way.
- BozoEngine's sampling profiler (BozoEngine #144, 467ff78): F2's Profiler tab, or `PS2X_PROFILE=<from>:<to>[:<file>]` with
  the window build (`--hidden` for scripted runs). Threads "game" and "window" from the engine, "vu1" from
  `oni_vu1.cpp`'s worker. The Dark Realm's arrival: the game thread mostly waiting
  (`ntdll.dll!ZwWaitForSingleObject`, the frame pacer and the VU1 worker), then the VU1 programs (`oni_vu1_std_nc`,
  `oni_vu1_std_cl`), the background movies (`decodeIpuMovie`, `mpeg2_decode_block_intra`), the game's code
  (`body_0x...`).

