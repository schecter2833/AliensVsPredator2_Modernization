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

## Fixed: Primal Hunt crash on "Zeta Site" load

**Not a test-setup problem.** The official launcher's command file (captured from
`%LOCALAPPDATA%\VirtualStore\Program Files (x86)\Aliens versus Predator 2 - Primal Hunt\avp2cmds.txt`,
since Windows redirects the launcher's write there) uses this order:

```
-rez <AVP2>\AVP2.rez -rez <AVP2>\sounds.rez -rez <AVP2>\Alien.rez -rez <AVP2>\Marine.rez
-rez <AVP2>\Predator.rez -rez <AVP2>\Multi.rez -rez multi.rez -rez AVP2dll.rez -rez AVP2l.rez
-rez dialogue.rez -rez avp2p.rez -rez avp2p1.rez -rez avp2x.rez -rez custom
+DisableMusic 0 +DisableSound 0 +EnableTripBuf 1 +DisableHardwareCursor 0 ++PerformanceConfig .DefaultLow
```

With exactly that order (`test-ph/run_official_order.bat`) Zeta Site still crashed the same way, with the
stock renderer.

**Cause:** a bug in Primal Hunt's `object.lto` (function at RVA `0xD5620`, a damage-effect update). It reads
a flag from the character attribute table (entries of `0x7D8` bytes) using the object's character index.
For objects that are neither `CCharacter` nor `BodyProp` that index is **-1**, so it reads `0x2B0` bytes
*before* the table. The faulting instruction is `mov ecx, [eax + ecx*8 + 0x528]` with the index already
multiplied by 251, hence `ECX = 0xFFFFFF05`. The table is a large allocation that starts 8 bytes into a
page (`EAX = ...F008`); on current Windows the page in front of it isn't mapped, so the stray read faults.
On the heap of the time there was readable memory there and the bug went unnoticed.

**Fix:** `src/compat/game_fixes.c`, applied in memory by both of our renderer DLLs (files on disk stay
stock, so multiplayer file checks are unaffected). A background watcher finds the loaded module by its
exact original bytes and rewrites the 49 bytes at RVA `0xD56F3` to treat a negative index as "flag not
set". Confirmed: with the trace renderer (stock rendering) the fix is logged as applied and Zeta Site loads
and plays. Lessons from getting there: the watcher must not use Toolhelp snapshots (deadlocked during
renderer Init), must not start before the first frame, and must check memory is readable before comparing
bytes (a fault on its thread silently ends the process).

Side note: the official launcher starts the real game fullscreen at 640×480, which works on this machine.

## Primal Hunt: stretched heads in the Zeta Site intro (not a renderer bug)

In the Zeta Site intro cutscene several characters show long thin spikes from the head, and Dunya's
hair is pulled up into a spike (the bald-looking head). The **stock renderer shows exactly the same
thing** at the same moments (compared side by side, same frames), so it comes from the game or engine
side on this system, not from our renderer. In the single-player menu the same character model looks
normal. Worth a later look (animation/skinning in the engine or game code on modern Windows), since a
fix would have to be outside the renderer.

Elsewhere at the start of Zeta Site (menus, mission screen, cutscene rooms, the bunker entrance) the
D3D11 renderer matches the stock one.
