# avp2m_d3d11.ren: Direct3D 11 renderer

A from-scratch replacement for the stock Direct3D 7 renderer (`d3d.ren`), written against the interface in [`../render_struct.h`](../render_struct.h). It works with both AVP2 1.0.9.6 and Primal Hunt, which share the interface.

## Status: milestone 1 done, milestone 2 (3D) in progress

**Works**, tested in AVP2 1.0.9.6 on Windows 11:
- Direct3D 11 device and swap chain (flip model, falling back to the older blt model), with vsync
- All 2D drawing: menus, fonts, HUD, motion tracker, message panels, fades and transparency. That covers surfaces, color keys, all 7 optimized-2D blend modes, and the optimized-2D color and alpha.
- Clear (full and partial)
- **Windowed mode is centered correctly.** The stock renderer centers vertically using the horizontal offset, which puts the window off-screen on wide displays.
- **Borderless fullscreen** on the current monitor, with the image scaled at the correct aspect ratio and black bars as needed. No display-mode switch.
- Display-mode list from DXGI (every mode the monitor supports, plus the classic 4:3 modes)
- Survives the engine's unload/reload of the renderer on every focus loss

**3D so far:**
- Camera from the engine's scene description (position, quaternion, FOV, viewport)
- All level geometry from the engine's world data, textured (32-bit and DXT1/3/5, mipmapped, anisotropic filtering), with depth buffering. Textures are fetched through the engine's own GetTexture/FreeTexture callbacks.
- Objects from the engine's visibility query. Sprites are drawn (billboards and rotatable sprites, alpha/additive/multiply), which brings back the main menu's starfield and planets.
- Static lighting: lightmaps (packed into 2048×2048 atlas pages, bilinear-filtered with clamped borders) and per-vertex gouraud light, both decoded from the level's `LightAnim_BASE` data. Brightness and color match the stock renderer.

**Not yet:**
- Animated and dynamic lights (flickering lights, muzzle flashes, flares), the sky, moving world models (doors), and all objects: characters, weapons, sprites, particles, and the menu's starfield
- No visibility culling. The whole level is one set of draw batches, which modern GPUs handle easily for 2001-era levels.
- **Video playback:** `GetHook("LPDIRECTDRAW"/"BACKBUFFER")` returns NULL, so the engine skips the intro videos cleanly.
- Screenshots, `RenderCommand` console commands, and the rarely used screen lock/warp slots

## Using it (testing only, not playable yet)

1. Put `avp2m_d3d11.ren` in the game folder.
2. In `autoexec.cfg`, set `"RENDERDLL" "avp2m_d3d11.ren"`.
3. Optional: start with `+windowed 1` for a window, or `+windowed 0` for borderless fullscreen.

**Don't change the resolution in the game's Graphics menu while the stock `d3d.ren` is also in the folder.** The engine lists modes from every `.ren` file, and choosing one of the stock renderer's modes switches back to it. For testing, use a copy of the game folder with `d3d.ren` renamed (e.g. to `d3d.ren.stock`).

Console variables:

| Variable | Default | Meaning |
|---|---|---|
| `windowed` | 0 | 1 = window, 0 = borderless fullscreen |
| `avp2m_vsync` | 1 | 0 disables vsync. The game is uncapped without it. |
| `avp2m_lmscale` | 100 | Light brightness in percent (lightmaps and vertex light). Read live. |
| `avp2m_lmdebug` | 0 | Debug views: 1 light only, 2 vertex light, 3 lightmaps, 4 lightmapped (red) vs gouraud (green). Read live. |

It writes `avp2m_d3d11.log` next to the DLL.

## Notes for contributors

- **The game's menu code only offers 4:3 modes,** even though the renderer reports every mode. That filter is in `CShell.DLL`, which we keep stock. True widescreen (Hor+) will come from rendering a 4:3 mode wider and adjusting the camera FOV through the hook layer, not from this mode list.
- Surfaces are CPU buffers in X8R8G8B8 (the engine writes pixels after `LockSurface`, using the pitch from `GetSurfaceInfo`). They're uploaded to B8G8R8A8 textures on first use after a change, with the color key turned into alpha. A key of `0xFFFFFFFF` means opaque.
- The engine draws text one glyph at a time from wide font strips, so 2D quads are batched by texture and blend state.
- Everything is drawn into a game-resolution render target, then scaled to the window at Present. That's where future upscaling and filters can go.
