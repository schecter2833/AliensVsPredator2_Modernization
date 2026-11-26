/*
 * render_struct.h - Engine <-> renderer interface for LithTech Talon
 * (Aliens versus Predator 2 1.0.9.6 and Primal Hunt).
 *
 * Clean-room description written from observed behaviour of the reference
 * builds listed in docs/hashes.md. Field and slot names are ours and
 * describe behaviour; entries marked TENTATIVE still need confirmation from
 * runtime traces (see src/trace/).
 *
 * Target: 32-bit x86, all functions __cdecl.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef AVP2M_RENDER_STRUCT_H
#define AVP2M_RENDER_STRUCT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value the renderer must write to RenderInit.version during Init. The
 * engine refuses the renderer if it doesn't match. */
#define AVP2M_RENDER_VERSION 0xD5Du

/* ------------------------------------------------------------------ */
/* Display mode record (0x214 bytes). GetSupportedModes returns a     */
/* singly linked list of these. The engine copies each record and     */
/* overwrites render_dll with the file name of the .ren it loaded.    */
/* ------------------------------------------------------------------ */
typedef struct RenderMode {
    uint32_t hardware;              /* 0x000 TENTATIVE: hardware/T&L flag  */
    char     render_dll[256];       /* 0x004 renderer file name           */
    char     internal_name[128];    /* 0x104 device name ("CardDesc")     */
    char     description[128];      /* 0x184 human-readable description   */
    uint32_t width;                 /* 0x204 */
    uint32_t height;                /* 0x208 */
    uint32_t bit_depth;             /* 0x20C */
    struct RenderMode *next;        /* 0x210 */
} RenderMode;

/* Argument to the Init slot. The engine zeroes it, copies the chosen
 * RenderMode in, calls Init, then checks version and the return value. */
typedef struct RenderInit {
    uint32_t   version;             /* 0x000 out: AVP2M_RENDER_VERSION     */
    RenderMode mode;                /* 0x004 in: requested display mode    */
} RenderInit;

/* ------------------------------------------------------------------ */
/* Render struct, owned by the engine, passed to RenderDLLSetup.      */
/* 0x00-0x6F: engine-side data and callbacks (filled by the engine).  */
/* 0x70-0x104: 38 slots filled by the renderer.                       */
/* ------------------------------------------------------------------ */
typedef void (*AVP2M_Fn)(void);

enum RenderSlot {
    RS_Init = 0,              /* int  (RenderInit*)                 0 = ok  */
    RS_Term,                  /* void (?)                                   */
    RS_BindTexture,           /* (texture*, flag) - paired 1:1 with Unbind   */
    RS_UnbindTexture,         /* (texture*)                                 */
    RS_LoadWorldData,         /* TENTATIVE: creates/pages in lightmaps      */
    RS_CreateContext,         /* (ContextInit*) -> context; at level start  */
    RS_DeleteContext,         /* (context) at level end                     */
    RS_Clear,                 /* (rect*, flags, color)                      */
    RS_Start3D,
    RS_End3D,
    RS_IsIn3D,                /* returns current "in 3D" flag               */
    RS_StartOptimized2D,
    RS_EndOptimized2D,
    RS_IsInOptimized2D,
    RS_SetOptimized2DBlend,   /* (mode)                                     */
    RS_GetOptimized2DBlend,   /* (mode* out)                                */
    RS_SetOptimized2DColor,   /* (color)  alpha forced to 0xFF              */
    RS_GetOptimized2DColor,   /* (color* out)                               */
    RS_RenderScene,           /* (SceneDesc*)                               */
    RS_RenderCommand,         /* (argc, argv) console command               */
    RS_GetHook,               /* (name) -> void*; game asks for
                                 "LPDIRECTDRAW" and "BACKBUFFER"            */
    RS_SwapBuffers,           /* (flags) once per frame                     */
    RS_GetInfoFlags,          /* () -> flags (stock renderer returns 0)     */
    RS_GetScreenFormat,       /* (PixelFormat* out)                         */
    RS_CreateSurface,         /* (w, h) -> surface                          */
    RS_DeleteSurface,         /* (surface)                                  */
    RS_GetSurfaceInfo,        /* (surface, w* out, h* out)                  */
    RS_LockSurface,           /* (surface, ...) -> pixels                   */
    RS_UnlockSurface,         /* (surface)                                  */
    RS_OptimizeSurface,       /* (surface, transparent color)               */
    RS_UnoptimizeSurface,     /* (surface)                                  */
    RS_LockScreen,            /* TENTATIVE */
    RS_UnlockScreen,          /* TENTATIVE */
    RS_BlitToScreen,          /* (BlitRequest*) all 2D/menu drawing         */
    RS_WarpToScreen,          /* TENTATIVE */
    RS_MakeScreenShot,        /* (const char* filename)                     */
    RS_ReadConsoleVariables,  /* () re-reads renderer console variables     */
    RS_BlitFromScreen,        /* TENTATIVE: warns on non-optimized surfaces */
    RS_SLOT_COUNT             /* 38 */
};

#define RS_ENGINE_CALLBACK_COUNT 14

typedef struct RenderStruct {
    uint32_t engine_0x00;                                   /* 0x00 */
    AVP2M_Fn engine_callbacks[RS_ENGINE_CALLBACK_COUNT];    /* 0x04-0x38 */
    uint32_t screen_width;                                  /* 0x3C */
    uint32_t screen_height;                                 /* 0x40 */
    uint32_t initialized;                                   /* 0x44 set by engine after Init */
    uint8_t  engine_0x48[0x70 - 0x48];                      /* 0x48-0x6F (cvar links etc.) */
    AVP2M_Fn slots[RS_SLOT_COUNT];                          /* 0x70-0x104 */
} RenderStruct;

/* Compile-time layout checks */
typedef char avp2m_check_mode_size [(sizeof(RenderMode)  == 0x214) ? 1 : -1];
typedef char avp2m_check_init_size [(sizeof(RenderInit)  == 0x218) ? 1 : -1];
typedef char avp2m_check_slots_off [(sizeof(RenderStruct) == 0x108) ? 1 : -1];

/* Renderer DLL exports (all __cdecl) */
typedef void        (*PFN_RenderDLLSetup)(RenderStruct *rs);
typedef RenderMode *(*PFN_GetSupportedModes)(void);
typedef void        (*PFN_FreeModeList)(RenderMode *list);

#ifdef __cplusplus
}
#endif

#endif /* AVP2M_RENDER_STRUCT_H */
