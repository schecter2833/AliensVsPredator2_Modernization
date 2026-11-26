/* Fake engine driving the trace proxy the way lithtech.exe does.
 * Original test code. SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../../src/renderer/render_struct.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main(void) {
    typedef int (__cdecl *F3)(int, int, int);
    typedef long long (__cdecl *F64)(void);
    typedef int (__cdecl *FInit)(RenderInit *);
    RenderStruct rs; RenderInit init; RenderMode *modes, *m; int i, n = 0;
    HMODULE h;
    const char *cmd_argv[2] = { "testcommand", NULL };

    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. enumeration pass: load, list modes, free, unload */
    h = LoadLibraryA("avp2m_trace.ren"); CHECK(h != NULL, "load proxy");
    modes = ((PFN_GetSupportedModes)(void*)GetProcAddress(h, "GetSupportedModes"))();
    for (m = modes; m; m = m->next) n++;
    CHECK(n == 2, "expected 2 modes, got %d", n);
    ((PFN_FreeModeList)(void*)GetProcAddress(h, "FreeModeList"))(modes);
    FreeLibrary(h);

    /* 2. real load, setup, init */
    h = LoadLibraryA("avp2m_trace.ren"); CHECK(h != NULL, "reload proxy");
    memset(&rs, 0, sizeof rs);
    for (i = 0; i < RS_ENGINE_CALLBACK_COUNT; i++) rs.engine_callbacks[i] = (AVP2M_Fn)(uintptr_t)(0xE0000000u + i);
    ((PFN_RenderDLLSetup)(void*)GetProcAddress(h, "RenderDLLSetup"))(&rs);
    memset(&init, 0, sizeof init);
    init.mode.width = 1920; init.mode.height = 1080; init.mode.bit_depth = 32;
    lstrcpyA(init.mode.render_dll, "avp2m_trace.ren");
    CHECK(((FInit)(void*)rs.slots[RS_Init])(&init) == 0, "Init result");
    CHECK(init.version == AVP2M_RENDER_VERSION, "Init version %lx", (unsigned long)init.version);

    /* 3. every slot through its thunk: args and return value pass through */
    for (i = 1; i < RS_SLOT_COUNT - 1; i++) {
        int r = ((F3)(void*)rs.slots[i])(i, 7, 1000);
        CHECK(r == (i + 7 + 1000) * 100 + i, "slot %d returned %d", i, r);
    }
    CHECK(((F64)(void*)rs.slots[37])() == 0x1122334455667788LL, "64-bit return through thunk");

    /* 3b. guessed-pointer arguments: a real console command, then garbage
     * pointers that the logger must not dereference */
    CHECK(((F3)(void*)rs.slots[RS_RenderCommand])(1, (int)(intptr_t)cmd_argv, 0) == (1 + (int)(intptr_t)cmd_argv) * 100 + RS_RenderCommand, "RenderCommand passthrough");
    ((F3)(void*)rs.slots[RS_GetHook])(0x10, 0, 0);
    printf("pointer-safety checks survived\n");

    /* 4. many frames, to exercise the periodic summary */
    for (i = 0; i < 4000; i++) ((F3)(void*)rs.slots[RS_SwapBuffers])(0, 0, 0);
    ((F3)(void*)rs.slots[RS_Term])(1, 0, 0);
    FreeLibrary(h);

    printf(fails ? "%d FAILURES\n" : "ALL TESTS PASSED\n", fails);
    return fails != 0;
}
