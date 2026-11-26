/* Fake stock renderer for testing the trace proxy. Original test code.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#include <windows.h>
#include <stdint.h>
#include "../../src/renderer/render_struct.h"

static RenderMode g_modes[2];
volatile int g_last_slot = -1;

static int __cdecl fake_init(RenderInit *init) { init->version = AVP2M_RENDER_VERSION; return init->mode.width == 1920 ? 0 : 1; }
#define SLOT(i) static int __cdecl s##i(int a, int b, int c) { g_last_slot = i; return (a + b + c) * 100 + i; }
SLOT(1) SLOT(2) SLOT(3) SLOT(4) SLOT(5) SLOT(6) SLOT(7) SLOT(8) SLOT(9) SLOT(10)
SLOT(11) SLOT(12) SLOT(13) SLOT(14) SLOT(15) SLOT(16) SLOT(17) SLOT(18) SLOT(19) SLOT(20)
SLOT(21) SLOT(22) SLOT(23) SLOT(24) SLOT(25) SLOT(26) SLOT(27) SLOT(28) SLOT(29) SLOT(30)
SLOT(31) SLOT(32) SLOT(33) SLOT(34) SLOT(35) SLOT(36)
/* a 64-bit return checks edx:eax survives the thunk */
static long long __cdecl s0_64(void) { return 0x1122334455667788LL; }

__declspec(dllexport) void __cdecl RenderDLLSetup(RenderStruct *rs) {
    AVP2M_Fn t[RS_SLOT_COUNT] = { (AVP2M_Fn)(void*)fake_init,
      (AVP2M_Fn)(void*)s1,(AVP2M_Fn)(void*)s2,(AVP2M_Fn)(void*)s3,(AVP2M_Fn)(void*)s4,(AVP2M_Fn)(void*)s5,(AVP2M_Fn)(void*)s6,(AVP2M_Fn)(void*)s7,(AVP2M_Fn)(void*)s8,(AVP2M_Fn)(void*)s9,(AVP2M_Fn)(void*)s10,
      (AVP2M_Fn)(void*)s11,(AVP2M_Fn)(void*)s12,(AVP2M_Fn)(void*)s13,(AVP2M_Fn)(void*)s14,(AVP2M_Fn)(void*)s15,(AVP2M_Fn)(void*)s16,(AVP2M_Fn)(void*)s17,(AVP2M_Fn)(void*)s18,(AVP2M_Fn)(void*)s19,(AVP2M_Fn)(void*)s20,
      (AVP2M_Fn)(void*)s21,(AVP2M_Fn)(void*)s22,(AVP2M_Fn)(void*)s23,(AVP2M_Fn)(void*)s24,(AVP2M_Fn)(void*)s25,(AVP2M_Fn)(void*)s26,(AVP2M_Fn)(void*)s27,(AVP2M_Fn)(void*)s28,(AVP2M_Fn)(void*)s29,(AVP2M_Fn)(void*)s30,
      (AVP2M_Fn)(void*)s31,(AVP2M_Fn)(void*)s32,(AVP2M_Fn)(void*)s33,(AVP2M_Fn)(void*)s34,(AVP2M_Fn)(void*)s35,(AVP2M_Fn)(void*)s36,(AVP2M_Fn)(void*)s0_64 };
    int i; for (i = 0; i < RS_SLOT_COUNT; i++) rs->slots[i] = t[i];
}
__declspec(dllexport) RenderMode *__cdecl GetSupportedModes(void) {
    g_modes[0].width = 1920; g_modes[0].height = 1080; g_modes[0].bit_depth = 32; g_modes[0].next = &g_modes[1];
    lstrcpyA(g_modes[0].internal_name, "FakeDevice"); lstrcpyA(g_modes[0].description, "Fake renderer for tests");
    g_modes[1] = g_modes[0]; g_modes[1].width = 2560; g_modes[1].height = 1440; g_modes[1].next = NULL;
    return g_modes;
}
__declspec(dllexport) void __cdecl FreeModeList(RenderMode *l) { (void)l; }
