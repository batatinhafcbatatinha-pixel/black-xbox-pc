# Black — Native PC Port (early work) | Original Xbox Static Recompilation

> [!WARNING]
> This repository does **not** include the game, an ISO/XBE, EA assets, generated game code or a precompiled executable.
> You need your own original-Xbox USA disc. See [the full notice](#notice) at the end.

Early work on a port of the original-Xbox game *Black* (USA), built through ahead-of-time translation of the game's x86 code, following the approach of the [FIFA Street (2005) port](https://github.com/batatinhafcbatatinha-pixel/fifa-street-pc). **This public repository is a work-in-progress source snapshot, not a ready-to-play game download or a self-contained build kit.** The game's code is translated locally from your own disc; no game executable or generated game code is distributed here.

## Status

- **Boots and plays natively, no NV2A/APU emulation.** Title, menus, briefings and mission 1 (Veblensk) run at
  60 fps on Direct3D 11 or Vulkan: world geometry, textures, lighting, weapons, particles, decals, HUD, enemies,
  pickups, death/restart and the pause menu were checked. Later missions have not been played through yet.
- The original D3D8 (linked with LTCG) still runs on the CPU, but its hardware boundary is native: device creation,
  push-buffer kick-off and fences take the library's own GPU-less path, and the D3D11 renderer draws every call.
  Methods the game writes straight into the push buffer (inlined setters, RenderWare's immediate-mode particles and
  post passes) are read back and drawn natively.
- DirectSound runs natively through XAudio2; controller input replaces XInput/XPP without the USB stack.

## Playing

`Jogar-Black.cmd` starts `project\out\black_native.exe` with `game_files\` (your extracted disc).

### PC Settings

Open it with **F1**, **Back + Start** on a controller, or **Back / Backspace / a click** on the "BACK PC SETTINGS"
button shown on the game's own menus (title, main menu, options, pause). The game pauses while it is open.
Settings are saved to `black-pc.ini` next to the executable. Defaults: borderless fullscreen at the monitor's
resolution and refresh rate, vsync on, no frame limit (the game's clock follows real time), internal resolution =
monitor resolution (no upscaling), 16x anisotropic filtering, Direct3D 11. **Graphics API** switches to **Vulkan**
(DXVK `d3d11.dll`/`dxgi.dll` in `vulkan\` next to the executable; the build copies them from `third_party/dxvk/`).

### Modern controls (default)

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

### Test options

`BLACK_SKIP_MOVIES=1` ends every pre-rendered movie at once (F10 or A skips one in normal play);
`RECOMP_PAD_SCRIPT` / `RECOMP_PAD_LIVE` drive the pad; `RECOMP_FB_DUMP=<prefix> RECOMP_HLE_DUMP_EVERY=<n>` save frames.
Diagnostics (`BLACK_GPU_TRACE_EVERY`, `BLACK_PB_DUMP`, `BLACK_VSH_DUMP`, `BLACK_WATCH`, `BLACK_MEMSNAP`) are
described in [CONTINUACAO.md](CONTINUACAO.md).

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

## What is in this repository

- `scripts/` — ISO inspection/extraction, game-file verification, analysis pipeline, boot/leaf/missing-function recovery, native-instruction lowering and GPU wrapper generation.
- `config/` — XBE configuration and runtime targets.
- `project/` — CMake project, integration code, the native D3D/DirectSound/input replacements (`src/native_d3d/`) and the native renderer with PC Settings (`src/native_gpu/`); translated game code (`src/recomp/gen`) is not included.
- [NOTES.md](NOTES.md) and [CONTINUACAO.md](CONTINUACAO.md) — development log and native plan (in Portuguese).
- `xboxrecomp/` — the toolkit, as a submodule: [batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp),
  branch `ports`. Clone with `git clone --recursive` (or run `git submodule update --init` afterwards). Without the
  submodule, the scripts and CMake fall back to an `xboxrecomp` checkout next to this folder.

## Your game files

```text
default.xbe  df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6
```

Do not upload the ISO, extracted files, generated code or the built executable.

## Credits and licenses

- Port integration and native layers: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp): Ned Heller / sp00nz and contributors (MIT); the modified copy these ports use is the fork [batatinhafcbatatinha-pixel/xboxrecomp](https://github.com/batatinhafcbatatinha-pixel/xboxrecomp) (branch `ports`, on top of v0.13.1). The fork also carries xemu's DSP sources (GPL-2.0/LGPL-2.1).
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase), used locally to locate XDK library functions.
- [DXVK](https://github.com/doitsujin/dxvk) (zlib license) for the optional Vulkan path; its DLLs are not committed.
- Black, its code, trademarks and assets belong to Electronic Arts / Criterion Games and the respective rights holders. This project is not affiliated with or endorsed by EA, Criterion or Microsoft.

---

<a id="notice"></a>

**⚠️ THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XBE, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS. ⚠️**
