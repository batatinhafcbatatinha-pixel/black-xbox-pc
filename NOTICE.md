# Redistribution and provenance

This is a source and tooling distribution. It contains no original game assets, no game executable (XBE), no generated translation units, no disassembly and no compiled game executable. Lists of function addresses and sizes describe where functions are, not what they contain. Code derived from the game's executable, such as `project/src/native_d3d/black_native_copies.c`, is produced on your machine by `scripts/native_pipeline.py` from your own disc and is never committed.

The independently written native integration (renderer, audio, input, PC settings) and tooling are offered under the root MIT license. This license grants no rights to Electronic Arts or Criterion game content.

External dependencies are downloaded or checked out separately and are not stored here: the `xboxrecomp` toolkit (submodule) and DXVK 3.1.1 (zlib license, `licenses/DXVK-LICENSE.txt`) for the Vulkan option. On Windows, SDL3 (zlib license) and SDL_GameControllerDB (zlib license) add non-XInput controllers when present next to the executable.

The publication audit (`tools/audit_publication.py`) uses an allowlist and rejects executables, images, audio, archives, game resources, generated code and oversized files. It is an engineering safeguard, not a legal guarantee for every jurisdiction or use. Do not submit game content or private build artifacts in pull requests or issues.
