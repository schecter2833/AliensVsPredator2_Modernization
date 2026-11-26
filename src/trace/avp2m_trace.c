/*
 * avp2m_trace.ren - logging proxy renderer for AVP2 / Primal Hunt.
 *
 * Loads the stock renderer (d3d.ren in the same folder), lets it fill the
 * render struct, then swaps each of the 38 slots for a thunk that logs the
 * call before jumping to the stock function. Rendering is unchanged; the
 * point is to record how the engine actually uses the renderer, so we can
 * build a replacement with confidence.
 *
 * Output: avp2m_trace.log next to the DLL.
 *
 * Original code. SPDX-License-Identifier: GPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>

#include "../renderer/render_struct.h"

#define TRACE_VERSION      "0.1.2"
#define LOG_FIRST_CALLS    8      /* full lines for the first N calls per slot */
#define SUMMARY_EVERY_MS   30000  /* ms between count summaries (menus run uncapped at 2000+ fps) */

/* Referenced from thunks.S */
AVP2M_Fn avp2m_orig_slots[RS_SLOT_COUNT];
extern const AVP2M_Fn avp2m_thunk_table[RS_SLOT_COUNT];
void __cdecl avp2m_trace_enter(int idx, const uint32_t *args);

static const char *const g_slot_names[RS_SLOT_COUNT] = {
    "Init", "Term", "BindTexture", "UnbindTexture", "LoadWorldData?",
    "CreateContext", "DeleteContext", "Clear", "Start3D", "End3D",
    "IsIn3D", "StartOptimized2D", "EndOptimized2D", "IsInOptimized2D",
    "SetOptimized2DBlend", "GetOptimized2DBlend", "SetOptimized2DColor",
    "GetOptimized2DColor", "RenderScene", "RenderCommand", "GetHook",
    "SwapBuffers", "GetInfoFlags", "GetScreenFormat", "CreateSurface",
    "DeleteSurface", "GetSurfaceInfo", "LockSurface", "UnlockSurface",
    "OptimizeSurface", "UnoptimizeSurface", "LockScreen?", "UnlockScreen?",
    "BlitToScreen", "WarpToScreen?", "MakeScreenShot",
    "ReadConsoleVariables", "BlitFromScreen?",
};

static HMODULE              g_self;
static HMODULE              g_real;
static PFN_RenderDLLSetup   g_real_setup;
static PFN_GetSupportedModes g_real_get_modes;
static PFN_FreeModeList     g_real_free_modes;
static RenderStruct        *g_rs;
static volatile LONG        g_counts[RS_SLOT_COUNT];
static volatile LONG        g_setup_done;
static HANDLE               g_log = INVALID_HANDLE_VALUE;
static CRITICAL_SECTION     g_lock;
static DWORD                g_start_tick;
static int                  g_lock_ready;
static volatile LONG        g_last_summary_tick;

/* ------------------------------------------------------------------ */
/* Logging (Win32 only: no CRT, no floating point)                     */
/* ------------------------------------------------------------------ */
static void dir_of_self(char *out, DWORD cap)
{
    DWORD n = GetModuleFileNameA(g_self, out, cap);
    while (n > 0 && out[n - 1] != '\\' && out[n - 1] != '/')
        n--;
    out[n] = '\0';
}

static void log_open(void)
{
    char path[MAX_PATH];
    if (g_log != INVALID_HANDLE_VALUE)
        return;
    dir_of_self(path, MAX_PATH - 32);
    lstrcatA(path, "avp2m_trace.log");
    g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
}

static void log_raw(const char *s)
{
    DWORD w;
    if (g_log == INVALID_HANDLE_VALUE)
        return;
    WriteFile(g_log, s, (DWORD)lstrlenA(s), &w, NULL);
}

static void tlog(const char *fmt, ...)
{
    char buf[1024];
    char line[1100];
    va_list ap;
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    wsprintfA(line, "[%7lu ms] %s\r\n", GetTickCount() - g_start_tick, buf);
    if (g_lock_ready) EnterCriticalSection(&g_lock);
    log_open();
    log_raw(line);
    if (g_lock_ready) LeaveCriticalSection(&g_lock);
}

static void log_summary(const char *why)
{
    int i;
    tlog("---- call counts (%s) ----", why);
    for (i = 0; i < RS_SLOT_COUNT; i++) {
        if (g_counts[i])
            tlog("  slot %2d %-22s %10ld", i, g_slot_names[i], g_counts[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Stock renderer loading                                               */
/* ------------------------------------------------------------------ */
static int load_real(void)
{
    char path[MAX_PATH];
    if (g_real)
        return 1;
    dir_of_self(path, MAX_PATH - 32);
    lstrcatA(path, "d3d.ren");
    g_real = LoadLibraryA(path);
    if (!g_real) {
        tlog("ERROR: could not load stock renderer %s (error %lu)", path, GetLastError());
        return 0;
    }
    g_real_setup      = (PFN_RenderDLLSetup)(void *)GetProcAddress(g_real, "RenderDLLSetup");
    g_real_get_modes  = (PFN_GetSupportedModes)(void *)GetProcAddress(g_real, "GetSupportedModes");
    g_real_free_modes = (PFN_FreeModeList)(void *)GetProcAddress(g_real, "FreeModeList");
    if (!g_real_setup || !g_real_get_modes || !g_real_free_modes) {
        tlog("ERROR: %s is missing renderer exports", path);
        FreeLibrary(g_real);
        g_real = NULL;
        return 0;
    }
    tlog("loaded stock renderer %s at %08lX", path, (unsigned long)(uintptr_t)g_real);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Safe reads of guessed pointer arguments                              */
/* ------------------------------------------------------------------ */
static int safe_ptr(const void *p, UINT_PTR n)
{
    return (uintptr_t)p >= 0x10000 && !IsBadReadPtr(p, n);
}

static int safe_str(const char *p)
{
    return (uintptr_t)p >= 0x10000 && !IsBadStringPtrA(p, 200);
}

/* ------------------------------------------------------------------ */
/* Per-call trace (called from the thunks with all registers saved)     */
/* ------------------------------------------------------------------ */
void __cdecl avp2m_trace_enter(int idx, const uint32_t *args)
{
    LONG n = InterlockedIncrement(&g_counts[idx]);

    if (n <= LOG_FIRST_CALLS) {
        tlog("slot %2d %-22s #%ld caller=%08lX args=%08lX %08lX %08lX %08lX",
             idx, g_slot_names[idx], n, (unsigned long)args[-1],
             (unsigned long)args[0], (unsigned long)args[1],
             (unsigned long)args[2], (unsigned long)args[3]);
        /* String arguments are only read after checking the memory is
         * readable: slot signatures are still partly guessed, and the
         * tracer must never be the thing that crashes the game. */
        if (idx == RS_RenderCommand && args[0] > 0 && args[0] < 64) {
            const char *const *argv = (const char *const *)(uintptr_t)args[1];
            if (safe_ptr(argv, sizeof(char *)) && safe_str(argv[0]))
                tlog("        command: %.200s", argv[0]);
        } else if (idx == RS_GetHook && safe_str((const char *)(uintptr_t)args[0])) {
            tlog("        string: %.200s", (const char *)(uintptr_t)args[0]);
        }
    }

    if (idx == RS_SwapBuffers) {
        LONG now = (LONG)GetTickCount();
        LONG last = g_last_summary_tick;
        if (now - last >= SUMMARY_EVERY_MS &&
            InterlockedCompareExchange(&g_last_summary_tick, now, last) == last)
            log_summary("periodic");
    }
    if (idx == RS_Term)
        log_summary("Term");
}

/* Init is wrapped in C (its signature is known) so the result can be logged. */
static int __cdecl hook_init(RenderInit *init)
{
    typedef int (__cdecl *PFN_Init)(RenderInit *);
    int r;
    avp2m_trace_enter(RS_Init, (const uint32_t *)&init);
    tlog("Init request: %lux%lux%lu dll=%.64s device=%.64s",
         (unsigned long)init->mode.width, (unsigned long)init->mode.height,
         (unsigned long)init->mode.bit_depth, init->mode.render_dll,
         init->mode.internal_name);
    r = ((PFN_Init)avp2m_orig_slots[RS_Init])(init);
    tlog("Init result=%d version=%lX (expect %X) engine screen=%lux%lu",
         r, (unsigned long)init->version, AVP2M_RENDER_VERSION,
         (unsigned long)g_rs->screen_width, (unsigned long)g_rs->screen_height);
    return r;
}

/* Installed as Init when the stock renderer can't be loaded, so the engine
 * reports its normal renderer error instead of calling a null pointer. */
static int __cdecl fail_init(RenderInit *init)
{
    (void)init;
    tlog("Init refused: stock renderer not available");
    return 1;
}

/* ------------------------------------------------------------------ */
/* Exports                                                              */
/* ------------------------------------------------------------------ */
__declspec(dllexport) void __cdecl RenderDLLSetup(RenderStruct *rs)
{
    int i;
    if (!load_real()) {
        rs->slots[RS_Init] = (AVP2M_Fn)(void *)fail_init;
        return;
    }

    g_real_setup(rs);
    g_rs = rs;
    for (i = 0; i < RS_SLOT_COUNT; i++) {
        avp2m_orig_slots[i] = rs->slots[i];
        rs->slots[i] = avp2m_thunk_table[i];
    }
    rs->slots[RS_Init] = (AVP2M_Fn)(void *)hook_init;
    InterlockedExchange(&g_setup_done, 1);

    tlog("RenderDLLSetup: render struct at %08lX, %d slots hooked",
         (unsigned long)(uintptr_t)rs, RS_SLOT_COUNT);
    for (i = 0; i < RS_SLOT_COUNT; i++)
        tlog("  slot %2d +0x%03X %-22s stock=%08lX", i, 0x70 + 4 * i,
             g_slot_names[i], (unsigned long)(uintptr_t)avp2m_orig_slots[i]);
    for (i = 0; i < RS_ENGINE_CALLBACK_COUNT; i++)
        tlog("  engine callback %2d +0x%02X = %08lX", i, 4 + 4 * i,
             (unsigned long)(uintptr_t)rs->engine_callbacks[i]);
}

__declspec(dllexport) RenderMode *__cdecl GetSupportedModes(void)
{
    RenderMode *list, *m;
    int count = 0;
    if (!load_real())
        return NULL;
    list = g_real_get_modes();
    for (m = list; m; m = m->next) {
        if (count < 64)
            tlog("mode %2d: %4lux%-4lu %2lu-bit hw=%lu device=%.64s desc=%.64s",
                 count, (unsigned long)m->width, (unsigned long)m->height,
                 (unsigned long)m->bit_depth, (unsigned long)m->hardware,
                 m->internal_name, m->description);
        count++;
    }
    tlog("GetSupportedModes: %d modes", count);
    return list;
}

__declspec(dllexport) void __cdecl FreeModeList(RenderMode *list)
{
    if (g_real_free_modes)
        g_real_free_modes(list);
    /* The engine loads every .ren just to enumerate modes. If this load was
     * only for that, release the stock renderer again. Never inside DllMain. */
    if (!g_setup_done && g_real) {
        FreeLibrary(g_real);
        g_real = NULL;
        g_real_setup = NULL;
        g_real_get_modes = NULL;
        g_real_free_modes = NULL;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        g_start_tick = GetTickCount();
        g_last_summary_tick = (LONG)g_start_tick;
        InitializeCriticalSection(&g_lock);
        g_lock_ready = 1;
        DisableThreadLibraryCalls(inst);
        tlog("avp2m_trace %s loaded", TRACE_VERSION);
    } else if (reason == DLL_PROCESS_DETACH) {
        /* On process exit other threads were killed and may own the lock. */
        if (reserved != NULL)
            g_lock_ready = 0;
        if (g_setup_done)
            log_summary("unload");
        if (g_log != INVALID_HANDLE_VALUE)
            CloseHandle(g_log);
    }
    return TRUE;
}
