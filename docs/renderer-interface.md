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
| world `+0x08` / `+0x0C` | light animation array (0x60 bytes each) / count. See "Lighting" below |
| world `+0xF8` | lightmap grid size in world units per texel (float; 40 in "Unwelcome Guests") |
| WorldPoly `+0x0C` / `+0x10` | light animation refs: array of `{u16 anim, u16 entry}` / count |
| WorldPoly `+0x14` | u16 flags; bits 11–13 select the lightmap axis frame (0–5) |
| WorldPoly `+0x28` | plane pointer (normal xyz, dist) |
| WorldPoly `+0x2C` | Surface pointer (surface `+0x30` = flags) |
| WorldPoly `+0x38` | lightmap origin vector |
| WorldPoly `+0x4C` / `+0x4D` | lightmap width / height in texels |
| WorldPoly `+0x50` / `+0x54` | vertex array (0x18 bytes each, normally inline at `+0x58`) / u16 count (`+0x56`: extra clip vertices) |
| poly vertex `+0` | pointer to position (3 floats) |
| poly vertex `+4` / `+8` | texture u / v **in texels of mip 0** (precomputed by the engine) |
| poly vertex `+0x0C` / `+0x10` | scratch: the stock renderer writes its lightmap u / v here |
| poly vertex `+0x14` | scratch: the stock renderer writes its vertex color here. The engine does not fill it, so a replacement renderer computes vertex light itself (see "Lighting") |
| Surface `+0x00` / `+0x0C` / `+0x18` | texture O / P / Q vectors |
| Surface `+0x2C` | SharedTexture pointer |
| Surface `+0x30` | flags: `0x04` invisible, `0x10` sky, `0x80` lightmapped |

**Textures.** A SharedTexture keeps the renderer's private pointer at `+0x0C`. To get pixels, call engine callback [1] (`+0x08`) `TextureData *GetTexture(SharedTexture*, int *flag)`, upload, then callback [2] (`+0x0C`) `FreeTexture(SharedTexture*)`. Upload before freeing, because the pixel data can live in a shared scratch buffer.

| TextureData | Field |
|---|---|
| `+0x08` | DTX header starts (public DTX format) |
| `+0x14` | u16 mip count |
| `+0x18` | DTX flags |
| `+0x20` | texture group |
| `+0x22` | pixel format: 0/3 = 32-bit BGRA, 4 = DXT1, 5 = DXT3, 6 = DXT5 |
| `+0xCC + 0x18·i` | mip i: width, height, data pointer, pitch (pitch is **0 for DXT mips**; compute it from blocks) |

**Lighting.** All static world light lives in the light animations; the renderer combines it.

| Light animation (0x60 bytes) | Field |
|---|---|
| `+0x00` | name, e.g. `LightAnim_BASE` (the static lighting), `Flicker1WaveLightFX0__LA` |
| `+0x20` | kind: 0 = baked frames (handled), non-zero = shadow-mask point light (not yet) |
| `+0x24` | pointer to the frame table (one pointer per frame) |
| `+0x28` | frame count |
| `+0x30` | u16 entries per frame |
| `+0x34` / `+0x38` / `+0x3C` | current frame A / frame B / blend 0–255 (−1 in A = off) |
| `+0x40` | intensity (float). The stock renderer skips anims below 0.02 |

A frame is an array of 0x18-byte entries, indexed by the poly ref's `entry`:

| Entry | Field |
|---|---|
| `+0x00` / `+0x04` | lightmap data pointer / u16 byte size (0 for gouraud polys) |
| `+0x08` / `+0x0C` / `+0x10` | gouraud polys: per-vertex **blue / green / red** byte arrays |
| `+0x14` | gouraud polys: vertex count |

- **Lightmap data** is run-length encoded 32-bit pixels: a u32 `0x00RRGGBB`; if bit 31 is set, a u8 run length follows. At most 32×32 texels.
- **Combining:** the first contributing anim is copied, and each further one is scaled by its intensity and added with saturation.
- **Lightmap UVs:** let N be the poly's plane normal and R the reference vector of its axis frame (index = flags bits 11–13): (0,0,−1), (0,0,1), (0,1,0), (0,−1,0), (0,−1,0), (0,−1,0). Then P = R × N and Q = N × P (not normalized). Texel coordinates are `s = dot(pos − origin, P) / grid + 0.5` and `t = dot(pos − origin, Q) / grid + 0.5`, so texel i's center sits at i + 0.5.
- **Polys without a lightmap** (most terrain) are gouraud-lit from the per-vertex arrays above. Texture × light, with no 2× overbright, matches the stock renderer in "Unwelcome Guests".

**Objects and visibility.** In world mode (draw mode 1) the SceneDesc object list is empty. The renderer asks the engine's world tree for objects:

- `world+0x6C` is the world tree. Its vtable slot 5 (`+0x14`) is a thiscall query taking one struct (≥ 0x48 bytes; the engine uses `+0x2C`–`+0x47` as scratch, so the struct must reserve them):

| Query | Field |
|---|---|
| `+0x00` | object list index (0 = normal objects) |
| `+0x04` | viewpoint (3 floats) |
| `+0x10` | radius around the viewpoint |
| `+0x14` | `void cb(obj, user)`: objects in quadtree nodes without BSP leaves whose box overlaps the query box |
| `+0x18` | `void cb(list_head, obj ***out, int *count)`: a visible BSP leaf's object list. Items are `{next, prev, obj}`; stop at the head or at an object whose `+0x190` is set. The stock renderer appends to `(*out)[(*count)++]` |
| `+0x1C` | user pointer passed to `+0x14` |
| `+0x20` | `void cb(leaf)`: visible leaf (its polys and polygrids) |
| `+0x24` | `int cb(box)`: box visibility (center `+0x0C`, half size `+0x18`) |
| `+0x28` | `int cb(node)`: quadtree node visibility (center `+0x30`, radius `+0x40`) |

The same object can be reported by several nodes, so deduplicate. "Unwelcome Guests" at the start: about 120 objects (17 models, 78 world models, 10 sprites, 10 particle systems, lights).

| Object | Field |
|---|---|
| `+0x00` | engine class vtable |
| `+0x58` | engine frame stamp (written by the engine during queries) |
| `+0x88` | flags (bit 0 = visible) |
| `+0x8C` | flags 2 (`0x100` = hidden from the renderer) |
| `+0x94` | color (ARGB) |
| `+0x9C` | rotation quaternion x, y, z, w (also mirrored at `+0x168`) |
| `+0xAC` | scale (3 floats) |
| `+0xBC` | byte type: 0 normal, 1 model, 2 world model, 3 sprite, 4 light, 5 camera, 6 particle system, 7 polygrid, 8 line system, 9 container, 10 canvas |
| `+0xF4` / `+0x100` | world-space bounding box min / max |
| `+0x194` | position |
| `+0x1A0` | dimensions (world models: half size; lights: radius at `+0x1B0`) |
| `+0x1C0` | models: model data pointer (tentative) |
| `+0x1B0` | world models: their WorldBsp (matches an entry in the world's BSP list) |
| `+0x1F8` | world models: current transform, 4×4 floats, row-major, translation in the last column. Identity until the object moves |
| `+0x1B4` / `+0x1B8` | sprites: sprite pointer / current frame entry (first dword = the frame's SharedTexture) |

**Sprites** (type 3): size is texture pixels × scale (`+0xAC` x, `+0xB0` y); color bytes `+0x94`–`+0x97` are r, g, b, a. Flags `+0x88`: `0x08` rotatable (quad oriented by the object's rotation instead of facing the camera), `0x20` scales with distance (glows), `0x40` "really close" (position is in camera space), `0x200` not lit by the world. Flags 2 `+0x8C`: `0x04` additive (ONE, ONE), `0x08` multiply (ZERO, SRCCOLOR), otherwise alpha blend.

**Models** (type 1). The engine animates the skeleton; the renderer skins:

| Field | Meaning |
|---|---|
| object vtable slot 18 (`+0x48`) | thiscall, no arguments: returns one 4×4 per node (row-major, translation in the last column), in model space |
| object `+0x1C0`–`+0x1CC` | skins: up to 4 SharedTexture pointers |
| object `+0x1F8` | model |
| model `+0x04` | file name, e.g. `models\props\flyingcreature.abc` |
| model `+0x28` / `+0x2C` | node array / node count |
| model `+0x38` / `+0x3C` | piece pointers / piece count |
| model `+0x74` / `+0x78` | scratch array the stock renderer fills with per-node matrices / count |
| piece `+0x04` / `+0x08` | vertices (0x20 bytes) / count |
| piece `+0x14` / `+0x18` | triangles (0x20 bytes) / count |
| piece `+0x34` | further LODs (array, tentative) |
| piece `+0x40` / `+0x48` | skin index (tentative) / name, e.g. `Torso` |
| vertex | `+0x00` weights pointer, `+0x04` u16 weight count, `+0x08` bind position, `+0x14` normal |
| weight (0x14 bytes) | `float offset[3]; float weight; u32 node`. The skinned position is the sum of `weight × (node transform × offset)` |
| triangle | `+0x00` u16 indices[3], `+0x08` three (u, v) float pairs |

The object matrix comes from position `+0x194`, rotation `+0x9C` and scale `+0xAC`; objects flagged `0x40` ("really close", the first-person weapon and arms) are in camera space.

**Model lighting** (partly mapped):

- **Light grid** at `world+0xFC`: `+0x00` cells (u32 `0x00RRGGBB`), `+0x08` y stride, `+0x20` z stride (in cells; x stride is 1), `+0x14`/`+0x18`/`+0x1C` max cell index x/y/z, `+0x24` cell size (3 floats; 2000 in "Unwelcome Guests"), `+0x30` 1/cell size, `+0x3C` grid minimum. The stock renderer samples it trilinearly at the model's position. The raw values are very dark (0–20 of 255 at the level start). The stock renderer's per-channel tables at `0x1005A004` / `0x1005A104` / `0x1005A204` turn out to be plain linear ramps, `min(255, i × light scale)`, built from the scene's light scale (1.0 here), so they don't explain the brighter stock result.
- **Global (sky) light:** RenderStruct `+0x138` is the direction the light travels and `+0x144` its color (3 floats, 0–255). "Unwelcome Guests" uses a moonlight of (50, 74, 99) shining straight down. The stock renderer stores the negated direction (towards the light) for shading.
- **Model hook:** the stock renderer calls the game code's hook (SceneDesc `+0xFC`, user data `+0x100`) once per model with `{object, hook flags = 1, object flags, float *light_add, float *object_color}`. Light add starts at SceneDesc `+0x1C` and color at the object's color, both 0–255. In "Unwelcome Guests" it returns add 0 and color 255.
- **Measured formula.** The trace proxy's research probe reads the stock renderer's per-model lighting state (image offset `0x52980`) while the original renderer runs. For the first-person knife it shows color 2.0 (object color / 255, times a 2× model factor), grid ambient (0, 2, 4) and the global light (50, 74, 99) towards (−0.06, 0.99, 0.01). So per vertex: light = add + color × (grid + global light × max(0, N·L) + point lights). The global light is what makes models visibly lit at night; the dark grid alone is correct.
- **Static lights:** world tree vtable slot 2 (`+0x08`, thiscall) is a box query: `+0x04` list index (1 = static lights), `+0x08` min, `+0x14` max, `+0x2C` `void cb(light, user)`, `+0x30` user. Light: `+0x68` position, `+0x74` radius, `+0x78` color (3 floats, 0–255), `+0x84` direction, `+0x90` cone cosine, `+0x94` outer color.
- **Dynamic lights:** light objects (type 4): position `+0x194`, radius `+0x1B0`, color bytes `+0x94`–`+0x96`.

**Particle systems** (type 6): `+0x1F0` SharedTexture, `+0x1D0` first particle, `+0x230` particle count; the list ends at a sentinel embedded at `+0x1B0`. Particle: `+0x0C` color (3 floats, 0–255), `+0x18` alpha (0–1), `+0x1C` size, `+0x20` next, `+0x24` position in system space (the object's position, rotation and scale apply). The stock renderer draws them in batches of 128 as camera-facing quads; blending follows flags 2 like sprites. The size-to-world scale isn't confirmed yet (stock: projected half size = size × a viewport factor that's set indirectly).

**Menus** (draw mode 2) draw only the SceneDesc object list (`+0xF4` / `+0xF8`): the main menu's starfield and planets are sprites, plus one model.

**Sky** (draw mode 1). SceneDesc `+0x84`/`+0x90` hold the sky box min/max, `+0x9C`/`+0xA8` the sky view min/max (the range the sky camera moves in; a single point in "Unwelcome Guests"), and `+0xB4`/`+0xB8` the sky object list (world models built far outside the level; 11 here). The sky is drawn first from the sky camera with the main camera's rotation, then depth is cleared and the level is drawn. Sky-flagged polygons (surface flag `0x10`) are left out, so the sky shows through them.

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
- [x] Milestone 2, step 2: world textures. 566 textures (32-bit + DXT1/3/5) in 'Unwelcome Guests', 5 polygons untextured
- [x] Step 3: static lighting. Lightmaps (3,313 polys in "Unwelcome Guests") and gouraud vertex light, both from `LightAnim_BASE`
- [ ] Animated lights (flicker anims, shadow-mask lights); dynamic lights
- [x] Object enumeration through the engine's world tree (models, world models, sprites, particles, lights)
- [x] Sprites (billboards and rotatable), in game and in the menu background
- [x] Step 4: world models (doors, lifts, rotating parts) drawn with their current transform
- [x] Step 5a: models (characters, first-person weapon and arms), skinned on the CPU from the engine's node transforms
- [x] Model lighting matching the stock formula (light grid, global sky light, static and dynamic point lights, the game's model hook, 2× model color)
- [x] Sky (sky world models from a separate sky camera)
- [~] Particles: drawn from the engine's particle lists (counts confirmed in game); size scale and look not yet compared with the stock renderer
- [ ] LODs, translucent models, attachments; sky sprites and models
- [ ] Video playback (DirectDraw hook emulation or a separate Bink path)
