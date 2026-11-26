# Findings

Reference builds: **AVP2 English 1.0.9.6** (with SP/MP Map Update #1) and **Primal Hunt** retail. Exact hashes are in [hashes.md](hashes.md).

## Two engine builds, not one

Primal Hunt installs to **its own folder** with its own, newer engine build. So the project has to support two sets of engine binaries:

| Binary | AVP2 1.0.9.6 (link date) | Primal Hunt (link date) |
|---|---|---|
| `lithtech.exe` | 937,984 B (2002-04-17) | 1,007,616 B (2002-05-20) |
| `d3d.ren` | 348,160 B (2002-02-07) | 364,544 B (2002-06-03) |
| `server.dll` | 557,056 B (2002-04-17) | 610,304 B (2002-05-07) |
| `CShell.DLL` (RTTI classes) | 2,084,864 B (277) | 2,183,168 B (284) |
| `Object.LTO` (RTTI classes) | 3,198,976 B (444) | 3,375,104 B (471) |

Both builds export the same interfaces (same export names on the renderer, server and game modules), and both renderers use Direct3D 7 via `DDRAW.dll`. That makes it likely that one replacement renderer and one hook layer can serve both games, with a small table of per-build offsets. This still needs to be confirmed by comparing the `RenderDLLSetup` function tables.

Primal Hunt also ships `cdaudio.dll`, localized `Su*.dll` resource DLLs, and its own `AVP2X.REZ` / `DIALOGUE.REZ` content.

The 1.0 retail binaries below were analyzed first. The interface conclusions carry over to 1.0.9.6.

## Binary layout

| File | Role | Notes |
|---|---|---|
| `lithtech.exe` | Engine / client host | MSVC 6.0, no PDB. Imports DINPUT, WSOCK32, WINMM, mss32 (Miles). 16 RTTI classes. |
| `d3d.ren` | Renderer DLL | Direct3D 7 via `DDRAW.dll` (queries `IID_IDirect3D7`). Exports: `RenderDLLSetup`, `GetSupportedModes`, `FreeModeList`. |
| `server.dll` | Server engine | Exports `CreateServer`, `DeleteServer`. Imports WSOCK32. |
| `AVP2Serv.exe` | Dedicated server GUI | MFC app that loads `server.dll`. |
| `CShell.DLL` (in `AVP2DLL.REZ`) | Client game code | Exports `GetClientShellFunctions`, `GetClientShellVersion`, `SetInstanceHandle`. **277 RTTI classes.** |
| `Object.LTO` (in `AVP2DLL.REZ`) | Server game code | Exports `GetServerShellFunctions`, `GetServerShellVersion`, `ObjectDLLSetup`, `SetInstanceHandle`. **444 RTTI classes.** |
| `mss32.dll` | Miles Sound System | Third-party; 89 imports from `lithtech.exe`. |
| `binkw32.dll`, `Smackw32.dll` | Video playback | Third-party (RAD). |

All first-party binaries were built with **MSVC 6.0** (linker 6.0) and ship without debug symbols. The RTTI type descriptors in the game DLLs give us class names, and through vtables, most of the class hierarchy.

## Interfaces

- **Renderer:** the engine loads `*.ren`, calls `RenderDLLSetup`, and gets back a structure of function pointers. Replacing this DLL is the cheapest way to modernize graphics and doesn't touch networking at all.
- **Game modules:** the engine gets the client and server shell interfaces through the `Get*ShellFunctions` exports. As long as we provide compatible engine-side interfaces, the stock game modules run unmodified.

## Networking

- The engine, `server.dll` and `CShell.DLL` all carry GameSpy support strings (`WS_GameSpySupport_*`, `WS_PeerAuthClient_*`) plus a `gamename` key. GameSpy's servers shut down in 2014, so the in-game server browser needs a replacement master server.
- The 1.0.9.6 patch "made the multiplayer file verification much more thorough." So clients have to keep stock game modules and attribute files to join stock servers.

## REZ archives

`tools/rez.py` reads LithTech REZ v1 archives: a fixed text header, then a version, root directory position and size, and a tree of directory/resource entries. Each resource entry stores a reversed 4-character extension. The retail install contains `AVP2.REZ`, `AVP2DLL.REZ`, `AVP2L.REZ`, `ALIEN.REZ`, `MARINE.REZ`, `PREDATOR.REZ`, `MULTI.REZ` and `SOUNDS.REZ`.

## Prior community work (for reference)

- **LithFix** (haekb): mouse-look and frame-limiter fixes for LithTech games, including AVP2 Primal Hunt.
- **AVP2 Custom Launcher** (SuiMachine): a widescreen/FOV fix done by patching memory.
- **LithTech "Jupiter" GPL source** (several GitHub mirrors): a later engine generation. Its GPL release is of uncertain provenance, so use it for **structural reference only**. Don't copy code from it into this repo without a clear decision from maintainers.
- **lithtools** (haekb): a viewer for DTX/ABC/DAT files.
