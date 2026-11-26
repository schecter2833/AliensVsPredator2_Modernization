# Findings

Analysis of the **retail 1.0** English install (files dated 2001-10-09). Results for 1.0.9.6 and Primal Hunt are still to come and will be added here.

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
