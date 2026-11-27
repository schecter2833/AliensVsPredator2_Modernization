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
- [x] Capture the Primal Hunt launcher's `avp2cmds.txt` (exact `-rez` list); retest the Zeta Site load crash
- [x] Fix the Zeta Site load crash (bug in PH `object.lto`, fixed in memory by `src/compat/game_fixes.c`)
- [x] Run Primal Hunt with the D3D11 renderer (model pointer at +0x1FC)
- [x] PH menus: background images fill the screen; 3D character portrait
- [x] PH Zeta Site intro: stretched character heads (repeated vertex weights in the model data; normalised when skinning)
- [ ] Diff the AVP2 vs Primal Hunt engine interfaces (function tables, struct sizes)

## Phase 1: Replacement renderer (`d3d.ren`)
Client-side only, so no effect on multiplayer.
- [x] Map the `RenderDLLSetup` slot table: 38 slots at 0x70–0x104, identical layout in AVP2 and Primal Hunt ([renderer-interface.md](renderer-interface.md))
- [x] Name each slot from behavior; map the init contract (version `0xD5D`), mode record and engine-side fields
- [x] Clean-room header: `src/renderer/render_struct.h`
- [x] Logging proxy renderer (`src/trace/`, `avp2m_trace.ren`) for both games
- [x] Collect trace logs from AVP2 and Primal Hunt; confirm slots 2, 3, 5, 6, 33 ([runtime-findings.md](runtime-findings.md))
- [ ] Decide how to serve `GetHook("LPDIRECTDRAW")` / `GetHook("BACKBUFFER")` (find the feature that uses them)
- [ ] Survive repeated Term/unload/reload on focus loss; make alt-tab painless
- [ ] Own window placement in windowed mode (stock engine puts the window mostly off-screen on wide displays)
- [x] Reverse `GetSupportedModes` / `FreeModeList` and how the engine enumerates `*.ren`
- [x] **Milestone 1:** Direct3D 11 renderer drawing all 2D (menus, HUD, fonts, fades), with vsync and correct window placement (`src/renderer/d3d11`)
- [ ] **Milestone 2:** 3D world in D3D11 (`RenderScene`: world geometry, lightmaps, models, sprites, particles, effects)
- [ ] Video playback (the engine asks for DirectDraw hooks)
- [ ] Native widescreen and Hor+ FOV. Note: the game's menu only offers 4:3 modes (filter in `CShell.DLL`), so widescreen needs camera FOV help from the hook layer
- [ ] Arbitrary resolutions, high refresh rates
- [ ] Correct HUD/menu scaling at 16:9 and 21:9
- [x] Borderless fullscreen with aspect-correct scaling; window centered correctly on wide displays
- [ ] Gamma that doesn't change the desktop; smoother alt-tab

## Phase 1b: Modern graphics and mod support
Our renderer owns every pixel, so graphics upgrades can live there without touching game files (multiplayer stays compatible).
- [ ] Texture replacement: load high-res PNG/DDS overrides from a `mods/textures/` folder, keyed by the original texture name, with the stock texture as fallback
- [ ] Mod load order: several mod folders stacked, later folders win
- [ ] Shader-based lighting: per-pixel lightmap filtering, gamma-correct blending, optional bloom and tonemapping
- [ ] Anti-aliasing (MSAA, plus an optional post-process AA), anisotropic filtering, and supersampling
- [ ] Normal and specular map support for replacement textures
- [ ] User shader hooks (a ReShade-style post-process chain, loaded from files)
- [ ] Model replacement (higher-poly models) once the model format is mapped
- [ ] Config file for all of the above, plus an in-game toggle

## HDR lighting
The renderer is set up so lighting can grow past the original 0–255 range without clipping.
- [x] HDR scene target: with `avp2m_hdr 1` the 3D scene renders into a 16-bit float target and is tone mapped (ACES or Reinhard, with exposure) before the HUD, which stays untouched.
- [ ] Linear-space lighting: convert textures and light values to linear before shading (the game's art is authored in display/gamma space).
- [ ] Overbright light sources: let lightmaps, the light grid and dynamic lights exceed 1.0 (muzzle flashes, flares, fires) instead of clamping.
- [ ] Bloom from the bright parts of the HDR image, and eye adaptation (auto exposure) for dark corridors versus bright outdoor areas.
- [ ] HDR display output (scRGB / HDR10 swap chain) on monitors that support it.
- [ ] Per-pixel dynamic lights with normal maps from texture packs.

## Rust
New components get written in Rust where it fits; the existing C renderer gets ported once its 3D contract is stable.
- Every engine boundary is a plain C ABI (`RenderDLLSetup`, cdecl slot tables), so a Rust `cdylib` built for `i686-pc-windows-gnu` or `-msvc` can replace any C component one at a time.
- Order:
  1. Master server: standalone, runs anywhere, no 32-bit constraint, so it's a good first Rust component.
  2. Renderer: port `avp2m_d3d11.c` to Rust (`windows` crate for D3D11) after Milestone 2. Today it's one ~1,500-line file, so the port is cheap. Porting it now would mean doing the 3D reverse engineering twice.
  3. Hook layer and dedicated server: Rust from the start.
- The clean-room header (`render_struct.h`) becomes a `#[repr(C)]` module with the same size checks.
- Builds go through CI (GitHub Actions on Windows), since the 32-bit Windows toolchains are what the game needs.

## Phase 2: Engine hook layer
An injected DLL that patches `lithtech.exe` at runtime. The files on disk are never touched.
- [ ] Frame limiter: the stock engine runs uncapped (2000+ fps in menus, ~900 fps in game)
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
- Shipping texture packs ourselves (we provide the loader; packs come from the community)
