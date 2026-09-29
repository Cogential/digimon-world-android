# Digimon World for Android

A native Android port of **Digimon World (USA, SLUS-01032)**, built on the
[psx-recomp-port](https://gitlab.com/ethan4love/psx-recomp-port) static
recompilation and the [PSXRecomp](https://gitlab.com/ethan4love/psxrecomp)
framework. It is not an emulator: the game's MIPS code is translated into C
and compiled to native ARM64.

**This repository and the APK it builds contain nothing from the game.** On
first launch the app asks for your own disc image, translates the game's
program into C on the phone (the recompiler is built into the app), and
compiles that translation with a bundled [TinyCC](https://bellard.org/tcc/)
each time the game starts.

## Features

- Setup screen: pick your disc image (`.bin` of a `.bin/.cue` dump, or `.chd`).
  The program on it is checked against the version this port supports.
- Widescreen (more view, nothing stretched), on by default at its widest,
  1.86:1: as far as the game's scenery reaches. Phones wider than that (most
  are about 19.5:9) fill the rest by stretching the picture slightly or by
  zooming, or keep black bars; the title screen and movies stay 4:3.
- On-screen PlayStation controls that hide while you use a controller. Every
  control, each face button included, can be moved and resized (Controls >
  Move and resize controls), and reset to the default layout.
- Bluetooth controllers (Xbox, PlayStation, ...), picked up at any time.
- An in-game menu, open with the menubutton, Android's back gesture,
- or Select+Start on a pad:
  - **Game**: 12 save-state slots, game speed 0.5x-4x, fast-forward, FPS counter
  - **Graphics**: widescreen aspect, fill screen, internal resolution,
    filtering, colour model
  - **Audio**, **Controls**
  - **Enhancements**: the port's mods (trainer, longer life, training
    multiplier, lucky reels, guaranteed drops, 52 extra raisable Digimon, ...)
    plus this port's own: Hide Window Outlines (on by default), which skips
    the grey wireframe boxes the game flashes when a text window opens or closes
  - **Cheats**: 57 GameShark codes, toggled live
  - **Partner**: view and edit your Digimon's hidden stats, care values, age
    and lifespan, plus bits and merit

## Installing

Download `DigimonWorld-<version>.apk` from the
[Releases](https://github.com/Cogential/digimon-world-android/releases) page
and open it on your phone (allow installing from your browser or file manager
when Android asks). The APK is for 64-bit ARM phones (Android 12 or newer).
Updates install over the previous version and keep your saves and settings.

On first launch, pick your disc image. Translating it takes a few seconds and
happens once; after that the app goes straight to the game.

## Building

Requirements: Linux, Git, CMake 3.22+, Ninja, Python 3, a JDK (17), and the
Android SDK in `~/Android/Sdk` with build-tools 35.0.1, platform android-35,
NDK 28.2.13676358 and CMake 3.31.6 (override with `ANDROID_HOME` /
`ANDROID_NDK`).

```sh
./setup.sh            # fetch upstream at pinned commits, apply patches
android/build_apk.sh  # -> android/out/DigimonWorld.apk (arm64-v8a)
```

No disc is needed to build. `ABIS="arm64-v8a x86_64"` adds the emulator ABI;
`DEBUG_TOOLS=ON` adds the runtime's TCP debug server (much slower, for testing).
The first build creates a signing key in `android/keystore/`; keep it, since
updates must be signed with the same key to install over an existing copy.

## How it works

| Piece | Where |
|---|---|
| Android app (setup screen, game activity, packaging) | `android/` |
| Menu and touch controls (Dear ImGui over SDL3) | `android/native/` |
| Android platform layer, dynamic game loading, in-app translation | `patches/psxrecomp-android.patch` |
| TinyCC: resolve host symbols in `-nostdlib` memory compiles | `patches/tinycc-resolve-host-symbols.patch` |
| This port's mod package (guarded code patches) | `android/mods/` |

The psxrecomp patch adds:

- `PSX_DYNAMIC_GAME`: builds the runtime without game code. At startup it
  compiles `generated/*.c` in memory with libtcc and binds the five game
  dispatch entry points to it (`psx_dynamic_game.c`).
- `psx_translate.cpp`: extracts the boot executable from the disc image,
  verifies it, and runs psxrecomp-game in-process.
- Android support: app data directory, logcat, a 64 MB game thread, an ARM64
  and x86-64 fiber switch (bionic has no `swapcontext`), atomic memory-card
  writes, controller handling, and hooks for the in-game overlay.

On the phone, the data folder is `Android/data/com.cogent.digimonworld/files/`
(saves in `saves/card1.mcd`, log in `log.txt`).

## Licences

PSXRecomp is PolyForm Noncommercial 1.0.0, so this port is for noncommercial
use only. Dear ImGui (MIT) is vendored in `android/native/imgui`. TinyCC
(LGPL), SDL3 (zlib), libchdr (BSD) and OpenBIOS (MIT) are fetched at build
time. Digimon World is (c) Bandai; you need your own copy of the game.
