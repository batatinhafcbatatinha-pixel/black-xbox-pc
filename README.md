# Black — Native PC Port (early work) | Original Xbox Static Recompilation

> [!WARNING]
> This repository does **not** include the game, an ISO/XBE, EA assets, generated game code or a precompiled executable.
> You need your own original-Xbox USA disc. See [the full notice](#notice) at the end.

Early work on a port of the original-Xbox game *Black* (USA), built through ahead-of-time translation of the game's x86 code, following the approach of the [FIFA Street (2005) port](https://github.com/batatinhafcbatatinha-pixel/fifa-street-pc). **This public repository is a work-in-progress source snapshot, not a ready-to-play game download or a self-contained build kit.** The game's code is translated locally from your own disc; no game executable or generated game code is distributed here.

## Status

- **Not playable yet.** The translated game library builds (13,358 functions plus 986 recovered call targets; 9 remaining aborts are disassembly noise outside executable sections).
- Boot passes partition setup, TDATA/UDATA and save metadata, then stops inside Direct3D device creation when the original D3D code touches NV2A hardware registers.
- Plan: replace D3D8's hardware boundary with the native Direct3D 8 → Direct3D 11 layer from the FIFA Street port (no NV2A/APU emulation). Black links D3D8 with LTCG, so part of the game writes GPU push-buffer methods directly and needs a native method decoder.
- The native D3D11 renderer sources are in `project/src/native_gpu/` but currently only observe the original D3D.

## What is in this repository

- `scripts/` — ISO inspection/extraction, game-file verification, analysis pipeline, boot/leaf/missing-function recovery, native-instruction lowering and GPU wrapper generation.
- `config/` — XBE configuration and runtime targets.
- `project/` — CMake project, integration code and the native GPU layer; translated game code (`src/recomp/gen`) is not included.
- [NOTES.md](NOTES.md) and [CONTINUACAO.md](CONTINUACAO.md) — development log and native plan (in Portuguese).

The scripts expect the author's local workspace (a neighboring `xboxrecomp` checkout).

## Your game files

```text
default.xbe  df2739c372d254a90aeeccf5971d12097f22d89fbb90db021fda96dd4deb68f6
```

Do not upload the ISO, extracted files, generated code or the built executable.

## Credits and licenses

- Port integration and native layers: [batatinhafcbatatinha-pixel](https://github.com/batatinhafcbatatinha-pixel).
- [xboxrecomp](https://github.com/sp00nznet/xboxrecomp): Ned Heller / sp00nz and contributors (MIT); the modified snapshot with its notices is published in [fifa-street-pc](https://github.com/batatinhafcbatatinha-pixel/fifa-street-pc).
- [XbSymbolDatabase](https://github.com/Cxbx-Reloaded/XbSymbolDatabase), used locally to locate XDK library functions.
- Black, its code, trademarks and assets belong to Electronic Arts / Criterion Games and the respective rights holders. This project is not affiliated with or endorsed by EA, Criterion or Microsoft.

---

<a id="notice"></a>

**⚠️ THIS PROJECT DOES NOT INCLUDE THE GAME, AN ISO/XBE, EA GAME ASSETS, GENERATED GAME CODE OR A PRECOMPILED GAME EXECUTABLE. YOU MUST PROVIDE YOUR OWN LAWFULLY OBTAINED GAME FILES AND HAVE THE RIGHTS REQUIRED FOR YOUR USE. THIS PROJECT DOES NOT AUTHORIZE COPYRIGHT INFRINGEMENT. ALL GAME RIGHTS BELONG TO THEIR RESPECTIVE OWNERS. ⚠️**
