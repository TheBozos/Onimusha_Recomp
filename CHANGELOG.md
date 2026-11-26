# Changelog

All notable changes for players. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and versions follow [Semantic Versioning](https://semver.org/).

## [Unreleased]

## [0.2.0] - 2026-11-27

### Added
- Settings inside the game: in the OPTION screen (from the title, or SELECT in play), L1 / R1 turn to Graphics (display mode, resolution, render scale, anti-aliasing, texture filter, frame rate, widescreen), Cheats (and the room warp), Save States (ten slots) and Close Game, drawn like the game's own Options and driven by the pad. F2 keeps control rebinding and the profiler.

## [0.1.1] - 2026-11-25

The first release: Onimusha: Warlords (USA, SLUS-20018) as a native PC game, built on your PC from your own disc.

### Added
- The whole game, natively at 60 frames a second: the story from the forest cave to Fortinbras, the ending and the RESULT screen; the optional Dark Realm, the Bishamon Sword, the Great Armor and the twenty Fluorites; after the credits, Special Feature (the Special Trailer and the Oni Spirits mini-game, its twelve holy grounds) and Samanosuke EXTRA.
- The pre-rendered rooms and their animated backgrounds, the movies with their sound and the epilogue's captions, the characters drawn by the game's own microprograms with the rooms' depth and their shadows, the effects (the magic, the souls, the lock walls' mist, the Demon Gate's energy).
- The game's own saves on memory cards kept in `%APPDATA%\Onimusha_Recomp`, loaded from the title's LOAD GAME; the GAME CLEAR DATA and its unlocks; Easy mode after four game overs, as on the console.
- PC options in the settings window (F2): widescreen (a 16:9 zoom: the rooms are 4:3 pictures, so the top and bottom are cut; the menus stay 4:3), render scale, anti-aliasing, texture smoothing and upscaling, frame rates above 60, window size and full screen, rebindable controls and gamepads, save states (F5 / F7), screenshots (F12), fast forward (`).
- Cheats (F2, Cheats): infinite health, magic, souls and items, one-hit kills, a room warp.
- A sampling profiler (F2, Profiler), for reports of slow spots.

### Known limits
- The whole game was played through by a scripted test player (with cheats, and teleports where it could not find its way); a human playthrough with a controller is still to come.
- Linux is not qualified; the release is Windows only.
