# Roadmap

The guiding rule: **never modify the original game modules or data** (`CShell.DLL`, `Object.LTO`, `.rez` content). Multiplayer servers verify them, and keeping them stock is what lets this project's players join existing 1.0.9.6 servers.

## Phase 0: Baseline and research
- [x] Inventory the retail binaries (imports, exports, RTTI). See [FINDINGS.md](FINDINGS.md).
- [x] REZ archive reader (`tools/rez.py`)
- [x] Reference install: AVP2 1.0.9.6 + SP/MP map updates + Primal Hunt
- [x] Record SHA-256 hashes of every 1.0.9.6 and Primal Hunt binary ([hashes.md](hashes.md)), so contributors can confirm they're analyzing the same build
- [x] Reverse-engineering setup guide: REA + Ghidra 12.2 ([RE-SETUP.md](RE-SETUP.md))
- [ ] Shared symbol/type exports (names and types only, no decompiled code)
- [x] Identify that Primal Hunt uses its own engine build (see FINDINGS.md)
- [ ] Diff the AVP2 vs Primal Hunt engine interfaces (function tables, struct sizes)

## Phase 1: Replacement renderer (`d3d.ren`)
Client-side only, so no effect on multiplayer.
- [x] Map the `RenderDLLSetup` slot table: 38 slots at 0x70–0x104, identical layout in AVP2 and Primal Hunt ([renderer-interface.md](renderer-interface.md))
- [x] Name each slot from behavior; map the init contract (version `0xD5D`), mode record and engine-side fields
- [x] Clean-room header: `src/renderer/render_struct.h`
- [x] Logging proxy renderer (`src/trace/`, `avp2m_trace.ren`) for both games
- [ ] Collect trace logs from AVP2 and Primal Hunt; confirm tentative slots
- [x] Reverse `GetSupportedModes` / `FreeModeList` and how the engine enumerates `*.ren`
- [ ] Direct3D 11 backend (Vulkan later, or via DXVK)
- [ ] Native widescreen and Hor+ FOV, arbitrary resolutions, high refresh rates
- [ ] Correct HUD/menu scaling at 16:9 and 21:9
- [ ] Gamma that doesn't change the desktop, borderless windowed mode, alt-tab

## Phase 2: Engine hook layer
An injected DLL that patches `lithtech.exe` at runtime. The files on disk are never touched.
- [ ] Frame-time / physics fix for high FPS (movement, jumping and wall-walking break above ~60 FPS)
- [ ] Raw mouse input in place of DirectInput 7; fix mouse acceleration
- [ ] Controller support (XInput / SDL)
- [ ] **Master server:** redirect the GameSpy server list and heartbeat to a community-run master (OpenSpy or a new one written for this project)
- [ ] NAT-friendly hosting notes and port documentation

## Phase 3: Networking documentation
- [ ] Document the LithTech Talon client/server protocol from packet captures against stock 1.0.9.6
- [ ] Document the file-verification handshake added in 1.0.9.6
- [ ] Wireshark dissector (Lua) for the protocol
- [ ] Compatibility test harness: a stock client against our server, and our client against a stock server

## Phase 4: Open engine reimplementation
- [ ] Replacement `server.dll` (dedicated server) that hosts stock clients
- [ ] Replacement `lithtech.exe` that loads stock `CShell.DLL` / `Object.LTO` through the original interfaces
- [ ] Sound backend in place of Miles (`mss32.dll`), e.g. OpenAL Soft
- [ ] Linux dedicated server (the original 1.0.9.6 Linux server is a useful reference)
- [ ] 64-bit builds would need the game modules reimplemented too, so they're out of scope until much later

## Non-goals (for now)
- Rewriting the game logic in `CShell` / `Object.LTO` (it would break multiplayer verification)
- Redistributing or modifying any game assets
- High-res texture packs (community packs already exist; they're not engine work)
