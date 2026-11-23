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
6. **Current stop** (frame 16): `getFlags` `0x123790` writes `IPU_CMD` (`0x10002000`): the first background decode.

## Next

- **Backgrounds**: answer the IPU decoders natively at their game functions (`decodeIPU` `0x1221ac` region,
  `Cut_set`, `Cut_set_onmemory`, `getFlags`, `Bgz_disp`; the animated ones `DecodeIPU_Movie` `0x197bc0`), decoding
  the `.IPU` streams (MPEG-1 intra frames; FFmpeg has a PS2 IPU decoder) into textures for the native renderer.
- Movies: the PSS files through BozoEngine's native libmpeg (`sceMpeg*` are answered natively; FFmpeg needed:
  `BOZO_REQUIRE_FFMPEG`, TTA's `scripts/build_ffmpeg.sh`).
- Renderer: Ot_draw / Spr_draw / Esp_sp_draw / Pit_trans and the VU1 programs (`Vu1mc_*`) as native draw hooks; the
  VU0 programs (`_$VU0_*`) translated to C++ (TTA's tools/tta_vu0_gen.py).
- Pads and memory cards (native libpad/libmc; the modules are answered at the EE APIs).
