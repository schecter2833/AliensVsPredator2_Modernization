# Renderer interface (`d3d.ren`)

Status: **slot layout confirmed for both builds and exercised at runtime in both games (see [runtime-findings.md](runtime-findings.md)). Names ending in `?` are still tentative.** The clean-room header is [`src/renderer/render_struct.h`](../src/renderer/render_struct.h). Runtime confirmation will come from the trace renderer ([`src/trace/`](../src/trace/)).

All addresses refer to the reference builds in [hashes.md](hashes.md). Analysis: Ghidra 12.2 via REA, plus objdump call-site scans of `lithtech.exe`.

## How the engine loads a renderer

1. **Mode enumeration.** The engine scans the game folder for `*.ren`. For each one it loads the DLL, calls `GetSupportedModes()` (no arguments, returns a linked list), and copies every mode record (0x214 bytes). It **overwrites each record's DLL-name field with that file's name**, then calls `FreeModeList(list)` and unloads the DLL. So any `.ren` file in the folder contributes its own modes.
2. **Selection.** `autoexec.cfg` stores the choice as `"RENDERDLL" "d3d.ren"`, plus `CARDDESC`, `SCREENWIDTH`, `SCREENHEIGHT` and `BITDEPTH`.
3. **Setup.** The engine loads the chosen `.ren` and calls `RenderDLLSetup(&render_struct)`. The renderer fills slots 0x70–0x104.
4. **Init.** The engine builds a 0x218-byte init block: a version field, then a copy of the chosen mode record. It calls slot 0 with it. Init must return 0 **and** write `0xD5D` into the version field, or the engine unloads the renderer and reports an error. **Both AVP2 1.0.9.6 and Primal Hunt check for `0xD5D`.**
5. The engine copies the requested width/height into the struct (+0x3C/+0x40), runs its own post-init steps, sets the initialized flag (+0x44), and logs `CardDesc`, `RenderDLL`, `ScreenWidth`, `ScreenHeight` and `BitDepth` to the console.

The render struct is a single global in `lithtech.exe`: `0x004E4888` in AVP2 1.0.9.6.

## Mode record (0x214 bytes)

| Offset | Field |
|---|---|
| 0x000 | flags (tentative: hardware) |
| 0x004 | renderer DLL name, 256 bytes |
| 0x104 | device name, 128 bytes (logged as `CardDesc`) |
| 0x184 | description, 128 bytes |
| 0x204 | width |
| 0x208 | height |
| 0x20C | bit depth |
| 0x210 | next record |

## Engine-side fields (0x00–0x6F)

| Offset | Meaning |
|---|---|
| 0x04–0x38 | 14 engine callbacks the renderer can call (the engine fills them before setup). The stock renderer uses callbacks at +0x18, +0x1C and +0x20 during Init; +0x1C also in `ReadConsoleVariables`, so it's probably a console-variable lookup. |
| 0x3C / 0x40 | Screen width / height (set by the engine after Init) |
| 0x44 | Renderer initialized flag (read at 26 places in the engine) |
| 0x58–0x68 | Linked into the engine's console-variable table |

## Renderer slots (0x70–0x104)

All slots are `__cdecl`. Both builds fill the same 38 slots, with nothing extra and nothing missing.

| # | Offset | Name | Evidence | AVP2 1.0.9.6 | Primal Hunt |
|---:|---|---|---|---|---|
| 0 | 0x070 | `Init` | Creates the DirectDraw/D3D device; writes version `0xD5D`; returns 0 on success | `0x10010B22` | `0x100118BE` |
| 1 | 0x074 | `Term` | Short teardown routine | `0x10010B13` | `0x100118AA` |
| 2 | 0x078 | `BindTexture` | Reads a texture record's data field | `0x10021B50` | `0x10022830` |
| 3 | 0x07C | `UnbindTexture` | Releases a texture record's data | `0x10021C60` | `0x10022940` |
| 4 | 0x080 | `LoadWorldData?` | Builds/pages in lightmaps | `0x1001B790` | `0x1001C3A0` |
| 5 | 0x084 | `CreateContext` | Allocates a 16-byte object | `0x1001B700` | `0x1001C310` |
| 6 | 0x088 | `DeleteContext` | Not yet characterized | `0x1001B770` | `0x1001C380` |
| 7 | 0x08C | `Clear` | Engine passes 3 arguments | `0x100142F0` | `0x10014D70` |
| 8 | 0x090 | `Start3D` | Sets the in-3D state | `0x1001BD80` | `0x1001C990` |
| 9 | 0x094 | `End3D` | Clears the in-3D state | `0x1001BDB0` | `0x1001C9C0` |
| 10 | 0x098 | `IsIn3D` | Returns the in-3D flag; engine checks it before 2D/3D calls (8 call sites) | `0x1001BE20` | `0x1001CA30` |
| 11 | 0x09C | `StartOptimized2D` | Requires a device; enters 2D mode | `0x1001C627` | `0x1001D237` |
| 12 | 0x0A0 | `EndOptimized2D` | Leaves 2D mode | `0x1001C725` | `0x1001D335` |
| 13 | 0x0A4 | `IsInOptimized2D` | Returns the in-2D flag | `0x1001C7EB` | `0x1001D3FB` |
| 14 | 0x0A8 | `SetOptimized2DBlend` | Stores a blend-mode selector | `0x1001C7F1` | `0x1001D401` |
| 15 | 0x0AC | `GetOptimized2DBlend` | Writes the stored blend mode to a pointer | `0x1001C8CA` | `0x1001D4DA` |
| 16 | 0x0B0 | `SetOptimized2DColor` | Stores a color with alpha forced to 0xFF | `0x1001C8B8` | `0x1001D4C8` |
| 17 | 0x0B4 | `GetOptimized2DColor` | Writes the stored color to a pointer | `0x1001C8DA` | `0x1001D4EA` |
| 18 | 0x0B8 | `RenderScene` | Large stack frame; the main scene draw | `0x10017AA0` | `0x10018520` |
| 19 | 0x0BC | `RenderCommand` | Takes argc/argv; console command handler | `0x1001B8A0` | `0x1001C4B0` |
| 20 | 0x0C0 | `GetHook` | Compares a string argument against hook names | `0x100109C6` | `0x1001175D` |
| 21 | 0x0C4 | `SwapBuffers` | Called once per frame | `0x1001E189` | `0x1001ED75` |
| 22 | 0x0C8 | `GetInfoFlags` | Stock renderer returns 0 | `0x1001BD70` | `0x1001C980` |
| 23 | 0x0CC | `GetScreenFormat` | Fills a pixel-format record | `0x1001DEC3` | `0x1001EAB3` |
| 24 | 0x0D0 | `CreateSurface` | Allocates a surface; out-of-memory message | `0x1001DA7F` | `0x1001E66F` |
| 25 | 0x0D4 | `DeleteSurface` | Frees a surface | `0x1001DB2D` | `0x1001E71D` |
| 26 | 0x0D8 | `GetSurfaceInfo` | Writes width/height to two pointers | `0x1001DB4E` | `0x1001E73E` |
| 27 | 0x0DC | `LockSurface` | Returns a pointer from a surface | `0x1001DB7E` | `0x1001E76E` |
| 28 | 0x0E0 | `UnlockSurface` | Releases a surface lock | `0x1001DBB4` | `0x1001E7A4` |
| 29 | 0x0E4 | `OptimizeSurface` | Builds an optimized copy; takes a transparent color | `0x1001C3E4` | `0x1001CFF4` |
| 30 | 0x0E8 | `UnoptimizeSurface` | Frees the optimized copy | `0x1001C3D2` | `0x1001CFE2` |
| 31 | 0x0EC | `LockScreen?` | Not yet characterized | `0x1001DBCA` | `0x1001E7BA` |
| 32 | 0x0F0 | `UnlockScreen?` | Not yet characterized | `0x1001DC4F` | `0x1001E83F` |
| 33 | 0x0F4 | `BlitToScreen` | Not yet characterized | `0x1001DE1C` | `0x1001EA0C` |
| 34 | 0x0F8 | `WarpToScreen?` | Not yet characterized | `0x1001DE85` | `0x1001EA75` |
| 35 | 0x0FC | `MakeScreenShot` | Writes a screenshot file; screenshot messages | `0x1001DF19` | `0x1001EB09` |
| 36 | 0x100 | `ReadConsoleVariables` | No arguments; re-reads console variables through an engine callback | `0x10012E4C` | `0x10013BCD` |
| 37 | 0x104 | `BlitFromScreen?` | Warns when drawing a non-optimized surface | `0x1001DC81` | `0x1001E871` |

## 2D contract (implemented in `avp2m_d3d11.ren`)

Worked out from the stock renderer and confirmed by running our own renderer in the game:

- **Init** gets the engine's window handle at offset `0x218` of the init block (after the mode record). The renderer positions and sizes the window itself, and must write the final width and height into the render struct (+0x3C/+0x40).
- **Console variables:** engine callbacks 5, 6 and 7 are `GetConsoleVar(name)`, `GetConsoleVarFloat(var)` and `GetConsoleVarString(var)`, all cdecl. `windowed` is true when its string value is `"1"`.
- **GetScreenFormat(PFormat\*)** receives an engine C++ object. Call its vtable[0] (`thiscall Init(type, aMask, rMask, gMask, bMask)`, where type 3 = 32-bit) and the engine fills in the shift and bit counts.
- **Surfaces** are opaque renderer handles. The engine calls `GetSurfaceInfo(surf, &w, &h, &pitch)`, then `LockSurface(surf)` (returns the pixel pointer), writes pixels in the screen format, then calls `UnlockSurface(surf)`.
- **OptimizeSurface(surf, key)**: a key of `0xFFFFFFFF` means opaque, and any other value is keyed to transparent.
- **BlitToScreen(BlitRequest\*)** takes `{surface, flags, color_key, src*, dst*, alpha}`. Outside optimized-2D mode, the key applies only if flags bit 0 is set. Inside it (Start3D + StartOptimized2D), the request's key always applies, and the quad is tinted by the optimized-2D color and alpha with the current blend mode.
- **Clear(rect\*, flags, float color[3] 0–255)** uses flag 1 for color and 2 for depth.
- **Blend modes 0–6** (SetOptimized2DBlend): alpha, solid, add, multiply, multiply×2, mask, mask-add. The table is in `render_struct.h`.
- **The menu background is 3D:** menu frames call `RenderScene` before drawing text. The legal/splash screen is a single CPU-composited 640×480 surface.
- **Video playback** uses `GetHook("LPDIRECTDRAW")` and `GetHook("BACKBUFFER")`. Returning NULL makes the engine skip videos without crashing.

## 3D contract (in progress, milestone 2)

**Render context.** CreateContext's init block starts with the engine's world pointer. The stock renderer keeps it at `context+8` and builds its render data from it.

**World data** (read-only from the renderer):

| Where | Field |
|---|---|
| world `+0x18C` / `+0x190` | array of BSP entries / count (294 in "Unwelcome Guests"); entry `+4` is a WorldBsp |
| WorldBsp `+0xA0` / `+0xA4` | `WorldPoly**` / count |
| WorldBsp `+0x88` / `+0x8C` | 0x30-byte records / count (lights, tentative) |
| WorldPoly `+0x14` | u16 flags |
| WorldPoly `+0x28` | plane pointer (normal xyz, dist) |
| WorldPoly `+0x2C` | Surface pointer (surface `+0x30` = flags) |
| WorldPoly `+0x38` | lightmap origin vector (tentative) |
| WorldPoly `+0x4C` / `+0x4D` | lightmap width / height in texels |
| WorldPoly `+0x50` / `+0x54` | vertex array (0x18 bytes each, normally inline at `+0x58`) / u16 count (`+0x56`: extra clip vertices) |
| poly vertex `+0` | pointer to position (3 floats) |

**SceneDesc** (argument of RenderScene):

| Offset | Field |
|---|---|
| `0x00` | draw mode: 1 = world + visible objects, 2 = object list only (menus) |
| `0x34` | render context |
| `0x38`–`0x4C` | global light color/direction (tentative) |
| `0x50` | global light scale (3 floats, 0–1) |
| `0x5C` | global light add (3 floats, 0–1) |
| `0x68` | fog/ambient color (3 floats, 0–255, tentative) |
| `0xBC` | viewport rect (4 ints) |
| `0xCC` / `0xD0` | horizontal / vertical FOV (radians) |
| `0xD4` | far Z (30000 in AVP2) |
| `0xD8` | camera position (3 floats) |
| `0xE4` | camera rotation (quaternion x, y, z, w) |
| `0xF4` / `0xF8` | object list / count (draw mode 2) |

The camera uses LithTech's left-handed, Y-up, Z-forward convention. The quaternion → basis conversion in `avp2m_d3d11.c` is confirmed in game: moving and mouse-look both behave correctly.

## Next steps

- [x] Map the slot layout in both builds
- [x] Name slots from behavior; map the init contract and the mode record
- [x] Run the trace renderer in both games to confirm names, argument counts and call order
- [x] Confirm slots 2, 3, 5, 6, 33 from traces
- [ ] Slots never seen at runtime yet: 4, 13, 15, 17, 19, 22, 30, 31, 32, 34, 35 (screenshot, console commands, alt paths)
- [ ] Map the 14 engine callbacks
- [x] Start the D3D11 renderer against `render_struct.h`: milestone 1 (2D + presentation) works in AVP2
- [x] Milestone 2, step 1: SceneDesc camera + world geometry (flat-shaded, all BSPs, no culling) renders in game
- [ ] Milestone 2, step 2: world textures (surface texture + UV mapping, texture upload via BindTexture)
- [ ] Step 3: lightmaps; step 4: world models (doors) transforms; step 5: models, sprites, particles, sky
- [ ] Video playback (DirectDraw hook emulation or a separate Bink path)
