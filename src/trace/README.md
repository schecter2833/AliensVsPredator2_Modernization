# avp2m_trace.ren: logging proxy renderer

A drop-in renderer that **changes nothing on screen**. It loads the stock `d3d.ren`, lets it set up as normal, and logs every call the engine makes into the renderer. These logs are how we confirm the slot names and argument counts in [docs/renderer-interface.md](../../docs/renderer-interface.md) before writing a new renderer.

It works with both **AVP2 1.0.9.6** and **Primal Hunt**, which share the same renderer interface.

## Build

From Linux, WSL, or an MSYS2 **MINGW32** shell:

```sh
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-i686.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Outputs: `build/avp2m_trace.ren`, plus test programs in `build/test/`.

## 1. Self-test (no game needed)

`build/test/` contains a fake engine, a fake stock renderer (named `d3d.ren`) and a copy of the proxy. Run:

```
cd build\test
fake_engine.exe
```

It should print `ALL TESTS PASSED` and create `avp2m_trace.log`. This checks that every slot's arguments and return values (including 64-bit returns) pass through the proxy unchanged.

## 2. In-game trace

1. Copy `avp2m_trace.ren` into the game folder, next to the stock `d3d.ren`. **Leave `d3d.ren` where it is**, because the proxy loads it.
2. Back up `autoexec.cfg`, then change
   `"RENDERDLL" "d3d.ren"` to `"RENDERDLL" "avp2m_trace.ren"`.
3. Start the game the way you normally do, and don't change display options in the launcher. Play for a minute: menus, start a level, open the console, take a screenshot, quit normally. If no log appears, check that `autoexec.cfg` still says `avp2m_trace.ren`, since the launcher's display settings can rewrite it.
4. Collect `avp2m_trace.log` from the game folder and attach it to an issue.

To undo: restore `autoexec.cfg` (or set `RENDERDLL` back to `d3d.ren`) and delete `avp2m_trace.ren`.

> The display-mode list may show every mode twice while the proxy is in the folder, because the engine lists modes from every `.ren` file it finds. That's expected.

> Use it offline or on a private server. It doesn't touch any game module or network code, but there's no reason to test experimental builds on public servers.

## What the log contains

- The stock renderer's address of every slot, and the engine's 14 callback pointers
- The display modes the stock renderer reports
- The Init request (resolution, device) and its result
- The first 8 calls of every slot, with caller address and the first 4 stack arguments
- Call counts per slot every 30 seconds, at `Term`, and when the game exits

## How it works

`thunks.S` holds 38 small assembly thunks. Each one saves all registers and flags, calls `avp2m_trace_enter(slot, args)`, restores everything, and jumps to the stock function, so the engine's stack is untouched. `Init` is wrapped in C so its result can be logged. The logging uses only Win32 calls and no floating point, so it can't disturb the engine's FPU or SSE state.

## Research probe (v0.1.3+)

While the stock renderer runs behind the proxy, every 30th `RenderScene` the proxy logs the stock renderer's per-model lighting state (`modellight` lines: position, light add, color, global light, grid ambient, light count) for the last model it lit. This is how the D3D11 renderer's model lighting formula was measured. The offsets are specific to the AVP2 1.0.9.6 `d3d.ren`. If `d3d.ren` isn't next to the proxy, it also tries `stock_d3d.ren`.

v0.1.4 also logs the stock renderer's particle size factor (`particle K`, image offset `0x55D8C`) and viewport width.
