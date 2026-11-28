/*
 * Cursor API hooks for the original modules (lithtech.exe, cshell.dll).
 *
 * The engine reads the mouse through DirectInput and only touches the Windows
 * cursor through ClipCursor, SetCursorPos, ShowCursor and SetCursor. Those
 * calls assume the window is the size of the game's display mode. In our
 * upscaled borderless fullscreen it isn't, so the renderer gets a chance to
 * map rectangles and points from game pixels to where the game image is
 * actually shown. Every call is also logged (rate-limited) so a single test
 * run shows what the game does with the cursor.
 *
 * Hooks are import-table patches in the loaded game modules; nothing on disk
 * changes. cshell.dll is loaded from a .rez after startup, so
 * avp2m_hook_cursor_apis is cheap to call repeatedly and only patches modules
 * it hasn't seen.
 */
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <string.h>
#include "cursor_hooks.h"

static cursor_log_fn g_log;
static cursor_map_fn g_map;
static void *g_map_user;
static HMODULE g_self;

typedef BOOL (WINAPI *ClipCursor_t)(const RECT *);
typedef BOOL (WINAPI *SetCursorPos_t)(int, int);
typedef int  (WINAPI *ShowCursor_t)(BOOL);
typedef HCURSOR (WINAPI *SetCursor_t)(HCURSOR);

static ClipCursor_t   real_ClipCursor;
static SetCursorPos_t real_SetCursorPos;
static ShowCursor_t   real_ShowCursor;
static SetCursor_t    real_SetCursor;

static volatile LONG g_ncalls[4];
static volatile LONG g_show_count;     /* game's view: last ShowCursor result */
static volatile LONG g_last_cursor;    /* last cursor handle the game set */

/* Log the first 8 calls of each kind, then calls 16, 32, 64, ... */
static int want_log(int kind, LONG *n)
{
    *n = InterlockedIncrement(&g_ncalls[kind]);
    return g_log && (*n <= 8 || (*n & (*n - 1)) == 0);
}

static BOOL WINAPI hook_ClipCursor(const RECT *r)
{
    RECT m;
    char b[160];
    LONG n;
    if (!r) {
        if (want_log(0, &n)) { wsprintfA(b, "ClipCursor(NULL) [call %ld]", n); g_log(b); }
        return real_ClipCursor(NULL);
    }
    m = *r;
    if (g_map) g_map(g_map_user, CURSOR_MAP_RECT, (long *)&m);
    if (want_log(0, &n)) {
        wsprintfA(b, "ClipCursor(%ld,%ld,%ld,%ld) -> %ld,%ld,%ld,%ld [call %ld]", r->left, r->top, r->right, r->bottom,
                  m.left, m.top, m.right, m.bottom, n);
        g_log(b);
    }
    return real_ClipCursor(&m);
}

static BOOL WINAPI hook_SetCursorPos(int x, int y)
{
    long p[2];
    char b[120];
    LONG n;
    p[0] = x; p[1] = y;
    if (g_map) g_map(g_map_user, CURSOR_MAP_POINT, p);
    if (want_log(1, &n)) { wsprintfA(b, "SetCursorPos(%d,%d) -> %ld,%ld [call %ld]", x, y, p[0], p[1], n); g_log(b); }
    return real_SetCursorPos((int)p[0], (int)p[1]);
}

static int WINAPI hook_ShowCursor(BOOL show)
{
    int r = real_ShowCursor(show);
    char b[80];
    LONG n;
    InterlockedExchange(&g_show_count, r);
    if (want_log(2, &n)) { wsprintfA(b, "ShowCursor(%d) = %d [call %ld]", show, r, n); g_log(b); }
    return r;
}

static HCURSOR WINAPI hook_SetCursor(HCURSOR c)
{
    char b[80];
    LONG n;
    InterlockedExchange(&g_last_cursor, (LONG)(intptr_t)c);
    if (want_log(3, &n)) { wsprintfA(b, "SetCursor(%08lX) [call %ld]", (unsigned long)(uintptr_t)c, n); g_log(b); }
    return real_SetCursor(c);
}

/* Replace one user32 import in a module. Returns 1 if patched. */
static int patch_import(HMODULE mod, const char *name, void *hook, void **orig)
{
    uint8_t *base = (uint8_t *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS32 *nt;
    IMAGE_IMPORT_DESCRIPTOR *imp;
    DWORD rva;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return 0;
    for (imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + rva); imp->Name; imp++) {
        IMAGE_THUNK_DATA32 *names, *addrs;
        if (lstrcmpiA((const char *)(base + imp->Name), "user32.dll")) continue;
        names = (IMAGE_THUNK_DATA32 *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        addrs = (IMAGE_THUNK_DATA32 *)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; names++, addrs++) {
            IMAGE_IMPORT_BY_NAME *ibn;
            DWORD old;
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, name)) continue;
            if ((void *)(uintptr_t)addrs->u1.Function == hook) return 0;
            if (!*orig) *orig = (void *)(uintptr_t)addrs->u1.Function;
            if (!VirtualProtect(&addrs->u1.Function, 4, PAGE_READWRITE, &old)) return 0;
            addrs->u1.Function = (DWORD)(uintptr_t)hook;
            VirtualProtect(&addrs->u1.Function, 4, old, &old);
            return 1;
        }
    }
    return 0;
}

void avp2m_cursor_hooks_init(cursor_log_fn log, cursor_map_fn map, void *user)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    g_log = log;
    g_map = map;
    g_map_user = user;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)(void *)avp2m_cursor_hooks_init, &g_self);
    /* fall back to the real functions if a module's import was already bound elsewhere */
    if (!real_ClipCursor)   real_ClipCursor   = (ClipCursor_t)(void *)GetProcAddress(u32, "ClipCursor");
    if (!real_SetCursorPos) real_SetCursorPos = (SetCursorPos_t)(void *)GetProcAddress(u32, "SetCursorPos");
    if (!real_ShowCursor)   real_ShowCursor   = (ShowCursor_t)(void *)GetProcAddress(u32, "ShowCursor");
    if (!real_SetCursor)    real_SetCursor    = (SetCursor_t)(void *)GetProcAddress(u32, "SetCursor");
}

int avp2m_hook_cursor_apis(void)
{
    static HMODULE seen[64];
    static int nseen;
    HMODULE mods[256];
    DWORD need = 0, i;
    int patched = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return 0;
    if (need > sizeof mods) need = sizeof mods;
    for (i = 0; i < need / sizeof(HMODULE); i++) {
        char path[MAX_PATH], *file;
        int k, known = 0;
        if (mods[i] == g_self) continue;
        for (k = 0; k < nseen; k++) if (seen[k] == mods[i]) { known = 1; break; }
        if (known) continue;
        if (!GetModuleFileNameA(mods[i], path, sizeof path)) continue;
        file = strrchr(path, '\\');
        file = file ? file + 1 : path;
        /* only the game's own modules: the engine and the client shell */
        if (lstrcmpiA(file, "lithtech.exe") && lstrcmpiA(file, "cshell.dll")) continue;
        if (nseen < 64) seen[nseen++] = mods[i];
        patched += patch_import(mods[i], "ClipCursor", (void *)hook_ClipCursor, (void **)&real_ClipCursor);
        patched += patch_import(mods[i], "SetCursorPos", (void *)hook_SetCursorPos, (void **)&real_SetCursorPos);
        patched += patch_import(mods[i], "ShowCursor", (void *)hook_ShowCursor, (void **)&real_ShowCursor);
        patched += patch_import(mods[i], "SetCursor", (void *)hook_SetCursor, (void **)&real_SetCursor);
        if (g_log) { char b[MAX_PATH + 40]; wsprintfA(b, "cursor hooks: %s, %d imports patched so far", file, patched); g_log(b); }
    }
    return patched;
}

int avp2m_game_cursor_visible(void)
{
    return g_show_count >= 0 && g_last_cursor != 0;
}
