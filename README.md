# Onimusha_Recomp

A native PC port of *Onimusha: Warlords* (NTSC-U, `SLUS-20018`), made by statically recompiling the game's
PlayStation 2 code to C++ on [BozoEngine](https://github.com/TheBozos/BozoEngine), The Bozos' own toolchain: the
game's EE code recompiled to C++, every library and kernel call it makes answered by native code. No PS2 hardware
model at any level.

> [!IMPORTANT]
> No game files are included: no ELF, IRX modules, data, or code generated from them. You need **your own copy** of
> the North American release, `SLUS-20018`:
>
> | File | SHA-1 |
> |---|---|
> | `SLUS_200.18` | `21166f0c7ec1ee2e6dfb87e81af242efaf521dc7` |

## Status

Early bring-up. The executable and its 98 code overlays (enemies, rooms) are recompiled; Capcom's sound driver and
Sony's synthesizer run as statically compiled native modules; the game's main loop runs its first frames headless
and stops at the first background decode (the IPU). See [docs/RECOMP_NOTES.md](docs/RECOMP_NOTES.md).

## Build (Windows, MSYS2 UCRT64)

```sh
git submodule update --init
scripts/extract_iso.sh "Onimusha - Warlords (USA).iso"   # into game/
scripts/build_runtime.sh -DONI_WINDOW=OFF                 # build/bozo/oni-headless.exe
scripts/run.sh --frames 600 --profile-io
```

## Licence

MIT, see [LICENSE](LICENSE). Game data is never part of this repository: the port builds from the player's own disc.
