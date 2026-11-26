# Runtime findings (trace renderer)

First live runs of `avp2m_trace.ren` on Windows 11 (2560×1080 display), 2026-11-26. Builds as in [hashes.md](hashes.md). Trimmed logs are in [traces/](traces/).

## Test setup

- **AVP2 1.0.9.6:** test copy of the install, started with
  `lithtech.exe -rez AVP2.REZ -rez SOUNDS.REZ -rez ALIEN.REZ -rez MARINE.REZ -rez PREDATOR.REZ -rez MULTI.REZ -rez AVP2L.REZ -rez AVP2DLL.REZ -rez AVP2P.REZ -rez AVP2P1.REZ -rez AVP2SP.REZ +windowed 1`
  and `"RENDERDLL" "avp2m_trace.ren"` in `autoexec.cfg`.
- **Primal Hunt:** engine files copied; data from both installs via absolute `-rez` paths (AVP2's `AVP2/SOUNDS/ALIEN/MARINE/PREDATOR.REZ`, then PH's `AVP2X/MULTI/AVP2P/AVP2P1/AVP2L/AVP2DLL/DIALOGUE.REZ`). The official PH launcher instead writes an `avp2cmds.txt` and runs `lithtech.exe -cmdfile avp2cmds.txt`. Its exact contents still need capturing (see the crash below).
- Self-test (`tests/trace`) passes on the same machine: all 38 slots pass arguments and return values through unchanged, including 64-bit returns.

## Results

| | AVP2 1.0.9.6 | Primal Hunt |
|---|---|---|
| Proxy loads, stock renderer chained | ✅ | ✅ |
| Init accepted (version `0xD5D`) | ✅ | ✅ |
| Render struct address | `0x004E4888` (matches static analysis) | `0x004F5A48` |
| Menus render | ✅ | ✅ |
| In-game 3D (cutscene + gameplay) | ✅ Marine "Unwelcome Guests" | ❌ crashed on level load, **same crash with the stock renderer** |
| Clean quit (Term + unload) | ✅ | n/a |

## What the traces confirmed

**Per-frame sequence** (both games):

```
Clear(rect*, flags, color)
Start3D
  RenderScene(SceneDesc*)            (in-game; PH also in its 3D menus)
  StartOptimized2D
    BlitToScreen(BlitRequest*) ...   (HUD, menus, text)
  EndOptimized2D
End3D
SwapBuffers(flags = 1)
```

`IsIn3D` is checked roughly 5 times per frame.

**Slots confirmed by runtime behavior:**
- **2/3 BindTexture/UnbindTexture:** always paired (e.g. 972/972).
- **5/6 CreateContext/DeleteContext:** called at level start and end. The handle returned by Create is the one passed to Delete.
- **33 BlitToScreen:** carries all 2D drawing. It was called **24 million times in one AVP2 session** and 230 million times while PH idled in its menu for about 55 minutes.
- **23 GetScreenFormat, 36 ReadConsoleVariables:** once, right after Init.
- **24–29 surface functions:** surfaces are created at load time. Lock/Unlock happen per frame during cutscenes, about 395k times, which points to software-drawn surfaces such as video or subtitles.

**Not called in normal play so far:** 4, 13, 15, 17, 19, 22, 30–32, 34, 35. These need the console, the screenshot key, or special paths.

## Important behaviors for a replacement renderer

1. **The game asks the renderer for raw DirectDraw objects.** `GetHook("LPDIRECTDRAW")` and `GetHook("BACKBUFFER")` are called from `lithtech.exe` at `0x401E73`/`0x401EA7`/`0x402366`. In one AVP2 session that happened about 240k times, so per frame during some phase. A D3D11 renderer has to either provide a DirectDraw-compatible object, or find out which feature uses these (probably video playback) and handle it another way.
2. **Every focus loss fully unloads the renderer.** On alt-tab or a click elsewhere, the engine calls `Term`, does `FreeLibrary` on the `.ren`, then reloads it with `RenderDLLSetup` + `Init` when focus returns. This happened 5 times in one session. A new renderer must survive repeated load/unload cycles, and this is where the classic AVP2 alt-tab problems come from.
3. **No frame limiter.** Menus ran at about 2,000+ fps and gameplay at about 900 fps. A frame limiter, or a fix for the physics at high frame rates, belongs in the hook layer (Phase 2).
4. **Windowed placement bug.** In windowed mode on a 2560×1080 display, the engine centers the window horizontally but places it at y≈929, mostly off-screen, and moves it back there every time it regains focus. A replacement renderer, or the hook layer, should own window placement.

## Known issue: Primal Hunt crash on "Zeta Site" load (test setup)

- `object.lto` (PH) access violation at RVA `0xD5719` (VA `0x100D5719` at its preferred base), inside the function at RVA `0xD5620`.
- That function looks up the current character's type ID (from `CCharacter` or `BodyProp`) and uses it to index a 0x7D8-byte-per-entry attribute table. The ID was `-251`, so the read went out of bounds.
- **It reproduces identically with the stock `d3d.ren`**, so the trace renderer isn't the cause. The most likely cause is the hand-built `-rez` order or set (character attributes not loaded or overridden as the official launcher would). Next step: capture the `avp2cmds.txt` the PH launcher writes and retest with exactly that.
