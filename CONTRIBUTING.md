# Contributing

Thanks for helping! These rules keep the project legally safe and keep multiplayer working.

## Hard rules

1. **No game files.** Never commit executables, DLLs, `.rez` archives, extracted assets, maps, textures, sounds, video, or configs copied from the game. The `.gitignore` blocks the common ones. Don't work around it.
2. **No decompiled code dumps.** Don't paste Ghidra/IDA pseudocode or disassembly listings into the repo. Write original code from your understanding of the behavior, and document behavior in your own words. Function names, struct layouts, offsets, addresses and protocol descriptions are fine, and they're encouraged.
3. **No cracks or DRM bypasses.** Don't include, link to, or depend on no-CD patches. Assume contributors and users own the game.
4. **Don't break multiplayer compatibility.** Anything that changes what goes over the wire, or that touches the game modules (`CShell.DLL`, `Object.LTO`) or verified data, needs a design discussion in an issue first.
5. **Leaked or uncertain-license source** (e.g. the LithTech Jupiter "GPL" mirrors) is for reference only. Don't copy code from it.

## Target builds

Reverse-engineering work targets these exact retail builds. Hashes will be listed in `docs/hashes.md`:
- Aliens versus Predator 2, English, **1.0.9.6**
- Aliens versus Predator 2: Primal Hunt (latest retail)

Say which build an address or offset refers to.

## Workflow

- Open an issue before starting large work, so effort isn't duplicated.
- Keep pull requests focused. Include how you tested, and for networking changes, which stock client/server you tested against.
- Python tools: standard library plus `pefile`, Python 3.10+.
- C/C++ code (renderer, hooks): 32-bit Windows target (the game is x86), built with CMake + MSVC 2022 or clang-cl.
