# Renderer interface (`d3d.ren`)

Status: **slot layout confirmed for both builds. The meaning of each slot isn't documented yet.**

## Loading

The engine loads the renderer DLL (`d3d.ren`) and calls its three exports:

| Export | Purpose (current understanding) |
|---|---|
| `RenderDLLSetup(RenderStruct*)` | Called once. Stores the engine's render struct pointer in a global, then fills a contiguous block of function-pointer slots in that struct. |
| `GetSupportedModes()` | Enumerates display modes (returns a linked list; to be confirmed). |
| `FreeModeList(list)` | Frees the list returned by `GetSupportedModes`. |

## The render struct

`RenderDLLSetup` receives a pointer to an engine-owned structure. Offsets **0x00–0x6F** are engine-side data or engine callbacks (not yet mapped). The renderer writes **38 function pointers** into the contiguous range **0x70–0x104**, one 4-byte slot each.

**AVP2 1.0.9.6 and Primal Hunt use the same slot layout**: the same 38 offsets, the same range, and nothing extra in either build. So a single replacement renderer can serve both games, as long as the meaning of each slot also matches. Checking that is the next step.

Analysis was done with Ghidra 12.2 through REA on the reference builds in [hashes.md](hashes.md). The addresses below are where each build's renderer implements each slot (image base `0x10000000` in both).

| Offset | Slot # | AVP2 1.0.9.6 `d3d.ren` | Primal Hunt `d3d.ren` |
|---|---:|---|---|
| 0x070 | 0 | `0x10010B22` | `0x100118BE` |
| 0x074 | 1 | `0x10010B13` | `0x100118AA` |
| 0x078 | 2 | `0x10021B50` | `0x10022830` |
| 0x07C | 3 | `0x10021C60` | `0x10022940` |
| 0x080 | 4 | `0x1001B790` | `0x1001C3A0` |
| 0x084 | 5 | `0x1001B700` | `0x1001C310` |
| 0x088 | 6 | `0x1001B770` | `0x1001C380` |
| 0x08C | 7 | `0x100142F0` | `0x10014D70` |
| 0x090 | 8 | `0x1001BD80` | `0x1001C990` |
| 0x094 | 9 | `0x1001BDB0` | `0x1001C9C0` |
| 0x098 | 10 | `0x1001BE20` | `0x1001CA30` |
| 0x09C | 11 | `0x1001C627` | `0x1001D237` |
| 0x0A0 | 12 | `0x1001C725` | `0x1001D335` |
| 0x0A4 | 13 | `0x1001C7EB` | `0x1001D3FB` |
| 0x0A8 | 14 | `0x1001C7F1` | `0x1001D401` |
| 0x0AC | 15 | `0x1001C8CA` | `0x1001D4DA` |
| 0x0B0 | 16 | `0x1001C8B8` | `0x1001D4C8` |
| 0x0B4 | 17 | `0x1001C8DA` | `0x1001D4EA` |
| 0x0B8 | 18 | `0x10017AA0` | `0x10018520` |
| 0x0BC | 19 | `0x1001B8A0` | `0x1001C4B0` |
| 0x0C0 | 20 | `0x100109C6` | `0x1001175D` |
| 0x0C4 | 21 | `0x1001E189` | `0x1001ED75` |
| 0x0C8 | 22 | `0x1001BD70` | `0x1001C980` |
| 0x0CC | 23 | `0x1001DEC3` | `0x1001EAB3` |
| 0x0D0 | 24 | `0x1001DA7F` | `0x1001E66F` |
| 0x0D4 | 25 | `0x1001DB2D` | `0x1001E71D` |
| 0x0D8 | 26 | `0x1001DB4E` | `0x1001E73E` |
| 0x0DC | 27 | `0x1001DB7E` | `0x1001E76E` |
| 0x0E0 | 28 | `0x1001DBB4` | `0x1001E7A4` |
| 0x0E4 | 29 | `0x1001C3E4` | `0x1001CFF4` |
| 0x0E8 | 30 | `0x1001C3D2` | `0x1001CFE2` |
| 0x0EC | 31 | `0x1001DBCA` | `0x1001E7BA` |
| 0x0F0 | 32 | `0x1001DC4F` | `0x1001E83F` |
| 0x0F4 | 33 | `0x1001DE1C` | `0x1001EA0C` |
| 0x0F8 | 34 | `0x1001DE85` | `0x1001EA75` |
| 0x0FC | 35 | `0x1001DF19` | `0x1001EB09` |
| 0x100 | 36 | `0x10012E4C` | `0x10013BCD` |
| 0x104 | 37 | `0x1001DC81` | `0x1001E871` |

## Next steps

- [ ] Map the engine-side part of the struct (0x00–0x6F): which fields the renderer reads (window handle, screen width/height, bit depth, engine callbacks)
- [ ] Name each of the 38 slots by behavior: init/term, begin/end scene, draw world, draw models, surfaces/textures, screen blits, gamma, mode switching, etc.
- [ ] Confirm slot semantics match between the two builds (compare function size and call patterns per slot)
- [ ] Find where `lithtech.exe` calls each slot, which gives argument counts and types
- [ ] Write `src/renderer/render_struct.h`: an original, clean-room header describing the struct layout
- [ ] Stub renderer: forwards every slot to the original `d3d.ren` and logs each call (a "proxy renderer"), so the exact calling sequence can be traced in a live game
