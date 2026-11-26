# Roadmap

The guiding rule: **never modify the original game modules or data** (`CShell.DLL`, `Object.LTO`, `.rez` content). Multiplayer servers verify them, and keeping them stock is what lets this project's players join existing 1.0.9.6 servers.

## Phase 0: Baseline and research
- [x] Inventory the retail binaries (imports, exports, RTTI). See [FINDINGS.md](FINDINGS.md).
- [x] REZ archive reader (`tools/rez.py`)
- [ ] Reference install: AVP2 1.0.9.6 + SP/MP map updates + Primal Hunt
- [ ] Record SHA-256 hashes of every 1.0.9.6 and Primal Hunt binary (`docs/hashes.md`), so contributors can confirm they're analyzing the same build
- [ ] Ghidra project setup guide, with shared symbol/type exports (names and types only, no decompiled code)
- [ ] Document how the Primal Hunt binaries differ from AVP2 1.0.9.6

## Phase 1: Replacement renderer (`d3d.ren`)
Client-side only, so no effect on multiplayer.
- [ ] Reverse the `RenderDLLSetup` / `GetSupportedModes` / `FreeModeList` interface and the render-struct function table it fills in
- [ ] Stub renderer that loads and lets the engine boot (logging every call)
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
