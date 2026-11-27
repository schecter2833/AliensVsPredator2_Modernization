/*
 * In-memory fixes for bugs in the original game modules (object.lto),
 * applied by our renderer DLLs. The files on disk are never changed, so
 * multiplayer file checks still see stock modules.
 *
 * Each fix lists the exact original bytes; a fix is applied only to a loaded
 * module whose bytes match, so other builds are left alone.
 *
 * Fix 1 - Primal Hunt object.lto (RVA 0xD5620, SHA-256 70ec9697...):
 *   A damage-effect update reads a flag from the character attribute table
 *   (0x7D8-byte entries) using the object's character index. For objects that
 *   are neither CCharacter nor BodyProp the index is -1, so it reads 0x2B0
 *   bytes before the table. The table is a large, page-aligned allocation on
 *   current Windows, so that read faults ("Zeta Site" crashes on load, with
 *   the stock renderer too). The patch treats a negative index as "flag not
 *   set" (0) and is otherwise identical.
 */
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <string.h>
#include "game_fixes.h"

typedef struct GameFix {
    const char *name;
    uint32_t rva;                 /* start of the patched bytes */
    uint32_t len;
    const uint8_t *orig;
    const uint8_t *patch;
    int reloc_off;                /* offset of a relocated absolute address in orig/patch, or -1 */
    uint32_t reloc_rva;           /* the RVA that address points at (orig: at reloc_off; patch: at patch_reloc_off) */
    int patch_reloc_off;
} GameFix;

/* PH object.lto 0x100D56F3..0x100D5723 */
static const uint8_t k_ph_dmg_orig[49] = {
    0x8b, 0x15, 0xdc, 0x22, 0x30, 0x10,             /* mov edx, [0x103022dc]        */
    0x8b, 0xc8,                                     /* mov ecx, eax                 */
    0xc1, 0xe1, 0x06,                               /* shl ecx, 6                   */
    0x2b, 0xc8,                                     /* sub ecx, eax                 */
    0x8d, 0xb3, 0xbc, 0x02, 0x00, 0x00,             /* lea esi, [ebx+0x2bc]         */
    0xc1, 0xe1, 0x02,                               /* shl ecx, 2                   */
    0x2b, 0xc8,                                     /* sub ecx, eax  (ecx = i*251)  */
    0x8b, 0x82, 0x74, 0x11, 0x00, 0x00,             /* mov eax, [edx+0x1174]        */
    0xc7, 0x44, 0x24, 0x18, 0x14, 0x00, 0x00, 0x00, /* mov [esp+0x18], 20           */
    0x8b, 0x8c, 0xc8, 0x28, 0x05, 0x00, 0x00,       /* mov ecx, [eax+ecx*8+0x528]   */
    0x89, 0x4c, 0x24, 0x24,                         /* mov [esp+0x24], ecx          */
};
static const uint8_t k_ph_dmg_patch[49] = {
    0x8d, 0xb3, 0xbc, 0x02, 0x00, 0x00,             /* lea esi, [ebx+0x2bc]         */
    0xc7, 0x44, 0x24, 0x18, 0x14, 0x00, 0x00, 0x00, /* mov [esp+0x18], 20           */
    0x31, 0xc9,                                     /* xor ecx, ecx                 */
    0x85, 0xc0,                                     /* test eax, eax                */
    0x78, 0x19,                                     /* js  store (index < 0)        */
    0x69, 0xc0, 0xd8, 0x07, 0x00, 0x00,             /* imul eax, eax, 0x7d8         */
    0x8b, 0x15, 0xdc, 0x22, 0x30, 0x10,             /* mov edx, [0x103022dc]        */
    0x8b, 0x92, 0x74, 0x11, 0x00, 0x00,             /* mov edx, [edx+0x1174]        */
    0x8b, 0x8c, 0x02, 0x28, 0x05, 0x00, 0x00,       /* mov ecx, [edx+eax+0x528]     */
    0x89, 0x4c, 0x24, 0x24,                         /* store: mov [esp+0x24], ecx   */
};

static const GameFix k_fixes[] = {
    { "Primal Hunt object.lto: character table read with index -1 (Zeta Site crash)",
      0xD56F3, 49, k_ph_dmg_orig, k_ph_dmg_patch, 2, 0x3022DC, 28 },
};

static int g_disabled;

/* Every byte of [p, p+n) committed and readable. Modules can have unreadable
   ranges inside SizeOfImage, and a fault on the watcher thread would end the
   process (the engine's crash handler only covers its main thread). */
static int readable_range(const void *p, size_t n)
{
    const uint8_t *a = (const uint8_t *)p, *end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        const uint8_t *rend;
        if (!VirtualQuery(a, &mbi, sizeof mbi)) return 0;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
            return 0;
        rend = (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (rend <= a) return 0;
        a = rend;
    }
    return 1;
}

static int apply_fix(const GameFix *f, uint8_t *base, uint32_t size, game_fix_log_fn log)
{
    uint8_t want[64], put[64];
    uint8_t *p;
    DWORD old;
    if (f->rva + f->len > size || f->len > sizeof want) return 0;
    p = base + f->rva;
    if (!readable_range(p, f->len)) return 0;
    memcpy(want, f->orig, f->len);
    memcpy(put, f->patch, f->len);
    if (f->reloc_off >= 0) {   /* the loader relocated this absolute address */
        uint32_t abs = (uint32_t)(uintptr_t)base + f->reloc_rva;
        memcpy(want + f->reloc_off, &abs, 4);
        memcpy(put + f->patch_reloc_off, &abs, 4);
    }
    if (memcmp(p, put, f->len) == 0) return 1;            /* already patched */
    if (memcmp(p, want, f->len) != 0) return 0;           /* different build */
    if (!VirtualProtect(p, f->len, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(p, put, f->len);
    VirtualProtect(p, f->len, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, f->len);
    if (log) log(f->name, (void *)p);
    return 1;
}

int avp2m_apply_game_fixes(game_fix_log_fn log)
{
    /* K32EnumProcessModules reads this process's own loader list; unlike a
       Toolhelp snapshot it starts no helper thread, which deadlocked against
       DLL loading during renderer Init. */
    HMODULE mods[512];
    DWORD need = 0, i, n;
    int applied = 0;
    if (g_disabled) return 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return 0;
    n = need / sizeof mods[0];
    if (n > 512) n = 512;
    for (i = 0; i < n; i++) {
        MODULEINFO mi;
        size_t k;
        if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi)) continue;
        for (k = 0; k < sizeof k_fixes / sizeof k_fixes[0]; k++)
            if (apply_fix(&k_fixes[k], (uint8_t *)mi.lpBaseOfDll, mi.SizeOfImage, log)) applied++;
    }
    return applied;
}

void avp2m_disable_game_fixes(int disabled) { g_disabled = disabled; }

/* Background watcher: object.lto is loaded and starts running during a level
 * load, possibly before the renderer is called again, so check every 15 ms.
 * The thread doesn't log; it records what it applied for the renderer's own
 * thread to report (avp2m_game_fix_news). */
static volatile LONG g_news;
static const char *volatile g_news_name;
static void *volatile g_news_where;
static void watcher_log(const char *name, void *where)
{
    g_news_name = name;
    g_news_where = where;
    InterlockedIncrement(&g_news);
}
static DWORD WINAPI watcher(LPVOID unused)
{
    (void)unused;
    for (;;) {
        avp2m_apply_game_fixes(watcher_log);
        Sleep(15);
    }
    return 0;
}
void avp2m_start_game_fix_watcher(void)
{
    static LONG started;
    HANDLE h;
    if (InterlockedExchange(&started, 1)) return;
    h = CreateThread(NULL, 0, watcher, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
int avp2m_game_fix_news(const char **name, void **where)
{
    static LONG seen;
    LONG n = g_news;
    if (n == seen) return 0;
    seen = n;
    if (name) *name = g_news_name;
    if (where) *where = g_news_where;
    return 1;
}
