# AliensVsPredator2_Modernization

A community project to modernize **Aliens versus Predator 2** (Monolith, 2001) and its expansion **Primal Hunt** (Third Law, 2002) for current PCs. We do this by reverse-engineering and replacing the LithTech Talon engine one piece at a time, without breaking compatibility with the existing 1.0.9.6 multiplayer community.

> **Status: early research.** Nothing is playable yet. See [docs/ROADMAP.md](docs/ROADMAP.md) for the plan and [docs/FINDINGS.md](docs/FINDINGS.md) for what we've learned so far.

## Goals

- Run well on Windows 10/11 with modern GPUs: native widescreen, high resolutions and refresh rates, stable alt-tab.
- Fix long-standing engine problems: physics that break at high frame rates, mouse look, and input.
- **Keep multiplayer working.** Stay wire-compatible with stock 1.0.9.6 clients and servers, and replace the dead GameSpy master server.
- Support both **AVP2** and **Primal Hunt**.
- Long term: an open replacement for the engine (`lithtech.exe` / `server.dll`) that loads the original game modules, plus a Linux dedicated server.

## How it works

AVP2 is modular. The game logic lives in `CShell.DLL` (client) and `Object.LTO` (server/gameplay), packed inside `AVP2DLL.REZ`. These talk to the engine through small function-table interfaces. The renderer is a separate DLL (`d3d.ren`, Direct3D 7) with only three exports.

That means we can replace the **engine side** a component at a time and leave the original game modules untouched. Multiplayer servers verify those modules and the game data, so leaving them alone is what keeps online play working.

## You need your own copy of the game

This repository contains **no game files**: no executables, DLLs, `.rez` archives, maps, textures or sounds. You need a legitimately owned copy of AVP2 (and Primal Hunt, if you want it), patched to **1.0.9.6**.

## Repository layout

| Path | Contents |
|---|---|
| `tools/` | Research utilities (REZ archive extractor, PE inventory) |
| `src/renderer/` | Replacement for `d3d.ren` (planned) |
| `src/hooks/` | Engine fix and hook layer (planned) |
| `docs/` | Roadmap, findings, file-format notes |

## Tools

```sh
pip install pefile
python tools/rez.py "C:/Games/AVP2/AVP2DLL.REZ"            # list contents
python tools/rez.py "C:/Games/AVP2/AVP2DLL.REZ" extracted/  # extract (git-ignored)
python tools/pe_inventory.py lithtech.exe d3d.ren server.dll
```

## Contributing

Help is welcome, especially from people who know reverse engineering, Direct3D/Vulkan, the LithTech engine, or the AVP2 multiplayer protocol. Please read [CONTRIBUTING.md](CONTRIBUTING.md) first. It has rules about game files and decompiled code that protect the project.

## License

GPL-3.0. See [LICENSE](LICENSE).

*Aliens vs. Predator 2 is a trademark of its respective owners. This project is not affiliated with or endorsed by Monolith Productions, Fox Interactive, Sierra, Disney or Warner Bros.*
