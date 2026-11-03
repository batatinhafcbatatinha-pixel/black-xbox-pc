# Black — Native PC Port (early work) | Original Xbox Static Recompilation

> [!WARNING]
> This repository does **not** include the game, an ISO/XBE, EA assets, generated game code or a precompiled executable.
> You need your own original-Xbox USA disc. See [the full notice](#notice) at the end.

Early work on a port of the original-Xbox game *Black* (USA), built through ahead-of-time translation of the game's x86 code, following the approach of the [FIFA Street (2005) port](https://github.com/batatinhafcbatatinha-pixel/fifa-street-pc). **This public repository is a work-in-progress source and local-build kit, not a ready-to-play game download.** The game's code is translated locally from your own disc; no game executable or generated game code is distributed here.

## How to play

> [!IMPORTANT]
> **There is no ready-to-play download.** This page has only tools: they turn *your own* game disc into a PC game,
> on your computer. The first time takes a while (30-90 minutes on a recent PC), mostly waiting.
> After that, you just double-click to play.

**You need**

- **Windows 10 or 11** (64-bit) with about **17 GB** free. (Linux and Mac: see below.)
- Your own **Black** (original Xbox, USA), as a disc image (`.iso`) made from your disc, or its extracted
  files. Other versions are not accepted; the setup checks this for you.

**Steps**

1. **Download**, one of these two ways:
   <details open><summary><b>Easiest: the ZIP</b></summary>

   Open [Releases](https://github.com/batatinhafcbatatinha-pixel/black-xbox-pc/releases/latest), download **Source code (zip)** and extract it to a short folder, such as
   `C:\Games\black-xbox-pc`.
   </details>
   <details><summary><b>With Git: always the newest version</b></summary>

   ```powershell
   git clone --recursive https://github.com/batatinhafcbatatinha-pixel/black-xbox-pc.git C:\Games\black-xbox-pc
   ```
   To update later: `git pull --recurse-submodules`, then run `Setup.cmd` again.
   </details>

2. **Double-click `Setup.cmd`.** It asks before installing anything, then:
   - installs Python and Microsoft's C++ Build Tools if you don't have them (free; Windows asks for permission once);
   - downloads the other build tools (about 1 GB) **into this folder only**;
   - opens a window where you **pick your `.iso`**: it checks the version and copies the game files;
   - builds your PC version, and offers a desktop shortcut.

   If anything stops, run `Setup.cmd` again: finished steps are skipped.

3. **Play:** double-click **`Play-Black.cmd`** (or the desktop shortcut).

**In the game:** **F1** opens the PC settings. Controllers and the keyboard work: see [Controls](#controls).

**Problems?** Open an [issue](https://github.com/batatinhafcbatatinha-pixel/black-xbox-pc/issues) with `setup-log.txt` and a screenshot. Never attach game files.

`Setup.cmd` is new: so far it has been tried from a fresh download only with the FIFA Street 3 port. If it stops for you, please open an issue with `setup-log.txt`.

<details><summary><b>Linux and Mac</b></summary>

- **Linux:** works natively (no Wine), but the setup is by hand for now: see [Linux](#linux).
- **Mac:** not playable yet. The Mac build files are an experiment that has never been compiled or run on a Mac. Details in [MACOS.md](MACOS.md); if you have a Mac and want to help test, open an issue.

</details>

<details><summary><b>Questions</b></summary>

- **Is this legal?** This page contains no game, only tools. You use the game you already own.
- **Can I share my built game?** No: it contains the game's code. Keep it on your PC.
- **Will it run on my PC?** Any graphics card with DirectX 11 should work. Measured: mission 1 runs at 60 fps on a Ryzen 5 3600 + RTX 3050 Laptop.
- **How do I remove it?** Delete the folder. Python and the Build Tools can be removed in Windows Settings > Apps.

</details>

## Status

- **Boots and plays natively, no NV2A/APU emulation.** Title, menus, briefings and mission 1 (Veblensk) run at
  60 fps on Direct3D 11 or Vulkan: world geometry, textures, lighting, weapons, particles, decals, HUD, enemies,
  pickups, death/restart and the pause menu were checked. Later missions have not been played through yet.
- The original D3D8 (linked with LTCG) still runs on the CPU, but its hardware boundary is native: device creation,
  push-buffer kick-off and fences take the library's own GPU-less path, and the D3D11 renderer draws every call.
  Methods the game writes straight into the push buffer (inlined setters, RenderWare's immediate-mode particles and
  post passes) are read back and drawn natively.
- DirectSound runs natively through XAudio2; controller input replaces XInput/XPP without the USB stack.

## Controls

### Controller

Xbox pads use XInput. On Windows, `SDL3.dll` next to the executable adds every other pad SDL knows: DualShock 3/4, DualSense, Switch pads, USB adapters for PS2 and original Xbox pads. `tools/build_local.py` downloads it with `gamecontrollerdb.txt` (both zlib license); `tools/fetch_pad_support.py` does the same for a manual build. Without those files the port behaves as before. No PlayStation pad or adapter has been tried on real hardware yet. **Controller vibration** in the F1 settings turns the pad's vibration off.

In gameplay the controls follow recent Call of Duty / Battlefield games; on the game's own pages (menus, pause,
briefings, "press A to continue" prompts) the pad works exactly as the game's prompts say.

| Action | Controller | Keyboard / mouse |
|---|---|---|
| Fire | RT | Left button |
| Aim (hold) | LT | Right button |
| Grenade | RB | G |
| Use health pack | LB / D-pad down | Q / H / 4 |
| Reload (hold: pick up weapon) | X | R |
| Pick up weapon / interact | A | F / E |
| Crouch | B | C / Left Ctrl |
| Melee | R3 | V / middle button |
| Next / previous weapon | Y / D-pad left | 2, wheel down, mouse 5 / 1, wheel up, mouse 4 |
| Firing mode | D-pad up | X / B |
| Suppressor | D-pad right | T |
| Next music track | L3 | M |
| Pause / back | Start / Back | Esc / Tab |
| Move / look | Sticks | W A S D / mouse |

Menus on the keyboard: arrows or W A S D, Enter/Space = confirm, Esc = back. `black-pc.ini`, section `[controles]`:
`esquema=classico` restores the original Xbox layout, `mira=segurar` passes LT straight to the game's aim button
(default `alternar` emulates hold-to-aim on the game's toggle), `sensibilidade_mouse=1.0`, `inverter_mouse=1`.

### PC settings

Open it with **F1**, **Back + Start** on a controller, or **Back / Backspace / a click** on the "BACK PC SETTINGS"
button shown on the game's own menus (title, main menu, options, pause). The game pauses while it is open.
Settings are saved to `black-pc.ini` next to the executable. Defaults: borderless fullscreen at the monitor's
resolution and refresh rate, vsync on, no frame limit (the game's clock follows real time), internal resolution =
monitor resolution (no upscaling), 16x anisotropic filtering, Direct3D 11. **Graphics API** switches to **Vulkan**
(DXVK `d3d11.dll`/`dxgi.dll` in `vulkan\` next to the executable; the build copies them from `third_party/dxvk/`).

## Your game files

```text
default.xbe  df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6
```

Do not upload the ISO, extracted files, generated code or the built executable.

## Building by hand

`Setup.cmd` ([How to play](#how-to-play)) does all of this on Windows. The steps below are for doing it yourself, or on Linux. `Prepare-Build.cmd` does only the preparation (tools, your game, the function lists and the translation from your disc), if you want to compile by hand afterwards.

### Windows

Requirements: Python 3.11+ with [Capstone](https://pypi.org/project/capstone/) (`pip install -r requirements.txt`), Visual Studio 2022 Build Tools (C++ workload, Windows SDK), LLVM/Clang (`clang-cl`), CMake and Ninja, in a Developer PowerShell (or after `vcvars64.bat`). Clone with `git clone --recursive`, or run `git submodule update --init`, for the `xboxrecomp/` toolkit.

```powershell
python -m pip install -r requirements.txt
Copy-Item game_files\default.xbe .
python scripts\native_pipeline.py generate     # translates your executable into project/src/recomp/gen
$env:BLACK_BUILD_JOBS = 4
python scripts\native_pipeline.py build        # recovers boot functions, lowers native code, compiles
.\Play-Black.cmd
```

The executable goes to `project\out\black_native.exe`. Generated code and the build stay on your PC: do not upload them. Vulkan in the PC settings needs DXVK's `d3d11.dll` and `dxgi.dll` in `third_party/dxvk/` before building (they are copied to `vulkan\` next to the executable).

### Linux

The same sources build natively on Linux (x86-64): the window, input and audio are SDL2 and the renderer's D3D11
runs on Vulkan through [DXVK native](https://github.com/doitsujin/dxvk), with vkd3d-shader for shaders, as in the
FIFA Street port. Tested under WSL2 (Ubuntu 24.04, WSLg) from boot to mission 1; a desktop with a real Vulkan
driver has not been tried yet.

- Packages (Ubuntu/Debian): `clang cmake ninja-build python3 pkg-config libsdl2-dev libsdl2-ttf-dev fonts-dejavu-core`
  and a Vulkan driver (`mesa-vulkan-drivers` or your GPU vendor's). The translation scripts need
  [capstone](https://pypi.org/project/capstone/): `python3 -m venv ~/black-py && ~/black-py/bin/pip install capstone`,
  then run the scripts below with `PYTHON=~/black-py/bin/python` (or that interpreter directly).
- DXVK native (a release's `usr/` with `include/dxvk` and `lib/libdxvk_{d3d11,dxgi}.so`) and vkd3d (built with
  `libvkd3d-shader.so`). The defaults are `../third_party/dxvk-native/usr` and `../third_party/vkd3d-inst`
  next to this folder; otherwise point at them with `BLACK_CMAKE_ARGS`.
- Translate the game code (`~/black-py/bin/python scripts/native_pipeline.py generate`, with your `default.xbe`
  here), then `./build-linux.sh`:

```sh
PYTHON=~/black-py/bin/python BLACK_CMAKE_ARGS="-DDXVK_NATIVE_DIR=/opt/dxvk-native/usr -DVKD3D_DIR=/opt/vkd3d" ./build-linux.sh
./Jogar-Black.sh
```

The binary goes to `project/out-linux/` with DXVK and vkd3d in `lib/` next to it and `black-pc.ini` beside it.
PC Settings, the controls and the test options above work the same way (on Linux the Graphics API is always Vulkan).

### macOS

Experimental and not compiled or run yet: see [MACOS.md](MACOS.md).

## For developers

`BLACK_SKIP_MOVIES=1` ends every pre-rendered movie at once (F10 or A skips one in normal play);
`RECOMP_PAD_SCRIPT` / `RECOMP_PAD_LIVE` drive the pad; `RECOMP_FB_DUMP=<prefix> RECOMP_HLE_DUMP_EVERY=<n>` save frames.
Diagnostics (`BLACK_GPU_TRACE_EVERY`, `BLACK_PB_DUMP`, `BLACK_VSH_DUMP`, `BLACK_WATCH`, `BLACK_MEMSNAP`) are
not documented here yet.

## Why a native port instead of an emulator

An emulator such as [xemu](https://github.com/xemu-project/xemu) recreates the Xbox itself: it interprets or JIT-translates the console's CPU code, and models the NV2A GPU, the MCPX audio DSP and the USB controllers register by register. That is the right approach for running the whole Xbox library accurately. This port takes a different route for one game:

- **Ahead-of-time code.** The game's x86 code is translated into C and compiled by a modern optimizing compiler before you play.
- **No emulated hardware in the hot path.** Direct3D calls are drawn by a native Direct3D 11 renderer, DirectSound plays through XAudio2 and the controller is read directly, instead of being lowered to push-buffer commands, DSP microcode and USB packets that are then emulated again.
- **PC-side improvements in one place.** Resolution, frame rate, anisotropic filtering, modern controls, mouse aiming and the in-game settings menu are part of the port.
- **Readable and fixable.** Every native layer is plain C source in this repository.

The trade-off: it covers only this game and revision, and it is early work. An emulator is the more general and better-tested option.

## What is in this repository

- `scripts/` — ISO inspection/extraction, game-file verification, analysis pipeline, boot/leaf/missing-function recovery, native-instruction lowering and GPU wrapper generation.
- `config/` — XBE configuration and runtime targets.
- `project/` — CMake project, integration code, the native D3D/DirectSound/input replacements (`src/native_d3d/`) and the native renderer with PC Settings (`src/native_gpu/`); translated game code (`src/recomp/gen`) is not included.
- [NOTES.md](NOTES.md) — development log (in Portuguese).
- `xboxrecomp/` — the toolkit, as a submodule: [batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp),
  branch `ports`. Clone with `git clone --recursive` (or run `git submodule update --init` afterwards). Without the
  submodule, the scripts and CMake fall back to an `xboxrecomp` checkout next to this folder.

## Credits and licenses

- Port integration and native layers: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp): Ned Heller / sp00nz and contributors (MIT); the modified copy these ports use is the fork [batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp) (branch `ports`, on top of v0.13.1). The fork also carries xemu's DSP sources (GPL-2.0/LGPL-2.1).
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase), used locally to locate XDK library functions.
- [DXVK](https://github.com/doitsujin/dxvk) (zlib license) for the optional Vulkan path; its DLLs are not committed.
- Black, its code, trademarks and assets belong to Electronic Arts / Criterion Games and the respective rights holders. This project is not affiliated with or endorsed by EA, Criterion or Microsoft.

---

<a id="notice"></a>

**THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XBE, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS.**
