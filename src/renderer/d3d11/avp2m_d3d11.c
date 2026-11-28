/*
 * avp2m_d3d11.ren - Direct3D 11 renderer for AVP2 1.0.9.6 / Primal Hunt.
 *
 * Milestone 1: owns the window and presentation, draws all 2D (menus, HUD,
 * text) natively, offers modern display modes, presents aspect-correct with
 * vsync. The 3D world (RenderScene) is not implemented yet and draws nothing.
 * Video playback hooks return NULL, which makes the engine skip videos.
 *
 * Interface: see src/renderer/render_struct.h and docs/renderer-interface.md.
 *
 * Original code. SPDX-License-Identifier: GPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "../../compat/game_fixes.h"
#include "../../compat/cursor_hooks.h"
#include <stddef.h>
#include <string.h>
#include <math.h>

#include "../render_struct.h"

#define RENDERER_VERSION "0.3.0-dev"
#define MAX_QUADS_PER_BATCH 4096

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */
static HMODULE g_self;
static HANDLE  g_log = INVALID_HANDLE_VALUE;
static DWORD   g_t0;

static void rlog(const char *fmt, ...)
{
    char buf[1024], line[1100];
    va_list ap;
    DWORD w;
    if (g_log == INVALID_HANDLE_VALUE) {
        char path[MAX_PATH];
        DWORD n = GetModuleFileNameA(g_self, path, MAX_PATH - 32);
        while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') n--;
        path[n] = 0;
        lstrcatA(path, "avp2m_d3d11.log");
        g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log == INVALID_HANDLE_VALUE) return;
    }
    va_start(ap, fmt);
    wvsprintfA(buf, fmt, ap);
    va_end(ap);
    wsprintfA(line, "[%7lu ms] %s\r\n", GetTickCount() - g_t0, buf);
    WriteFile(g_log, line, (DWORD)lstrlenA(line), &w, NULL);
}

/* ------------------------------------------------------------------ */
/* State                                                                */
/* ------------------------------------------------------------------ */
typedef struct Surface {
    uint32_t w, h, pitch;
    uint8_t *pixels;              /* X8R8G8B8, engine writes here when locked */
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t tex_key;             /* key the texture was built with           */
    int dirty;
    uint32_t opt_key;             /* key from OptimizeSurface                 */
    struct Surface *next, *prev;
} Surface;

typedef struct Vertex { float x, y, u, v; uint32_t rgba; } Vertex;

static RenderStruct *g_rs;
static HWND  g_hwnd;
static int   g_windowed;
static uint32_t g_w = 640, g_h = 480;          /* game render resolution */
/* Widescreen (cvar avp2m_widescreen, default 1): the game only offers 4:3 modes, so the game target is
 * made as wide as the window's aspect (g_tw x g_h). Full-screen 3D scenes fill all of it with a wider
 * horizontal FOV (Hor+); everything 2D stays 4:3 in the middle, g_xoff pixels from the left. */
static uint32_t g_tw = 640, g_xoff = 0;
static int g_widescreen = 1;
/* HUD to the screen edges (cvar avp2m_hudedges, default 1). In a widescreen frame with a full-screen
 * scene, 2D blits after the scene are collected, grouped into elements (runs of glyphs from one surface
 * on one line), and each element moves to the left or right screen edge when it sits in that part of the
 * 4:3 screen. Full-screen overlays are stretched to the whole width. */
typedef struct HudQuad { ID3D11ShaderResourceView *srv; int blend; float x0, y0, x1, y1, u0, v0, u1, v1; uint32_t rgba; } HudQuad;
#define MAX_HUD_QUADS 8192
static HudQuad g_hud[MAX_HUD_QUADS];
static int g_nhud, g_hud_frame, g_hud_edges = 1;
static float g_disp_x0, g_disp_y0, g_disp_s;   /* game pixel (0,0) and scale in the window, from the last Present */

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain      *g_swap;
static ID3D11RenderTargetView *g_bb_rtv;
static uint32_t g_bb_w, g_bb_h;

static ID3D11Texture2D          *g_rt_tex;     /* game-resolution render target */
static ID3D11Texture2D          *g_scr_staging;   /* LockScreen readback */
static uint8_t *g_scr_buf;
static uint32_t g_scr_w, g_scr_h;
/* HDR (cvar avp2m_hdr): the 3D scene renders into a float target and is
 * tone mapped into g_rt before the HUD is drawn, so the 2D layer stays exact. */
static ID3D11Texture2D          *g_hdr_tex;
static ID3D11RenderTargetView   *g_hdr_rtv;
static ID3D11ShaderResourceView *g_hdr_srv;
static ID3D11VertexShader       *g_vs_fs;
static ID3D11PixelShader        *g_ps_tonemap;
static ID3D11Buffer             *g_cb_tm;
static int   g_hdr;                     /* 0 off, 1 ACES filmic, 2 Reinhard */
static float g_exposure = 1.0f;         /* cvar avp2m_exposure x100 */
static const char k_hlsl_tm[] =
    "cbuffer T : register(b0) { float4 p; };\n"   /* x exposure, y mode */
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD; };\n"
    "VO vs(uint id : SV_VertexID) { VO o; float2 q = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "float3 aces(float3 x) { return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14)); }\n"
    "float4 ps(VO i) : SV_Target { float3 c = t.Sample(s, i.uv).rgb * p.x;\n"
    /* the game's art is authored in display (gamma) space: tone map in linear, then back */
    "  float3 lin = pow(max(c, 0), 2.2);\n"
    "  float3 m = p.y > 1.5 ? lin / (1 + lin) * 1.25 : aces(lin * 0.8);\n"
    "  return float4(pow(saturate(m), 1 / 2.2), 1); }\n";
static ID3D11RenderTargetView   *g_rt_rtv;
static ID3D11ShaderResourceView *g_rt_srv;

static ID3D11VertexShader *g_vs;
static ID3D11PixelShader  *g_ps;
static ID3D11InputLayout  *g_layout;
static ID3D11Buffer       *g_vb;
static ID3D11Buffer       *g_cb;
static ID3D11SamplerState *g_samp_point, *g_samp_linear, *g_samp_wrap;
static ID3D11BlendState   *g_blend[OPT2D_BLEND_COUNT];
static ID3D11RasterizerState *g_rast;
static ID3D11Texture2D *g_white_tex;
static ID3D11ShaderResourceView *g_white_srv;

static UINT g_vb_pos;                          /* in vertices */

static Surface g_surfaces = { 0 };             /* list head   */

/* 3D world (milestone 2, step 1: untextured geometry) */
typedef struct WVertex { float x, y, z, u, v, lu, lv; uint32_t rgba; } WVertex;

/* Renderer-side texture, created from engine TextureData. Its pointer is
 * stored in SharedTexture+0x0C, like the stock renderer does. */
typedef struct RTexture {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t w, h;                   /* uploaded size (top mip the engine gave us) */
    uint32_t bw, bh;                 /* DTX base size; larger when detail settings drop top mips */
    void *shared;                    /* engine SharedTexture */
    struct RTexture *next;
} RTexture;
static RTexture *g_textures;

typedef struct DrawBatch { RTexture *tex; int lmpage; int bsp; UINT first, count; } DrawBatch;

/* Lightmap atlas pages (built per world at load). */
#define LM_PAGE 2048
typedef struct LMPage {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
} LMPage;
typedef struct RenderContext {
    void *world;                     /* engine world (first field of the init block) */
    ID3D11Buffer *vb;
    UINT vcount;
    DrawBatch *batches;
    UINT nbatches;
    LMPage *lmpages;
    int nlmpages;
    ID3D11Buffer *cvb;               /* vertex colours (stream 1); alpha 1 = lightmapped */
    const uint8_t **bsps;            /* WorldBsp pointer per BSP index */
    uint32_t nbsps;
    const float **bsp_xf;            /* per frame: world-model transform (engine 4x4) or NULL */
    uint8_t *sky;                    /* per frame: 1 = BSP belongs to the sky */
    uint32_t *vcol;                  /* CPU copy, for light animation updates */
    int built;
    /* animated light (flicker anims): polygons that reference a light animation
       other than LightAnim_BASE are recomputed when an animation changes */
    struct DynLM { const uint8_t *poly; int page, x, y, w, h; } *dlm;
    uint32_t ndlm;
    struct DynGV { const uint8_t *poly; uint32_t nv, first; } *dgv;
    uint32_t ndgv, ndgv_cols;
    struct DynVert { uint32_t vcol; uint32_t col; } *dvert;   /* col = dgv_cols index */
    uint32_t ndvert;
    uint32_t *anim_state;            /* last seen frame A/B/blend/intensity per anim */
    uint32_t nanim_state;
} RenderContext;
static RenderContext *g_cur_ctx;
static float g_lmscale = 1.0f;       /* cvar avp2m_lmscale x100 */
static float g_sun_dir[3], g_sun_col[3];   /* RenderStruct +0x138 / +0x144, read per frame */
static int g_sun_on;
static float g_model_sat = 2.0f;        /* model color multiplier (stock: 2x); cvar avp2m_modelsat x100 */
static int g_lmdebug;                /* cvar avp2m_lmdebug: 1 light only, 2 vertex colour only */
static ID3D11VertexShader *g_vs3d;
static ID3D11PixelShader  *g_ps3d, *g_ps_spr, *g_ps_mdl;
static ID3D11DepthStencilState *g_ds_test;      /* depth test, no depth write */
static ID3D11Buffer *g_cbw;                     /* b1: object-to-world rows (3 x float4) */
#define MAX_SPRITES 16384             /* sprites plus particles */
static ID3D11Buffer *g_spr_vb, *g_spr_cvb;
#define MAX_MODEL_VERTS (3 * 120000)
static ID3D11Buffer *g_mdl_vb, *g_mdl_cvb;
static ID3D11InputLayout  *g_layout3d;
static ID3D11Buffer       *g_cb3d, *g_cbdl;
static ID3D11Texture2D    *g_depth_tex;
static ID3D11DepthStencilView *g_dsv;
static ID3D11DepthStencilState *g_ds_on, *g_ds_off;

static int g_in3d, g_in_opt2d;
static int g_opt_blend = OPT2D_ALPHA;
static uint32_t g_opt_color = 0xFFFFFFFFu;     /* A8R8G8B8 */
static int g_vsync = 1;

/* ------------------------------------------------------------------ */
/* Engine console variables                                             */
/* ------------------------------------------------------------------ */
typedef void *(__cdecl *PFN_GetVar)(const char *);
typedef const char *(__cdecl *PFN_GetVarString)(void *);

static const char *cvar_string(const char *name)
{
    void *v;
    if (!g_rs) return NULL;
    v = ((PFN_GetVar)(void *)g_rs->engine_callbacks[RS_CB_GET_CONSOLE_VAR])(name);
    if (!v) return NULL;
    return ((PFN_GetVarString)(void *)g_rs->engine_callbacks[RS_CB_GET_CONSOLE_VAR_STRING])(v);
}

static int cvar_int(const char *name, int def)
{
    const char *s = cvar_string(name);
    return (s && *s) ? atoi(s) : def;
}

/* ------------------------------------------------------------------ */
/* Shaders                                                              */
/* ------------------------------------------------------------------ */
static const char k_hlsl3d[] =
    "cbuffer C : register(b0) { float4x4 vp; float4 lmscale; };\n"
    "Texture2D t : register(t0); Texture2D lm : register(t1);\n"
    "SamplerState s : register(s0); SamplerState sl : register(s1);\n"
    "struct VI { float3 p : POSITION; float2 uv : TEXCOORD0; float2 luv : TEXCOORD1; float4 c : COLOR; };\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 luv : TEXCOORD1; float4 c : COLOR; float3 wp : TEXCOORD2; };\n"
    "cbuffer W : register(b1) { float4 w0, w1, w2; };\n"
    "cbuffer D : register(b2) { float4 dlc; float4 dlp[16]; float4 dlcol[16]; float4 sund; float4 sunc; };\n"
    "float3 dyn(float3 wp) { float3 l = 0; int n = (int)dlc.x; if (n <= 0) return l;\n"
    "  float3 nn = normalize(cross(ddx(wp), ddy(wp)));\n"
    "  for (int k = 0; k < n; k++) { float3 d = dlp[k].xyz - wp; float dist = length(d) + 1e-4;\n"
    "    float att = saturate(1 - dist / dlp[k].w); float ndl = saturate(dot(nn, d / dist));\n"
    "    l += dlcol[k].rgb * att * ndl; }\n"
    "  return l * dlc.y; }\n"
    "VO vs(VI i) { VO o; float4 p = float4(i.p, 1);\n"
    "  float3 wp = float3(dot(w0, p), dot(w1, p), dot(w2, p));\n"
    "  o.p = mul(float4(wp, 1), vp); o.uv = i.uv; o.luv = i.luv; o.c = i.c; o.wp = wp; return o; }\n"
    "float4 ps(VO i) : SV_Target { float4 c = t.Sample(s, i.uv); clip(c.a - 0.5);\n"
    "  float3 lmc = lm.Sample(sl, i.luv).rgb * lmscale.x * i.c.a;\n"
    "  float3 l = (i.c.a > 0.5 ? lmc : i.c.rgb) + dyn(i.wp);\n"
    "  if (sunc.w > 0.5 && i.c.a < 0.5) l += sunc.rgb * saturate(dot(normalize(cross(ddx(i.wp), ddy(i.wp))), sund.xyz));\n"
    "  if (lmscale.y > 3.5) return float4(lerp(dot(c.rgb, 0.33).xxx, i.c.a > 0.5 ? float3(1,0.2,0.2) : float3(0.2,1,0.2), 0.5), 1);\n"
    "  if (lmscale.y > 2.5) return float4(lmc, 1);\n"
    "  if (lmscale.y > 1.5) return float4(i.c.rgb, 1);\n"
    "  if (lmscale.y > 0.5) return float4(saturate(l), 1);\n"
    "  float3 o = c.rgb * l; return float4(lmscale.z > 0.5 ? o : saturate(o), 1); }\n"
    "float4 ps_spr(VO i) : SV_Target { return t.Sample(s, i.uv) * i.c; }\n"
    "float4 ps_mdl(VO i) : SV_Target { float4 c = t.Sample(s, i.uv); float3 o = c.rgb * i.c.rgb;\n"
    "  return float4(lmscale.z > 0.5 ? o : saturate(o), c.a * i.c.a); }\n";

static const char k_hlsl[] =
    "cbuffer C : register(b0) { float4 xf; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct VI { float2 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "VO vs(VI i) { VO o; o.p = float4(i.p * xf.xy + xf.zw, 0, 1); o.uv = i.uv; o.c = i.c; return o; }\n"
    "float4 ps(VO i) : SV_Target { return t.Sample(s, i.uv) * i.c; }\n";

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *,
                                         ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT,
                                         ID3DBlob **, ID3DBlob **);

static int create_shaders(void)
{
    static HMODULE comp;
    PFN_D3DCompile compile;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_INPUT_ELEMENT_DESC il[3] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    HRESULT hr;

    if (!comp) comp = LoadLibraryA("d3dcompiler_47.dll");
    if (!comp) { rlog("ERROR: d3dcompiler_47.dll not found"); return 0; }
    compile = (PFN_D3DCompile)(void *)GetProcAddress(comp, "D3DCompile");
    if (!compile) return 0;

    hr = compile(k_hlsl, sizeof(k_hlsl) - 1, "avp2m", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { rlog("VS compile failed: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
    hr = compile(k_hlsl, sizeof(k_hlsl) - 1, "avp2m", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { rlog("PS compile failed: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }

    hr = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateInputLayout(g_dev, il, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hr)) return 0;

    {
        D3D11_INPUT_ELEMENT_DESC il3[4] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT,    0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  1, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        vsb = psb = err = NULL;
        hr = compile(k_hlsl3d, sizeof(k_hlsl3d) - 1, "avp2m3d", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
        if (SUCCEEDED(hr)) hr = compile(k_hlsl3d, sizeof(k_hlsl3d) - 1, "avp2m3d", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
        if (FAILED(hr)) { rlog("3D shader compile failed: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
        hr = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs3d);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps3d);
        if (SUCCEEDED(hr)) {
            ID3DBlob *sb = NULL;
            hr = compile(k_hlsl3d, sizeof(k_hlsl3d) - 1, "avp2m3d", NULL, NULL, "ps_spr", "ps_4_0", 0, 0, &sb, &err);
            if (SUCCEEDED(hr)) {
                hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(sb), ID3D10Blob_GetBufferSize(sb), NULL, &g_ps_spr);
                ID3D10Blob_Release(sb);
            } else {
                rlog("sprite shader compile failed: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
            }
            sb = NULL;
            if (SUCCEEDED(compile(k_hlsl3d, sizeof(k_hlsl3d) - 1, "avp2m3d", NULL, NULL, "ps_mdl", "ps_4_0", 0, 0, &sb, &err))) {
                ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(sb), ID3D10Blob_GetBufferSize(sb), NULL, &g_ps_mdl);
                ID3D10Blob_Release(sb);
            } else {
                rlog("translucent model shader compile failed: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
            }
        }
        if (SUCCEEDED(hr)) {
            ID3DBlob *tv = NULL, *tp = NULL;
            if (SUCCEEDED(compile(k_hlsl_tm, sizeof(k_hlsl_tm) - 1, "avp2mtm", NULL, NULL, "vs", "vs_4_0", 0, 0, &tv, &err)) &&
                SUCCEEDED(compile(k_hlsl_tm, sizeof(k_hlsl_tm) - 1, "avp2mtm", NULL, NULL, "ps", "ps_4_0", 0, 0, &tp, &err))) {
                ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(tv), ID3D10Blob_GetBufferSize(tv), NULL, &g_vs_fs);
                ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(tp), ID3D10Blob_GetBufferSize(tp), NULL, &g_ps_tonemap);
            } else {
                rlog("tone map shader compile failed: %s (HDR unavailable)", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
            }
            if (tv) ID3D10Blob_Release(tv);
            if (tp) ID3D10Blob_Release(tp);
        }
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_dev, il3, 4, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_layout3d);
        ID3D10Blob_Release(vsb);
        ID3D10Blob_Release(psb);
    }
    return SUCCEEDED(hr);
}

/* ------------------------------------------------------------------ */
/* Device objects                                                       */
/* ------------------------------------------------------------------ */
#define SAFE_RELEASE(p, kind) do { if (p) { kind##_Release(p); (p) = NULL; } } while (0)

static void make_blend(int mode, D3D11_BLEND src, D3D11_BLEND dst, BOOL enable)
{
    D3D11_BLEND_DESC d;
    memset(&d, 0, sizeof d);
    d.RenderTarget[0].BlendEnable = enable;
    d.RenderTarget[0].SrcBlend = src;
    d.RenderTarget[0].DestBlend = dst;
    d.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    d.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    d.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    d.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11Device_CreateBlendState(g_dev, &d, &g_blend[mode]);
}

static int create_objects(void)
{
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_RASTERIZER_DESC rd;
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    uint32_t white = 0xFFFFFFFFu;

    if (!create_shaders()) return 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(Vertex) * 6 * MAX_QUADS_PER_BATCH;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_vb))) return 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 16;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb))) return 0;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = 80;               /* float4x4 vp + float4 lmscale */
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb3d))) return 0;
    bd.ByteWidth = 48;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cbw))) return 0;
    bd.ByteWidth = 16 + 32 * 16 + 32;   /* dynamic lights: count/scale, 16 positions+radius, 16 colours; sun dir, colour */
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cbdl))) return 0;
    bd.ByteWidth = 16;
    ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb_tm);
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(WVertex) * 6 * MAX_SPRITES;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_spr_vb))) return 0;
    bd.ByteWidth = 4 * 6 * MAX_SPRITES;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_spr_cvb))) return 0;
    bd.ByteWidth = sizeof(WVertex) * MAX_MODEL_VERTS;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_mdl_vb))) return 0;
    bd.ByteWidth = 4 * MAX_MODEL_VERTS;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_mdl_cvb))) return 0;
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_ds_on);
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_ds_test);
        dd.DepthEnable = FALSE;
        ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_ds_off);
    }

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_linear);
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.MaxAnisotropy = 8;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_wrap);

    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = FALSE;
    rd.DepthClipEnable = TRUE;
    ID3D11Device_CreateRasterizerState(g_dev, &rd, &g_rast);

    /* Blend modes, matching the stock renderer's SetOptimized2DBlend. */
    make_blend(OPT2D_ALPHA,     D3D11_BLEND_SRC_ALPHA,  D3D11_BLEND_INV_SRC_ALPHA, TRUE);
    make_blend(OPT2D_SOLID,     D3D11_BLEND_ONE,        D3D11_BLEND_ZERO,          FALSE);
    make_blend(OPT2D_ADD,       D3D11_BLEND_ONE,        D3D11_BLEND_ONE,           TRUE);
    make_blend(OPT2D_MULTIPLY,  D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO,          TRUE);
    make_blend(OPT2D_MULTIPLY2, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_SRC_COLOR,     TRUE);
    make_blend(OPT2D_MASK,      D3D11_BLEND_ZERO,       D3D11_BLEND_INV_SRC_COLOR, TRUE);
    make_blend(OPT2D_MASKADD,   D3D11_BLEND_ONE,        D3D11_BLEND_INV_SRC_COLOR, TRUE);

    memset(&td, 0, sizeof td);
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    init.pSysMem = &white;
    init.SysMemPitch = 4;
    init.SysMemSlicePitch = 4;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &g_white_tex))) return 0;
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_white_tex, NULL, &g_white_srv);
    return 1;
}

static int create_game_target(void)
{
    D3D11_TEXTURE2D_DESC td;
    g_tw = g_w;
    if (g_widescreen && g_bb_w && g_bb_h && (uint64_t)g_bb_w * g_h > (uint64_t)g_bb_h * g_w + g_bb_h) {
        g_tw = (uint32_t)((double)g_h * g_bb_w / g_bb_h + 0.5) & ~1u;
        if (g_tw > 8192) g_tw = 8192;
        if (g_tw < g_w) g_tw = g_w;
    }
    g_xoff = (g_tw - g_w) / 2;
    memset(&td, 0, sizeof td);
    td.Width = g_tw;
    td.Height = g_h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_rt_tex))) return 0;
    if (g_tw != g_w) rlog("widescreen: game target %lux%lu, 4:3 area at x %lu", (unsigned long)g_tw, (unsigned long)g_h, (unsigned long)g_xoff);
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_rt_tex, NULL, &g_rt_rtv);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_rt_tex, NULL, &g_rt_srv);
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_hdr_tex))) {
        ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_hdr_tex, NULL, &g_hdr_rtv);
        ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_hdr_tex, NULL, &g_hdr_srv);
    }
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_depth_tex)))
        ID3D11Device_CreateDepthStencilView(g_dev, (ID3D11Resource *)g_depth_tex, NULL, &g_dsv);
    return g_rt_rtv && g_rt_srv && g_dsv;
}

static int create_backbuffer_view(void)
{
    ID3D11Texture2D *bb = NULL;
    DXGI_SWAP_CHAIN_DESC sd;
    if (FAILED(IDXGISwapChain_GetBuffer(g_swap, 0, &IID_ID3D11Texture2D, (void **)&bb))) return 0;
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)bb, NULL, &g_bb_rtv);
    ID3D11Texture2D_Release(bb);
    IDXGISwapChain_GetDesc(g_swap, &sd);
    g_bb_w = sd.BufferDesc.Width;
    g_bb_h = sd.BufferDesc.Height;
    return g_bb_rtv != NULL;
}

static void client_size(uint32_t *w, uint32_t *h)
{
    RECT r;
    GetClientRect(g_hwnd, &r);
    *w = (uint32_t)(r.right - r.left);
    *h = (uint32_t)(r.bottom - r.top);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

static int create_swapchain(void)
{
    IDXGIDevice *dxdev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *f2 = NULL;
    IDXGIFactory *f1 = NULL;
    uint32_t cw, ch;
    HRESULT hr = E_FAIL;

    client_size(&cw, &ch);
    ID3D11Device_QueryInterface(g_dev, &IID_IDXGIDevice, (void **)&dxdev);
    IDXGIDevice_GetAdapter(dxdev, &adapter);
    IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory, (void **)&f1);

    if (SUCCEEDED(IDXGIFactory_QueryInterface(f1, &IID_IDXGIFactory2, (void **)&f2))) {
        DXGI_SWAP_CHAIN_DESC1 d;
        IDXGISwapChain1 *sc1 = NULL;
        memset(&d, 0, sizeof d);
        d.Width = cw;
        d.Height = ch;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)g_dev, g_hwnd, &d, NULL, NULL, &sc1);
        if (FAILED(hr)) {               /* pre-Windows 10: flip sequential */
            d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            hr = IDXGIFactory2_CreateSwapChainForHwnd(f2, (IUnknown *)g_dev, g_hwnd, &d, NULL, NULL, &sc1);
        }
        if (SUCCEEDED(hr)) {
            IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain, (void **)&g_swap);
            IDXGISwapChain1_Release(sc1);
        }
        IDXGIFactory2_Release(f2);
    }
    if (!g_swap) {                      /* legacy blt model */
        DXGI_SWAP_CHAIN_DESC d;
        memset(&d, 0, sizeof d);
        d.BufferDesc.Width = cw;
        d.BufferDesc.Height = ch;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 1;
        d.OutputWindow = g_hwnd;
        d.Windowed = TRUE;
        d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        hr = IDXGIFactory_CreateSwapChain(f1, (IUnknown *)g_dev, &d, &g_swap);
    }
    if (g_swap)
        IDXGIFactory_MakeWindowAssociation(f1, g_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    IDXGIFactory_Release(f1);
    IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxdev);
    if (!g_swap) { rlog("ERROR: CreateSwapChain failed hr=%08lX", (unsigned long)hr); return 0; }
    return create_backbuffer_view();
}

static void release_surface_textures(void)
{
    Surface *s;
    for (s = g_surfaces.next; s && s != &g_surfaces; s = s->next) {
        SAFE_RELEASE(s->srv, ID3D11ShaderResourceView);
        SAFE_RELEASE(s->tex, ID3D11Texture2D);
        s->dirty = 1;
    }
}

static void release_all_textures(void);
static void release_device(void)
{
    int i;
    release_surface_textures();
    if (g_ctx) ID3D11DeviceContext_ClearState(g_ctx);
    SAFE_RELEASE(g_white_srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_white_tex, ID3D11Texture2D);
    for (i = 0; i < OPT2D_BLEND_COUNT; i++) SAFE_RELEASE(g_blend[i], ID3D11BlendState);
    SAFE_RELEASE(g_rast, ID3D11RasterizerState);
    SAFE_RELEASE(g_samp_point, ID3D11SamplerState);
    SAFE_RELEASE(g_samp_linear, ID3D11SamplerState);
    SAFE_RELEASE(g_samp_wrap, ID3D11SamplerState);
    SAFE_RELEASE(g_cb, ID3D11Buffer);
    SAFE_RELEASE(g_vb, ID3D11Buffer);
    SAFE_RELEASE(g_layout, ID3D11InputLayout);
    SAFE_RELEASE(g_ps, ID3D11PixelShader);
    SAFE_RELEASE(g_vs, ID3D11VertexShader);
    if (g_cur_ctx) {
        SAFE_RELEASE(g_cur_ctx->vb, ID3D11Buffer);
        free(g_cur_ctx->batches);
        g_cur_ctx->batches = NULL;
        g_cur_ctx->nbatches = 0;
        g_cur_ctx->built = 0;
    }
    release_all_textures();
    SAFE_RELEASE(g_dsv, ID3D11DepthStencilView);
    SAFE_RELEASE(g_depth_tex, ID3D11Texture2D);
    SAFE_RELEASE(g_ds_on, ID3D11DepthStencilState);
    SAFE_RELEASE(g_ds_off, ID3D11DepthStencilState);
    SAFE_RELEASE(g_cb3d, ID3D11Buffer);
    SAFE_RELEASE(g_cbdl, ID3D11Buffer);
    SAFE_RELEASE(g_layout3d, ID3D11InputLayout);
    SAFE_RELEASE(g_ps3d, ID3D11PixelShader);
    SAFE_RELEASE(g_ps_spr, ID3D11PixelShader);
    SAFE_RELEASE(g_ps_mdl, ID3D11PixelShader);
    SAFE_RELEASE(g_vs_fs, ID3D11VertexShader);
    SAFE_RELEASE(g_ps_tonemap, ID3D11PixelShader);
    SAFE_RELEASE(g_cb_tm, ID3D11Buffer);
    SAFE_RELEASE(g_cbw, ID3D11Buffer);
    SAFE_RELEASE(g_spr_vb, ID3D11Buffer);
    SAFE_RELEASE(g_spr_cvb, ID3D11Buffer);
    SAFE_RELEASE(g_mdl_vb, ID3D11Buffer);
    SAFE_RELEASE(g_mdl_cvb, ID3D11Buffer);
    SAFE_RELEASE(g_ds_test, ID3D11DepthStencilState);
    SAFE_RELEASE(g_vs3d, ID3D11VertexShader);
    SAFE_RELEASE(g_rt_srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_hdr_srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_hdr_rtv, ID3D11RenderTargetView);
    SAFE_RELEASE(g_hdr_tex, ID3D11Texture2D);
    SAFE_RELEASE(g_rt_rtv, ID3D11RenderTargetView);
    SAFE_RELEASE(g_scr_staging, ID3D11Texture2D);
    g_scr_w = g_scr_h = 0;
    SAFE_RELEASE(g_rt_tex, ID3D11Texture2D);
    SAFE_RELEASE(g_bb_rtv, ID3D11RenderTargetView);
    SAFE_RELEASE(g_swap, IDXGISwapChain);
    SAFE_RELEASE(g_ctx, ID3D11DeviceContext);
    SAFE_RELEASE(g_dev, ID3D11Device);
}

/* ------------------------------------------------------------------ */
/* Window placement (fixes the stock renderer's off-screen windows)     */
/* ------------------------------------------------------------------ */
static void place_window(void)
{
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoA(mon, &mi);

    if (g_windowed) {
        RECT r = { 0, 0, (LONG)g_w, (LONG)g_h };
        LONG ww, wh, x, y;
        LONG style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongA(g_hwnd, GWL_STYLE, style | WS_VISIBLE);
        AdjustWindowRect(&r, (DWORD)style, FALSE);
        ww = r.right - r.left;
        wh = r.bottom - r.top;
        x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
        y = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
        if (x < mi.rcWork.left) x = mi.rcWork.left;
        if (y < mi.rcWork.top) y = mi.rcWork.top;
        SetWindowPos(g_hwnd, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        /* Borderless fullscreen on the current monitor; the game image is
         * scaled to fit with correct aspect ratio. */
        SetWindowLongA(g_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

/* ------------------------------------------------------------------ */
/* Quad batching                                                        */
/* ------------------------------------------------------------------ */
static ID3D11ShaderResourceView *g_batch_srv;
static int g_batch_blend = -1;
static Vertex g_batch[6 * MAX_QUADS_PER_BATCH];
static UINT g_batch_verts;

static void set_transform(float w, float h)
{
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float *f = (float *)m.pData;
        f[0] = 2.0f / w;  f[1] = -2.0f / h;  f[2] = -1.0f;  f[3] = 1.0f;
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb, 0);
    }
}

static void bind_pipeline_at(ID3D11RenderTargetView *rtv, float x, float w, float h, ID3D11SamplerState *samp)
{
    D3D11_VIEWPORT vp = { x, 0, w, h, 0, 1 };
    UINT stride = sizeof(Vertex), offset = 0;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_off, 0);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &g_vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &samp);
    set_transform(w, h);
}

static void bind_pipeline(ID3D11RenderTargetView *rtv, float w, float h, ID3D11SamplerState *samp)
{
    bind_pipeline_at(rtv, 0, w, h, samp);
}

static void flush_batch(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    D3D11_MAP mode;
    float bf[4] = { 0, 0, 0, 0 };
    UINT n = g_batch_verts;
    if (!n || !g_ctx) { g_batch_verts = 0; return; }
    if (g_vb_pos + n > 6 * MAX_QUADS_PER_BATCH) g_vb_pos = 0;
    mode = g_vb_pos ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_vb, 0, mode, 0, &m))) {
        memcpy((Vertex *)m.pData + g_vb_pos, g_batch, n * sizeof(Vertex));
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_vb, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_batch_srv);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[g_batch_blend], bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_Draw(g_ctx, n, g_vb_pos);
        g_vb_pos += n;
    }
    g_batch_verts = 0;
}

static void push_quad(ID3D11ShaderResourceView *srv, int blend,
                      float x0, float y0, float x1, float y1,
                      float u0, float v0, float u1, float v1, uint32_t rgba)
{
    Vertex *q;
    if (srv != g_batch_srv || blend != g_batch_blend || g_batch_verts + 6 > 6 * MAX_QUADS_PER_BATCH) {
        flush_batch();
        g_batch_srv = srv;
        g_batch_blend = blend;
    }
    q = &g_batch[g_batch_verts];
    q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
    q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
    q[2].x = x0; q[2].y = y1; q[2].u = u0; q[2].v = v1;
    q[3] = q[2];
    q[4] = q[1];
    q[5].x = x1; q[5].y = y1; q[5].u = u1; q[5].v = v1;
    q[0].rgba = q[1].rgba = q[2].rgba = q[3].rgba = q[4].rgba = q[5].rgba = rgba;
    g_batch_verts += 6;
}

/* Begin 2D drawing into the game-resolution target. */
static void begin_game_pass(void)
{
    bind_pipeline_at(g_rt_rtv, (float)g_xoff, (float)g_w, (float)g_h, g_samp_point);
}

/* Draw the collected HUD blits (see g_hud). Called before anything else draws to the game target. */
static void flush_hud(void)
{
    int i, j, n = g_nhud;
    float lim_l, lim_r, off = (float)g_xoff;
    if (!n) return;
    g_nhud = 0;
    flush_batch();
    bind_pipeline_at(g_rt_rtv, 0, (float)g_tw, (float)g_h, g_samp_point);   /* whole wide target, x in target pixels */
    lim_l = (float)g_w * 0.42f;
    lim_r = (float)g_w * 0.58f;
    for (i = 0; i < n; i = j) {
        float l = g_hud[i].x0, r = g_hud[i].x1, dx, k;
        int full = g_hud[i].x0 <= 0.5f && g_hud[i].x1 >= (float)g_w - 0.5f;
        /* a run: same surface, same line, each blit starting near where the last one ended */
        for (j = i + 1; j < n && !full; j++) {
            const HudQuad *a = &g_hud[j - 1], *b = &g_hud[j];
            float gw = a->x1 - a->x0;
            if (b->srv != a->srv || b->y0 != a->y0 || b->y1 != a->y1) break;
            if (b->x0 < a->x1 - 2.0f || b->x0 > a->x1 + (gw > 8.0f ? gw : 8.0f)) break;
            if (b->x1 > r) r = b->x1;
        }
        if (full) j = i + 1;
        dx = off;                                   /* default: stays in the middle */
        if (!full && r <= lim_l) dx = 0;            /* left part of the 4:3 screen: to the left edge */
        else if (!full && l >= lim_r) dx = 2 * off; /* right part: to the right edge */
        for (k = (float)i; (int)k < j; k += 1.0f) {
            const HudQuad *q = &g_hud[(int)k];
            if (full) push_quad(q->srv, q->blend, 0, q->y0, (float)g_tw, q->y1, q->u0, q->v0, q->u1, q->v1, q->rgba);
            else push_quad(q->srv, q->blend, q->x0 + dx, q->y0, q->x1 + dx, q->y1, q->u0, q->v0, q->u1, q->v1, q->rgba);
        }
    }
    flush_batch();
    begin_game_pass();
}

/* ------------------------------------------------------------------ */
/* Surfaces                                                             */
/* ------------------------------------------------------------------ */
static int upload_surface(Surface *s, uint32_t key)
{
    D3D11_TEXTURE2D_DESC td;
    D3D11_SUBRESOURCE_DATA init;
    uint32_t *tmp, *dst;
    const uint8_t *row;
    uint32_t x, y, keyrgb = key & 0x00FFFFFFu;
    int use_key = (key != COLORKEY_NONE);

    flush_hud();   /* queued HUD blits may still use this surface's old texture */
    tmp = (uint32_t *)malloc((size_t)s->w * s->h * 4);
    if (!tmp) return 0;
    for (y = 0; y < s->h; y++) {
        const uint32_t *src;
        row = s->pixels + (size_t)y * s->pitch;
        src = (const uint32_t *)row;
        dst = tmp + (size_t)y * s->w;
        for (x = 0; x < s->w; x++) {
            uint32_t p = src[x] & 0x00FFFFFFu;
            dst[x] = (use_key && p == keyrgb) ? 0u : (p | 0xFF000000u);
        }
    }

    if (s->tex) {
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)s->tex, 0, NULL, tmp, s->w * 4, 0);
    } else {
        memset(&td, 0, sizeof td);
        td.Width = s->w;
        td.Height = s->h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = tmp;
        init.SysMemPitch = s->w * 4;
        init.SysMemSlicePitch = 0;
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &s->tex)) ||
            FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)s->tex, NULL, &s->srv))) {
            SAFE_RELEASE(s->tex, ID3D11Texture2D);
            free(tmp);
            return 0;
        }
    }
    free(tmp);
    s->tex_key = key;
    s->dirty = 0;
    return 1;
}


/* ------------------------------------------------------------------ */
/* 3D world: geometry from the engine's world data                      */
/* Layouts (AVP2 1.0.9.6 / PH, see docs/renderer-interface.md):         */
/*   world+0x18C  entry** bsps, world+0x190 count, entry+4 -> WorldBsp  */
/*   bsp+0xA0     WorldPoly** polies, bsp+0xA4 count                    */
/*   poly+0x28    float *plane (normal xyz, dist)                       */
/*   poly+0x2C    Surface*                                              */
/*   poly+0x50    vertex array (0x18 bytes each), poly+0x54 u16 count   */
/*   vertex+0     float *pos (xyz)                                      */
/* Every pointer is validated before it is read.                        */
/* ------------------------------------------------------------------ */
static int readable(const void *p, UINT_PTR n)
{
    return (uintptr_t)p >= 0x10000 && !IsBadReadPtr(p, n);
}

#define RD32(base, off) (*(const uint32_t *)((const uint8_t *)(base) + (off)))
#define RDPTR(base, off) ((const uint8_t *)(uintptr_t)RD32(base, off))



/* ------------------------------------------------------------------ */
/* Textures                                                             */
/* SharedTexture+0x0C: renderer data. Engine callbacks:                 */
/*   [1] +0x08 TextureData *GetTexture(SharedTexture*, int *flag)       */
/*   [2] +0x0C void FreeTexture(SharedTexture*)                         */
/* TextureData: +0x14 u16 mip count, +0x22 u8 format (0/3 = 32-bit,     */
/*   4/5/6 = DXT1/3/5), mips from +0xCC, 0x18 bytes each:               */
/*   width, height, data*, pitch.                                       */
/* ------------------------------------------------------------------ */
typedef void *(__cdecl *PFN_GetTexture)(void *shared, int *flag);
typedef void (__cdecl *PFN_FreeTexture)(void *shared);

static RTexture *create_texture(void *shared)
{
    const uint8_t *td;
    int flag = 0;
    uint32_t fmt, nmips, w, h, i;
    DXGI_FORMAT dxfmt;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA init[16];
    RTexture *rt = NULL;

    if (!g_dev || !readable(shared, 0x10)) return NULL;
    td = (const uint8_t *)((PFN_GetTexture)(void *)g_rs->engine_callbacks[1])(shared, &flag);
    if (!readable(td, 0xCC + 0x18)) {
        static int w1;
        if (w1++ < 5) rlog("texture %p: GetTexture returned %p (flag %d)", shared, (void *)td, flag);
        goto done;
    }
    nmips = *(const uint16_t *)(td + 0x14);
    fmt = td[0x22];
    if (fmt == 0) fmt = 3;
    switch (fmt) {
    case 3: dxfmt = DXGI_FORMAT_B8G8R8A8_UNORM; break;
    case 4: dxfmt = DXGI_FORMAT_BC1_UNORM; break;
    case 5: dxfmt = DXGI_FORMAT_BC2_UNORM; break;
    case 6: dxfmt = DXGI_FORMAT_BC3_UNORM; break;
    default: {
        static int warned;
        if (warned++ < 5) rlog("texture %p: unsupported format %lu", shared, (unsigned long)fmt);
        goto done;
    }
    }
    w = RD32(td, 0xCC);
    h = RD32(td, 0xD0);
    {   /* format histogram (diagnostics) */
        static uint32_t nfmt[8], ntot;
        nfmt[fmt & 7]++;
        if ((++ntot % 200) == 0)
            rlog("textures: %lu created: 32-bit %lu, DXT1 %lu, DXT3 %lu, DXT5 %lu", (unsigned long)ntot,
                 (unsigned long)nfmt[3], (unsigned long)nfmt[4], (unsigned long)nfmt[5], (unsigned long)nfmt[6]);
    }
    if (nmips < 1 || nmips > 16 || w < 1 || h < 1 || w > 4096 || h > 4096) {
        static int w2;
        if (w2++ < 5) rlog("texture %p: bad header nmips=%lu w=%lu h=%lu fmt=%lu", shared,
                           (unsigned long)nmips, (unsigned long)w, (unsigned long)h, (unsigned long)fmt);
        goto done;
    }
    if (fmt != 3 && ((w & 3) || (h & 3))) nmips = 1;     /* BC needs 4x4 blocks */
    for (i = 0; i < nmips; i++) {
        const uint8_t *m = td + 0xCC + i * 0x18;
        uint32_t mw = RD32(m, 0), mh = RD32(m, 4), pitch = RD32(m, 12);
        const void *data = (const void *)(uintptr_t)RD32(m, 8);
        uint32_t rows = fmt == 3 ? mh : (mh + 3) / 4;
        /* The engine leaves pitch at 0 for block-compressed mips. */
        if (fmt != 3) pitch = ((mw + 3) / 4) * (fmt == 4 ? 8u : 16u);
        else if (!pitch) pitch = mw * 4;
        if (mw != (w >> i ? w >> i : 1) || mh != (h >> i ? h >> i : 1)) break;
        if (fmt != 3 && ((mw & 3) || (mh & 3))) break;
        if (!pitch || !readable(data, (UINT_PTR)pitch * rows)) break;
        init[i].pSysMem = data;
        init[i].SysMemPitch = pitch;
        init[i].SysMemSlicePitch = 0;
    }
    if (i == 0) {
        static int w3;
        if (w3++ < 8) rlog("texture %p: mip0 rejected w=%lu h=%lu fmt=%lu pitch=%lu data=%08lX m0=%lu,%lu",
                           shared, (unsigned long)w, (unsigned long)h, (unsigned long)fmt,
                           (unsigned long)RD32(td, 0xCC + 12), (unsigned long)RD32(td, 0xCC + 8),
                           (unsigned long)RD32(td, 0xCC), (unsigned long)RD32(td, 0xD0));
        goto done;
    }
    memset(&desc, 0, sizeof desc);
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = i;
    desc.ArraySize = 1;
    desc.Format = dxfmt;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    rt = (RTexture *)calloc(1, sizeof *rt);
    if (!rt) goto done;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &desc, init, &rt->tex)) ||
        FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)rt->tex, NULL, &rt->srv))) {
        static int w4;
        if (w4++ < 5) rlog("texture %p: CreateTexture2D failed %lux%lu fmt=%lu mips=%lu", shared,
                           (unsigned long)w, (unsigned long)h, (unsigned long)fmt, (unsigned long)i);
        if (rt->tex) ID3D11Texture2D_Release(rt->tex);
        free(rt);
        rt = NULL;
        goto done;
    }
    rt->w = w;
    rt->h = h;
    {   /* DTX header at TextureData +0x08: base width/height (u16) at +0x10/+0x12 */
        uint32_t bw = *(const uint16_t *)(td + 0x10), bh = *(const uint16_t *)(td + 0x12);
        static int blog;
        rt->bw = (bw >= w && bw <= 8192) ? bw : w;
        rt->bh = (bh >= h && bh <= 8192) ? bh : h;
        if ((rt->bw != w || rt->bh != h) && blog < 10) {
            rlog("texture %p: loaded %lux%lu, base size %lux%lu", shared, (unsigned long)w, (unsigned long)h,
                 (unsigned long)rt->bw, (unsigned long)rt->bh);
            blog++;
        }
    }
    rt->shared = shared;
    rt->next = g_textures;
    g_textures = rt;
    *(void **)((uint8_t *)shared + 0x0C) = rt;
done:
    if (td) ((PFN_FreeTexture)(void *)g_rs->engine_callbacks[2])(shared);
    return rt;
}

static RTexture *get_texture(void *shared)
{
    RTexture *rt;
    if (!readable(shared, 0x10)) return NULL;
    rt = (RTexture *)(uintptr_t)RD32(shared, 0x0C);
    for (RTexture *t = g_textures; t; t = t->next) if (t == rt) return rt;   /* ours and alive */
    return create_texture(shared);
}

static void release_texture(RTexture *rt)
{
    RTexture **pp;
    for (pp = &g_textures; *pp; pp = &(*pp)->next) {
        if (*pp == rt) {
            *pp = rt->next;
            if (readable(rt->shared, 0x10) && RD32(rt->shared, 0x0C) == (uint32_t)(uintptr_t)rt)
                *(void **)((uint8_t *)rt->shared + 0x0C) = NULL;
            if (rt->srv) ID3D11ShaderResourceView_Release(rt->srv);
            if (rt->tex) ID3D11Texture2D_Release(rt->tex);
            free(rt);
            return;
        }
    }
}

static void release_all_textures(void)
{
    while (g_textures) release_texture(g_textures);
}

/* ------------------------------------------------------------------ */
/* Lightmaps                                                            */
/* ------------------------------------------------------------------ */
/* Engine layout (see docs/renderer-interface.md, "Lightmaps"):
 *   world+0x08/+0x0C  light animation array (0x60 bytes each) and count
 *   world+0xF8        lightmap grid size (world units per texel)
 *   poly+0x0C/+0x10   list of {u16 anim, u16 entry} refs and count
 *   poly+0x14         flags; (flags >> 11) & 7 selects the lightmap axis frame
 *   poly+0x38         lightmap origin; poly+0x4C/+0x4D lightmap width/height
 *   anim+0x20         0 = baked colour frames (others are shadow-mask lights)
 *   anim+0x24         frame table: frame -> array of 0x18-byte entries
 *   anim+0x30         u16 entry count; +0x34/+0x38 frames A/B; +0x3C blend 0..255
 *   anim+0x40         float intensity
 *   entry+0 / +4      RLE data pointer / u16 byte size
 * RLE: a u32 0x00RRGGBB pixel; if bit 31 is set a u8 run length follows. */
#define LM_MAX_TEXELS 1024

/* Reference "up" vector per lightmap axis frame; P = R x N, Q = N x P. */
static const float k_lm_ref[6][3] = {
    { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 },
};

static void cross3(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static void rlog_dump(const char *tag, const void *p, UINT_PTR n);
static const uint8_t *obj_model(const uint8_t *o);
static uint32_t g_model_off;      /* model objects: Model pointer at +0x1F8 (AVP2) or +0x1FC (PH) */
static int g_skin_normalize = 1;  /* cvar avp2m_skinnorm: normalise vertex weights that do not sum to 1 */
static int lm_decode(const uint8_t *src, uint32_t size, uint32_t *dst, uint32_t count)
{
    const uint8_t *end = src + size;
    uint32_t outn = 0;
    if (!src || !size || !readable(src, size)) return 0;
    while (src + 4 <= end) {
        uint32_t px = *(const uint32_t *)src, run = 1, k;
        src += 4;
        if (px & 0x80000000u) {
            if (src >= end) return 0;
            run = *src++;
            px &= 0x7FFFFFFFu;
        }
        if (outn + run > LM_MAX_TEXELS) return 0;
        for (k = 0; k < run; k++) dst[outn++] = px;
    }
    return outn >= count;
}

static const uint8_t *lm_entry(const uint8_t *anim, int frame, uint32_t entry)
{
    const uint8_t *frames = RDPTR(anim, 0x24), *fr;
    if (frame < 0 || frame > 65535 || !readable(frames, (UINT_PTR)(frame + 1) * 4)) return NULL;
    fr = RDPTR(frames, frame * 4);
    if (!readable(fr, (UINT_PTR)(entry + 1) * 0x18)) return NULL;
    return fr + entry * 0x18;
}

/* Gather the poly's lightmap into out (w*h 0x00RRGGBB). Returns 1 if lit. */
static int lm_gather(const uint8_t *world, const uint8_t *poly, uint32_t w, uint32_t h, uint32_t *out)
{
    static uint32_t a[LM_MAX_TEXELS], b[LM_MAX_TEXELS];
    const uint8_t *anims = RDPTR(world, 0x08), *refs = RDPTR(poly, 0x0C);
    uint32_t nanim = RD32(world, 0x0C), nref = RD32(poly, 0x10), r, i, count = w * h;
    int got = 0;

    if (!nref || nref > 64 || nanim > 65535 || !readable(refs, nref * 4) ||
        !readable(anims, (UINT_PTR)nanim * 0x60)) return 0;
    for (r = 0; r < nref; r++) {
        uint32_t ai = ((const uint16_t *)(refs + r * 4))[0];
        uint32_t ei = ((const uint16_t *)(refs + r * 4))[1];
        const uint8_t *anim, *ea, *eb;
        int fa, fb, blend;
        float inten;
        if (ai >= nanim) continue;
        anim = anims + ai * 0x60;
        fa = (int)RD32(anim, 0x34);
        fb = (int)RD32(anim, 0x38);
        blend = (int)RD32(anim, 0x3C);
        inten = *(const float *)(anim + 0x40);
        if (fa == -1 || !(inten >= 0.02f)) continue;
        if (RD32(anim, 0x20) != 0) continue;           /* shadow-mask light: TODO */
        if (ei >= *(const uint16_t *)(anim + 0x30)) continue;
        if (fa == fb || blend < 1) {
            ea = lm_entry(anim, fa, ei);
            if (!ea || !lm_decode(RDPTR(ea, 0), *(const uint16_t *)(ea + 4), a, count)) continue;
        } else if (blend >= 255) {
            ea = lm_entry(anim, fb, ei);
            if (!ea || !lm_decode(RDPTR(ea, 0), *(const uint16_t *)(ea + 4), a, count)) continue;
        } else {
            ea = lm_entry(anim, fa, ei);
            eb = lm_entry(anim, fb, ei);
            if (!ea || !eb || !lm_decode(RDPTR(ea, 0), *(const uint16_t *)(ea + 4), a, count) ||
                !lm_decode(RDPTR(eb, 0), *(const uint16_t *)(eb + 4), b, count)) continue;
            for (i = 0; i < count; i++) {
                uint32_t o = 0; int sh;
                for (sh = 0; sh < 24; sh += 8) {
                    int ca = (a[i] >> sh) & 0xFF, cb = (b[i] >> sh) & 0xFF;
                    int c = ca + (((cb - ca) * blend) >> 8);
                    o |= (uint32_t)(c < 0 ? 0 : c > 255 ? 255 : c) << sh;
                }
                a[i] = o;
            }
        }
        if (!got) {
            memcpy(out, a, count * 4);
        } else {
            uint32_t scale = inten >= 1.0f ? 256u : (uint32_t)(inten * 256.0f);
            for (i = 0; i < count; i++) {
                uint32_t o = 0; int sh;
                for (sh = 0; sh < 24; sh += 8) {
                    uint32_t c = ((out[i] >> sh) & 0xFF) + ((((a[i] >> sh) & 0xFF) * scale) >> 8);
                    o |= (c > 255 ? 255 : c) << sh;
                }
                out[i] = o;
            }
        }
        got = 1;
    }
    return got;
}

/* Gouraud polys: a light animation entry with no lightmap carries per-vertex
 * colours as three byte arrays (entry+8/+12/+16 = R/G/B, entry+20 = count).
 * Output is R8G8B8A8 (alpha 0 = not lightmapped). */
static int lm_vertex_colors(const uint8_t *world, const uint8_t *poly, uint32_t nv, uint32_t *out)
{
    const uint8_t *anims = RDPTR(world, 0x08), *refs = RDPTR(poly, 0x0C);
    uint32_t nanim = RD32(world, 0x0C), nref = RD32(poly, 0x10), r, k;
    uint32_t acc[64][3];
    int got = 0;

    if (!nv || nv > 64 || !nref || nref > 64 || !readable(refs, nref * 4) ||
        !readable(anims, (UINT_PTR)nanim * 0x60)) return 0;
    for (r = 0; r < nref; r++) {
        uint32_t ai = ((const uint16_t *)(refs + r * 4))[0];
        uint32_t ei = ((const uint16_t *)(refs + r * 4))[1];
        const uint8_t *anim, *e, *cr, *cg, *cb;
        float inten;
        uint32_t scale;
        if (ai >= nanim) continue;
        anim = anims + ai * 0x60;
        inten = *(const float *)(anim + 0x40);
        if ((int)RD32(anim, 0x34) == -1 || !(inten >= 0.02f) || RD32(anim, 0x20) != 0) continue;
        if (ei >= *(const uint16_t *)(anim + 0x30)) continue;
        e = lm_entry(anim, (int)RD32(anim, 0x34), ei);
        if (!e || RD32(e, 20) != nv) continue;
        cr = RDPTR(e, 8); cg = RDPTR(e, 12); cb = RDPTR(e, 16);
        if (!readable(cr, nv) || !readable(cg, nv) || !readable(cb, nv)) continue;
        scale = (!got || inten >= 1.0f) ? 256u : (uint32_t)(inten * 256.0f);
        for (k = 0; k < nv; k++) {
            uint32_t c0 = (cr[k] * scale) >> 8, c1 = (cg[k] * scale) >> 8, c2 = (cb[k] * scale) >> 8;
            if (!got) { acc[k][0] = c0; acc[k][1] = c1; acc[k][2] = c2; }
            else { acc[k][0] += c0; acc[k][1] += c1; acc[k][2] += c2; }
        }
        got = 1;
    }
    if (!got) return 0;
    for (k = 0; k < nv; k++) {
        uint32_t c0 = acc[k][0] > 255 ? 255 : acc[k][0], c1 = acc[k][1] > 255 ? 255 : acc[k][1];
        uint32_t c2 = acc[k][2] > 255 ? 255 : acc[k][2];
        out[k] = c0 | (c1 << 8) | (c2 << 16);   /* arrays are R, G, B */
    }
    return 1;
}

/* Shelf packer over LM_PAGE x LM_PAGE pages; CPU copies are uploaded at the end. */
typedef struct LMBuild {
    uint32_t **pix;          /* per page */
    int npages;
    int sx, sy, shelf_h;     /* cursor in current page */
} LMBuild;

static int lm_alloc(LMBuild *lb, int w, int h, int *page, int *x, int *y)
{
    if (w > LM_PAGE || h > LM_PAGE) return 0;
    if (lb->npages && lb->sx + w > LM_PAGE) { lb->sx = 0; lb->sy += lb->shelf_h; lb->shelf_h = 0; }
    if (!lb->npages || lb->sy + h > LM_PAGE) {
        uint32_t **np;
        if (lb->npages >= 16) return 0;
        np = (uint32_t **)realloc(lb->pix, (lb->npages + 1) * sizeof *np);
        if (!np) return 0;
        lb->pix = np;
        lb->pix[lb->npages] = (uint32_t *)malloc((size_t)LM_PAGE * LM_PAGE * 4);
        if (!lb->pix[lb->npages]) return 0;
        {   /* fill with mid grey so unlit gaps don't read as black holes */
            size_t k;
            for (k = 0; k < (size_t)LM_PAGE * LM_PAGE; k++) lb->pix[lb->npages][k] = 0xFF808080u;
        }
        lb->npages++;
        lb->sx = lb->sy = lb->shelf_h = 0;
    }
    *page = lb->npages - 1; *x = lb->sx; *y = lb->sy;
    lb->sx += w;
    if (h > lb->shelf_h) lb->shelf_h = h;
    return 1;
}

/* Place a w*h lightmap with a 1-texel clamped border; returns the inner origin. */
static int lm_place(LMBuild *lb, const uint32_t *src, int w, int h, int *page, int *ox, int *oy)
{
    int x0, y0, x, y;
    uint32_t *dst;
    if (!lm_alloc(lb, w + 2, h + 2, page, &x0, &y0)) return 0;
    dst = lb->pix[*page];
    for (y = -1; y <= h; y++) {
        int sy = y < 0 ? 0 : y >= h ? h - 1 : y;
        for (x = -1; x <= w; x++) {
            int sx = x < 0 ? 0 : x >= w ? w - 1 : x;
            dst[(size_t)(y0 + 1 + y) * LM_PAGE + (x0 + 1 + x)] = src[sy * w + sx] | 0xFF000000u;
        }
    }
    *ox = x0 + 1; *oy = y0 + 1;
    return 1;
}

static void lm_upload(RenderContext *rc, LMBuild *lb)
{
    int i;
    if (!lb->npages) return;
    rc->lmpages = (LMPage *)calloc((size_t)lb->npages, sizeof *rc->lmpages);
    if (rc->lmpages) rc->nlmpages = lb->npages;
    for (i = 0; i < lb->npages; i++) {
        D3D11_TEXTURE2D_DESC td;
        D3D11_SUBRESOURCE_DATA init;
        if (!rc->lmpages) break;
        memset(&td, 0, sizeof td);
        td.Width = td.Height = LM_PAGE;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;    /* animated lights update parts of it */
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        init.pSysMem = lb->pix[i];
        init.SysMemPitch = LM_PAGE * 4;
        init.SysMemSlicePitch = 0;
        if (SUCCEEDED(ID3D11Device_CreateTexture2D(g_dev, &td, &init, &rc->lmpages[i].tex)))
            ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)rc->lmpages[i].tex, NULL,
                                                  &rc->lmpages[i].srv);
    }
    if (cvar_int("avp2m_lmdump", 0) && lb->pix[0]) {   /* write atlas page 0 as a BMP (diagnostics) */
        FILE *f = fopen("avp2m_lmpage0.bmp", "wb");
        if (f) {
            uint8_t hdr[54];
            uint32_t sz = LM_PAGE * LM_PAGE * 4;
            int32_t hh = -LM_PAGE;
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M';
            *(uint32_t *)(hdr + 2) = 54 + sz; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
            *(int32_t *)(hdr + 18) = LM_PAGE; memcpy(hdr + 22, &hh, 4);
            *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 32; *(uint32_t *)(hdr + 34) = sz;
            fwrite(hdr, 1, 54, f);
            fwrite(lb->pix[0], 1, sz, f);
            fclose(f);
            rlog("lightmap atlas page 0 written to avp2m_lmpage0.bmp");
        }
    }
    for (i = 0; i < lb->npages; i++) free(lb->pix[i]);
    free(lb->pix);
    lb->pix = NULL;
    lb->npages = 0;
}

static void lm_release(RenderContext *rc)
{
    int i;
    for (i = 0; i < rc->nlmpages; i++) {
        SAFE_RELEASE(rc->lmpages[i].srv, ID3D11ShaderResourceView);
        SAFE_RELEASE(rc->lmpages[i].tex, ID3D11Texture2D);
    }
    free(rc->lmpages);
    rc->lmpages = NULL;
    rc->nlmpages = 0;
}

typedef struct BuildTri { int bsp; RTexture *tex; int lmpage; WVertex v[3]; uint32_t col[3]; int dyn; uint8_t corner[3]; } BuildTri;

static int cmp_tri(const void *a, const void *b)
{
    const BuildTri *p = (const BuildTri *)a, *q = (const BuildTri *)b;
    uintptr_t x = (uintptr_t)p->tex, y = (uintptr_t)q->tex;
    if (p->bsp != q->bsp) return p->bsp - q->bsp;
    if (x != y) return x < y ? -1 : 1;
    return (p->lmpage > q->lmpage) - (p->lmpage < q->lmpage);
}

/* Surface flags (LithTech; tentative names). */
#define SURF_INVISIBLE 0x04
#define SURF_SKY       0x10
#define SURF_LIGHTMAP  0x80

/* 1 if the poly references a baked light animation other than LightAnim_BASE
 * (index 0); such polys are relit when the animation changes. */
static int poly_has_anim(const uint8_t *world, const uint8_t *poly)
{
    const uint8_t *anims = RDPTR(world, 0x08), *refs = RDPTR(poly, 0x0C);
    uint32_t nanim = RD32(world, 0x0C), nref = RD32(poly, 0x10), r;
    if (!nref || nref > 64 || !readable(refs, nref * 4) || !readable(anims, (UINT_PTR)nanim * 0x60)) return 0;
    for (r = 0; r < nref; r++) {
        uint32_t ai = ((const uint16_t *)(refs + r * 4))[0];
        uint32_t ei = ((const uint16_t *)(refs + r * 4))[1];
        const uint8_t *anim, *e;
        uint32_t nf, f, k;
        if (!ai || ai >= nanim) continue;
        anim = anims + ai * 0x60;
        if (RD32(anim, 0x20) != 0 || ei >= *(const uint16_t *)(anim + 0x30)) continue;
        nf = RD32(anim, 0x28);
        for (f = 0; f < nf && f < 64; f++) {   /* any frame with light for this poly */
            e = lm_entry(anim, (int)f, ei);
            if (!e) continue;
            if (RD32(e, 0) && *(const uint16_t *)(e + 4)) return 1;      /* lightmap data */
            {
                uint32_t nv = RD32(e, 20);
                const uint8_t *c0 = RDPTR(e, 8), *c1 = RDPTR(e, 12), *c2 = RDPTR(e, 16);
                if (nv && nv <= 64 && readable(c0, nv) && readable(c1, nv) && readable(c2, nv))
                    for (k = 0; k < nv; k++) if (c0[k] | c1[k] | c2[k]) return 1;
            }
        }
    }
    return 0;
}

static void build_world(RenderContext *rc)
{
    const uint8_t *world = (const uint8_t *)rc->world;
    const uint8_t *list;
    uint32_t nbsp, b, total_polys = 0, skipped = 0, hidden = 0, untextured = 0;
    BuildTri *tris = NULL;
    size_t cap = 0, n = 0, i;
    WVertex *verts;
    LMBuild lb;
    uint32_t lit = 0, lmfail = 0, gouraud = 0, dvcap = 0;
    double lmsum = 0;
    float grid = 1.0f;
    static uint32_t lmpix[LM_MAX_TEXELS];
    memset(&lb, 0, sizeof lb);

    rc->built = 1;
    if (!readable(world, 0x194)) { rlog("world: bad world pointer %p", (void *)world); return; }
    list = RDPTR(world, 0x18C);
    nbsp = RD32(world, 0x190);
    if (readable(world, 0xFC)) grid = *(const float *)(world + 0xF8);
    if (!(grid > 0.01f && grid < 4096.0f)) grid = 16.0f;
    rlog("world: %lu light animations, lightmap grid %d", (unsigned long)RD32(world, 0x0C), (int)grid);
    {   /* diagnostics: dump the light animation headers */
        const uint8_t *anims = RDPTR(world, 0x08);
        uint32_t na = RD32(world, 0x0C), ai;
        for (ai = 0; ai < na && ai < 8 && readable(anims, (ai + 1) * 0x60); ai++) {
            const uint32_t *d = (const uint32_t *)(anims + ai * 0x60);
            char nm[33]; int k;
            for (k = 0; k < 32; k++) { char ch = ((const char *)d)[k]; nm[k] = (ch >= 32 && ch < 127) ? ch : '.'; }
            nm[32] = 0;
            rlog("  anim %lu: [%s] 20=%08lx 24=%08lx 28=%08lx 2c=%08lx 30=%08lx 34=%ld 38=%ld 3c=%ld 40=%d/1000",
                 (unsigned long)ai, nm, (unsigned long)d[8], (unsigned long)d[9], (unsigned long)d[10],
                 (unsigned long)d[11], (unsigned long)d[12], (long)d[13], (long)d[14], (long)d[15],
                 (int)(((const float *)d)[16] * 1000));
        }
    }
    if (nbsp > 4096 || !readable(list, nbsp * 4)) { rlog("world: bad bsp list %p n=%lu", (void *)list, (unsigned long)nbsp); return; }

    rc->bsps = (const uint8_t **)calloc(nbsp ? nbsp : 1, sizeof *rc->bsps);
    rc->bsp_xf = (const float **)calloc(nbsp ? nbsp : 1, sizeof *rc->bsp_xf);
    rc->sky = (uint8_t *)calloc(nbsp ? nbsp : 1, 1);
    rc->nbsps = (rc->bsps && rc->bsp_xf && rc->sky) ? nbsp : 0;
    for (b = 0; b < nbsp; b++) {
        const uint8_t *entry = RDPTR(list, b * 4), *bsp, *polies;
        uint32_t np, pi;
        if (!readable(entry, 8)) continue;
        bsp = RDPTR(entry, 4);
        if (!readable(bsp, 0xA8)) continue;
        if (rc->nbsps) rc->bsps[b] = bsp;
        polies = RDPTR(bsp, 0xA0);
        np = RD32(bsp, 0xA4);
        if (np > 2000000 || !readable(polies, np * 4)) { skipped++; continue; }
        for (pi = 0; pi < np; pi++) {
            const uint8_t *poly = RDPTR(polies, pi * 4), *vlist, *surface, *v0;
            const float *plane, *p0;
            uint32_t nv, k, sflags;
            RTexture *tex = NULL;
            float iw = 1.0f / 64, ih = 1.0f / 64;
            int lmpage = -1, lmx = 0, lmy = 0, have_vc = 0, dyn = -1;
            uint32_t vcols[64];
            float P[3] = { 0, 0, 0 }, Q[3] = { 0, 0, 0 }, O[3] = { 0, 0, 0 };
            if (!readable(poly, 0x58)) { skipped++; continue; }
            vlist = RDPTR(poly, 0x50);
            nv = *(const uint16_t *)(poly + 0x54);
            plane = (const float *)RDPTR(poly, 0x28);
            surface = RDPTR(poly, 0x2C);
            if (nv < 3 || nv > 64 || !readable(vlist, nv * 0x18) || !readable(plane, 12) ||
                !readable(surface, 0x34)) { skipped++; continue; }
            sflags = RD32(surface, 0x30);
            if (sflags & (SURF_INVISIBLE | SURF_SKY)) { hidden++; continue; }
            tex = get_texture((void *)(uintptr_t)RD32(surface, 0x2C));
            if (tex) { iw = 1.0f / (float)tex->w; ih = 1.0f / (float)tex->h; } else untextured++;
            if ((sflags & SURF_LIGHTMAP) && readable(poly, 0x50)) {
                uint32_t lw = poly[0x4C], lh = poly[0x4D];
                uint32_t axis = ((uint32_t)*(const uint16_t *)(poly + 0x14) >> 11) & 7;
                if (lw && lh && lw * lh <= LM_MAX_TEXELS && axis < 6) {
                    int anim = poly_has_anim(world, poly), got = lm_gather(world, poly, lw, lh, lmpix);
                    if (!got && anim) { memset(lmpix, 0, lw * lh * 4); got = 1; }   /* dark now, may light up */
                    if (got && lm_place(&lb, lmpix, (int)lw, (int)lh, &lmpage, &lmx, &lmy)) {
                        if (anim) {
                            if ((rc->ndlm & 255) == 0) rc->dlm = (struct DynLM *)realloc(rc->dlm, (rc->ndlm + 256) * sizeof *rc->dlm);
                            if (rc->dlm) {
                                struct DynLM *d = &rc->dlm[rc->ndlm++];
                                d->poly = poly; d->page = lmpage; d->x = lmx; d->y = lmy; d->w = (int)lw; d->h = (int)lh;
                            }
                        }
                        cross3(k_lm_ref[axis], plane, P);
                        cross3(plane, P, Q);
                        memcpy(O, poly + 0x38, sizeof O);
                        {
                            uint32_t k, sum = 0;
                            for (k = 0; k < lw * lh; k++)
                                sum += ((lmpix[k] >> 16) & 0xFF) + ((lmpix[k] >> 8) & 0xFF) + (lmpix[k] & 0xFF);
                            lmsum += (double)sum / (3.0 * lw * lh);
                        }
                        lit++;
                    } else {
                        lmpage = -1;
                        lmfail++;
                    }
                }
            }
            /* Vertex colour stream: alpha 1 = lightmapped (colour unused); otherwise the
             * gouraud light from the light animations, or mid grey if there is none. */
            if (lmpage < 0 && (have_vc = lm_vertex_colors(world, poly, nv, vcols)) != 0) gouraud++;
            dyn = -1;
            if (lmpage < 0 && poly_has_anim(world, poly)) {   /* vertex-lit and animated */
                if ((rc->ndgv & 255) == 0) rc->dgv = (struct DynGV *)realloc(rc->dgv, (rc->ndgv + 256) * sizeof *rc->dgv);
                if (rc->dgv) {
                    rc->dgv[rc->ndgv].poly = poly; rc->dgv[rc->ndgv].nv = nv; rc->dgv[rc->ndgv].first = rc->ndgv_cols;
                    rc->ndgv_cols += nv;
                    dyn = (int)rc->ndgv++;
                }
            }
            v0 = vlist;
            p0 = (const float *)RDPTR(v0, 0);
            if (!readable(p0, 12)) { skipped++; continue; }
            if (n + (nv - 2) > cap) {
                size_t ncap = cap ? cap * 2 : 32768;
                while (ncap < n + (nv - 2)) ncap *= 2;
                tris = (BuildTri *)realloc(tris, ncap * sizeof *tris);
                if (!tris) { rlog("world: out of memory"); return; }
                cap = ncap;
            }
            for (k = 1; k + 1 < nv; k++) {
                const uint8_t *vv[3];
                int c;
                vv[0] = v0;
                vv[1] = vlist + k * 0x18;
                vv[2] = vlist + (k + 1) * 0x18;
                tris[n].tex = tex;
                tris[n].bsp = (int)b;
                tris[n].lmpage = lmpage;
                tris[n].dyn = dyn;
                tris[n].corner[0] = 0; tris[n].corner[1] = (uint8_t)k; tris[n].corner[2] = (uint8_t)(k + 1);
                tris[n].col[0] = lmpage >= 0 ? 0xFF000000u : have_vc ? vcols[0] : 0x00808080u;
                tris[n].col[1] = lmpage >= 0 ? 0xFF000000u : have_vc ? vcols[k] : 0x00808080u;
                tris[n].col[2] = lmpage >= 0 ? 0xFF000000u : have_vc ? vcols[k + 1] : 0x00808080u;
                for (c = 0; c < 3; c++) {
                    const float *p = (const float *)RDPTR(vv[c], 0);
                    const float *uv = (const float *)(vv[c] + 4);
                    if (!readable(p, 12)) break;
                    tris[n].v[c].x = p[0]; tris[n].v[c].y = p[1]; tris[n].v[c].z = p[2];
                    tris[n].v[c].u = uv[0] * iw; tris[n].v[c].v = uv[1] * ih;
                    tris[n].v[c].rgba = 0;
                    if (lmpage >= 0) {
                        float d0 = p[0] - O[0], d1 = p[1] - O[1], d2 = p[2] - O[2];
                        float tu = (d0 * P[0] + d1 * P[1] + d2 * P[2]) / grid + 0.5f;
                        float tv = (d0 * Q[0] + d1 * Q[1] + d2 * Q[2]) / grid + 0.5f;
                        tris[n].v[c].lu = (tu + (float)lmx) / (float)LM_PAGE;
                        tris[n].v[c].lv = (tv + (float)lmy) / (float)LM_PAGE;
                    } else {
                        tris[n].v[c].lu = tris[n].v[c].lv = 0;
                    }
                }
                if (c == 3) n++;
            }
            total_polys++;
        }
    }

    if (n) {
        qsort(tris, n, sizeof *tris, cmp_tri);
        verts = (WVertex *)malloc(n * 3 * sizeof *verts);
        rc->batches = (DrawBatch *)calloc(n, sizeof *rc->batches);
        if (verts && rc->batches) {
            D3D11_BUFFER_DESC bd;
            D3D11_SUBRESOURCE_DATA init;
            rc->vcol = (uint32_t *)malloc(n * 3 * sizeof *rc->vcol);
            for (i = 0; i < n; i++) {
                memcpy(&verts[i * 3], tris[i].v, sizeof tris[i].v);
                if (rc->vcol) memcpy(&rc->vcol[i * 3], tris[i].col, sizeof tris[i].col);
                if (tris[i].dyn >= 0) {
                    int c;
                    if (rc->ndvert + 3 > dvcap) {
                        dvcap = dvcap ? dvcap * 2 : 3072;
                        rc->dvert = (struct DynVert *)realloc(rc->dvert, dvcap * sizeof *rc->dvert);
                        if (!rc->dvert) { rc->ndvert = 0; dvcap = 0; }
                    }
                    if (rc->dvert && rc->dgv)
                        for (c = 0; c < 3; c++) {
                            rc->dvert[rc->ndvert].vcol = (uint32_t)(i * 3 + c);
                            rc->dvert[rc->ndvert].col = rc->dgv[tris[i].dyn].first + tris[i].corner[c];
                            rc->ndvert++;
                        }
                }
                if (!rc->nbatches || rc->batches[rc->nbatches - 1].tex != tris[i].tex ||
                    rc->batches[rc->nbatches - 1].lmpage != tris[i].lmpage ||
                    rc->batches[rc->nbatches - 1].bsp != tris[i].bsp) {
                    rc->batches[rc->nbatches].bsp = tris[i].bsp;
                    rc->batches[rc->nbatches].tex = tris[i].tex;
                    rc->batches[rc->nbatches].lmpage = tris[i].lmpage;
                    rc->batches[rc->nbatches].first = (UINT)(i * 3);
                    rc->nbatches++;
                }
                rc->batches[rc->nbatches - 1].count += 3;
            }
            memset(&bd, 0, sizeof bd);
            bd.ByteWidth = (UINT)(n * 3 * sizeof(WVertex));
            bd.Usage = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            init.pSysMem = verts;
            init.SysMemPitch = init.SysMemSlicePitch = 0;
            if (SUCCEEDED(ID3D11Device_CreateBuffer(g_dev, &bd, &init, &rc->vb))) rc->vcount = (UINT)(n * 3);
            memset(&bd, 0, sizeof bd);
            bd.ByteWidth = (UINT)(n * 3 * 4);
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            init.pSysMem = rc->vcol;
            if (!rc->vcol || FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, &init, &rc->cvb))) {
                SAFE_RELEASE(rc->vb, ID3D11Buffer);
                rc->vcount = 0;
            }
        }
        free(verts);
    }
    free(tris);
    lm_upload(rc, &lb);
    rlog("world: animated light on %lu lightmapped and %lu vertex-lit polygons", (unsigned long)rc->ndlm, (unsigned long)rc->ndgv);
    {   /* light animations: list them (kind +0x20: 0 baked frames, else shadow-mask light) */
        const uint8_t *anims = RDPTR(world, 0x08);
        uint32_t na = RD32(world, 0x0C), ai, nk = 0;
        static int alog;
        if (alog++ < 1 && readable(anims, (UINT_PTR)na * 0x60)) {
            for (ai = 0; ai < na; ai++) {
                const uint8_t *an = anims + ai * 0x60;
                if (RD32(an, 0x20) == 0 && ai > 3) continue;
                if (nk++ >= 24) break;
                {
                    char nm[32];
                    const char *src = readable(RDPTR(an, 0), 32) ? (const char *)RDPTR(an, 0) : (const char *)an;
                    int c;
                    for (c = 0; c < 31 && src[c] >= 0x20 && src[c] < 0x7F; c++) nm[c] = src[c];
                    nm[c] = 0;
                    rlog("lightanim %lu: %s kind %08lx", (unsigned long)ai, nm, (unsigned long)RD32(an, 0x20));
                }
                rlog_dump("  la", an, 0x60);
            }
            rlog("lightanims: %lu total", (unsigned long)na);
        }
    }
    rlog("world: %lu lightmapped polygons on %d atlas pages (%lu without lightmap data), lmscale %d%%, mean texel %d",
         (unsigned long)lit, rc->nlmpages, (unsigned long)lmfail, (int)(g_lmscale * 100),
         lit ? (int)(lmsum / lit) : -1);
    rlog("world: %lu gouraud-lit polygons from light animations", (unsigned long)gouraud);
    {
        uint32_t ntex = 0;
        for (RTexture *t = g_textures; t; t = t->next) ntex++;
        rlog("world: %lu bsps, %lu polygons -> %lu triangles in %lu batches, %lu textures "
             "(%lu hidden, %lu untextured, %lu skipped)", (unsigned long)nbsp, (unsigned long)total_polys,
             (unsigned long)n, (unsigned long)rc->nbatches, (unsigned long)ntex,
             (unsigned long)hidden, (unsigned long)untextured, (unsigned long)skipped);
    }
}

/* SceneDesc fields used so far (dword offsets into the engine's struct). */
#define SD_DRAWMODE   0x00
#define SD_CONTEXT    0x34
#define SD_RECT       0xBC
#define SD_XFOV       0xCC
#define SD_YFOV       0xD0
#define SD_FARZ       0xD4
#define SD_POS        0xD8
#define SD_ROT        0xE4

static void quat_basis(const float *q, float *right, float *up, float *fwd)
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    right[0] = 1 - 2 * (y * y + z * z); right[1] = 2 * (x * y + w * z);     right[2] = 2 * (x * z - w * y);
    up[0]    = 2 * (x * y - w * z);     up[1]    = 1 - 2 * (x * x + z * z); up[2]    = 2 * (y * z + w * x);
    fwd[0]   = 2 * (x * z + w * y);     fwd[1]   = 2 * (y * z - w * x);     fwd[2]   = 1 - 2 * (x * x + y * y);
}

#define OBJ_FLAGS     0x88    /* bit 0: visible */
#define OBJ_FLAGS2    0x8C
#define OBJ_COLOR     0x94
#define OBJ_TYPE      0xBC    /* byte: 1 model, 2 world model, 3 sprite, 4 light, 6 particles, 7 polygrid, 8 lines, 9 container, 10 canvas */
#define OBJ_POS       0x194   /* position (3 floats) */
#define OBJ_ROT       0x168   /* rotation quaternion x, y, z, w (tentative) */
#define OBJ_SCALE     0x1A0   /* scale (3 floats; world models: half size) */

#define MAX_VIS_OBJECTS 4096
static uint8_t *g_vis_objs[MAX_VIS_OBJECTS];
static uint32_t g_nvis_objs, g_nvis_seen;

/* ------------------------------------------------------------------ */
/* Models (characters, weapons, props)                                  */
/* ------------------------------------------------------------------ */
/* The engine animates the skeleton: model object vtable slot 18 (+0x48,
 * thiscall, no arguments) returns one 4x4 (row-major, translation in the last
 * column) per node, in model space. The renderer skins the vertices.
 *   object +0x1C0..+0x1CC  skins: up to 4 SharedTexture pointers
 *   object +0x1F8          model
 *   model  +0x28 / +0x2C   node array / node count
 *   model  +0x38 / +0x3C   piece pointers / piece count
 *   piece  +0x04 / +0x08   vertices (0x20 bytes each) / count
 *   piece  +0x14 / +0x18   triangles (0x20 bytes each) / count
 *   piece  +0x24           skin index (stock d3d.ren 0x1000447A)
 *   vertex +0x00 weights pointer, +0x04 u16 weight count,
 *          +0x08 position, +0x14 normal (bind pose)
 *   weight {float offset[3]; float weight; u32 node} (0x14 bytes): the
 *          vertex is sum(weight * (node transform * offset))
 *   triangle +0x00 u16 indices[3], +0x08 uv[3] (float pairs) */
typedef const float *(__fastcall *PFN_NodeXforms)(void *self, void *edx);
/* blend: 0 solid, 1 alpha (object alpha < 255), 2 additive (flags2 0x04) */
typedef struct MdlDraw { RTexture *tex; UINT first, count; int blend; } MdlDraw;

static void xform34(const float *m, const float *v, float *o)
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2] + m[3];
    o[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2] + m[7];
    o[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2] + m[11];
}
static void rot33(const float *m, const float *v, float *o)
{
    o[0] = m[0] * v[0] + m[1] * v[1] + m[2] * v[2];
    o[1] = m[4] * v[0] + m[5] * v[1] + m[6] * v[2];
    o[2] = m[8] * v[0] + m[9] * v[1] + m[10] * v[2];
}

/* Object-to-world 3x4 from position, rotation quaternion and scale. */
static void object_matrix(const uint8_t *o, const float *campos, const float *cr, const float *cu,
                          const float *cf, float *m)
{
    const float *pos = (const float *)(o + OBJ_POS), *sc = (const float *)(o + 0xAC);
    float r[3], u[3], f[3];
    int k;
    quat_basis((const float *)(o + 0x9C), r, u, f);
    for (k = 0; k < 3; k++) {          /* columns are the scaled basis vectors */
        m[k * 4 + 0] = r[k] * sc[0];
        m[k * 4 + 1] = u[k] * sc[1];
        m[k * 4 + 2] = f[k] * sc[2];
        m[k * 4 + 3] = pos[k];
    }
    if (RD32(o, OBJ_FLAGS) & 0x40) {   /* "really close": camera space */
        float w[12];
        for (k = 0; k < 4; k++) {
            int a;
            for (a = 0; a < 3; a++)
                w[a * 4 + k] = cr[a] * m[0 * 4 + k] + cu[a] * m[1 * 4 + k] + cf[a] * m[2 * 4 + k] +
                               (k == 3 ? campos[a] : 0.0f);
        }
        memcpy(m, w, sizeof w);
    }
}

/* World light grid (world+0xFC): ambient light for objects.
 *   +0x00 cells (u32 0x00RRGGBB), +0x08 y stride, +0x20 z stride (in cells),
 *   +0x14/+0x18/+0x1C max cell index x/y/z, +0x30 1/cell size (3 floats),
 *   +0x3C grid minimum (3 floats). Sampled trilinearly. */
static int lightgrid_sample(const uint8_t *world, const float *pos, float *rgb)
{
    const uint8_t *g, *cells;
    int32_t mx, my, mz, sy, sz, ix[3], k;
    float f[3];
    if (!world || !readable(world + 0xFC, 0x48)) return 0;
    g = world + 0xFC;
    cells = RDPTR(g, 0);
    sy = (int32_t)RD32(g, 0x08); sz = (int32_t)RD32(g, 0x20);
    mx = (int32_t)RD32(g, 0x14); my = (int32_t)RD32(g, 0x18); mz = (int32_t)RD32(g, 0x1C);
    if (!cells || mx < 0 || my < 0 || mz < 0 || mx > 4096 || my > 4096 || mz > 4096 || sy <= 0 || sz <= 0) return 0;
    for (k = 0; k < 3; k++) {
        float t = (pos[k] - ((const float *)(g + 0x3C))[k]) * ((const float *)(g + 0x30))[k];
        int32_t lim = k == 0 ? mx : k == 1 ? my : mz;
        int32_t i = (int32_t)floorf(t);
        if (i < 0) { i = 0; t = 0; }
        if (i >= lim) { i = lim; t = (float)lim; }
        ix[k] = i;
        f[k] = t - (float)i;
    }
    if (!readable(cells, ((size_t)mx + 1 + (size_t)my * sy + (size_t)mz * sz + 1) * 4)) return 0;
    rgb[0] = rgb[1] = rgb[2] = 0;
    for (k = 0; k < 8; k++) {
        int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        int32_t x = ix[0] + dx, y = ix[1] + dy, z = ix[2] + dz;
        float w = (dx ? f[0] : 1 - f[0]) * (dy ? f[1] : 1 - f[1]) * (dz ? f[2] : 1 - f[2]);
        uint32_t c;
        if (x > mx) x = mx;
        if (y > my) y = my;
        if (z > mz) z = mz;
        c = RD32(cells, ((size_t)x + (size_t)y * sy + (size_t)z * sz) * 4);
        rgb[0] += w * (float)((c >> 16) & 0xFF);
        rgb[1] += w * (float)((c >> 8) & 0xFF);
        rgb[2] += w * (float)(c & 0xFF);
    }
    return 1;
}

/* Point lights affecting a model: static lights from the world tree (box
 * query, list 1) and dynamic light objects (type 4) from this frame's list. */
#define MAX_MODEL_LIGHTS 8
typedef struct MLight { float pos[3], radius, col[3]; } MLight;
static MLight g_mlights[MAX_MODEL_LIGHTS];
static int g_nmlights;
static float g_mlight_center[3], g_mlight_reach;

typedef struct BoxQuery {
    uint32_t engine;                      /* engine writes the tree pointer */
    uint32_t list_index;                  /* 1 = static lights */
    float min[3], max[3];
    uint32_t pad[3];                      /* callback must sit at +0x2C, user at +0x30 */
    void (__cdecl *on_object)(const uint8_t *light, void *user);
    void *user;
    uint32_t scratch[8];
} BoxQuery;
typedef char box_query_layout_check[(offsetof(BoxQuery, on_object) == 0x2C && offsetof(BoxQuery, user) == 0x30) ? 1 : -1];
typedef void (__fastcall *PFN_WorldTreeBox)(void *self, void *edx, BoxQuery *q);

static void add_mlight(const float *pos, float radius, float r, float g, float b)
{
    float d[3], dist;
    int k;
    if (g_nmlights >= MAX_MODEL_LIGHTS || !(radius > 0)) return;
    for (k = 0; k < 3; k++) d[k] = pos[k] - g_mlight_center[k];
    dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist - g_mlight_reach >= radius) return;
    memcpy(g_mlights[g_nmlights].pos, pos, 12);
    g_mlights[g_nmlights].radius = radius;
    g_mlights[g_nmlights].col[0] = r / 255.0f;
    g_mlights[g_nmlights].col[1] = g / 255.0f;
    g_mlights[g_nmlights].col[2] = b / 255.0f;
    g_nmlights++;
}

/* Static light: +0x68 position, +0x74 radius, +0x78 color (3 floats, 0..255). */
static void __cdecl on_static_light(const uint8_t *l, void *user)
{
    const float *f;
    (void)user;
    if (!readable(l, 0xA0)) return;
    f = (const float *)(l + 0x68);
    add_mlight(f, f[3], f[4], f[5], f[6]);
}

static void gather_model_lights(const uint8_t *world, const float *center, float reach)
{
    uint32_t i;
    g_nmlights = 0;
    memcpy(g_mlight_center, center, 12);
    g_mlight_reach = reach;
    if (world && readable(world, 0x70) && readable(RDPTR(world + 0x6C, 0), 0x10)) {
        BoxQuery q;
        int k;
        memset(&q, 0, sizeof q);
        q.list_index = 1;
        for (k = 0; k < 3; k++) { q.min[k] = center[k] - reach; q.max[k] = center[k] + reach; }
        q.on_object = on_static_light;
        ((PFN_WorldTreeBox)(void *)(uintptr_t)RD32(RDPTR(world + 0x6C, 0), 0x08))((void *)(world + 0x6C), NULL, &q);
    }
    for (i = 0; i < g_nvis_objs; i++) {
        const uint8_t *o = g_vis_objs[i];
        if (o[OBJ_TYPE] != 4 || !readable(o, 0x1B4)) continue;
        add_mlight((const float *)(o + OBJ_POS), *(const float *)(o + 0x1B0), o[0x94], o[0x95], o[0x96]);
    }
}


/* The game code's model hook (SceneDesc +0xFC, user data +0x100), called per
 * model as the stock renderer does. It may change the model's light add and
 * color; the game uses it for things like vision modes and highlights. */
typedef struct ModelHookData {
    void *object;
    uint32_t hook_flags;          /* the stock renderer passes 1 */
    uint32_t object_flags;        /* object +0x88 */
    float *light_add;             /* 3 floats, 0..255; starts at SceneDesc +0x1C */
    float *object_color;          /* 3 floats, 0..255; starts at the object's color */
} ModelHookData;
typedef void (__cdecl *PFN_ModelHook)(ModelHookData *data, void *user);
static const uint8_t *g_cur_sd;

static void run_model_hook(uint8_t *o, float *add, float *col)
{
    int k;
    for (k = 0; k < 3; k++) {
        add[k] = (g_cur_sd && readable(g_cur_sd, 0x104)) ? ((const float *)(g_cur_sd + 0x1C))[k] : 0.0f;
        col[k] = (float)o[0x94 + k];
    }
    if (g_cur_sd && readable(g_cur_sd, 0x104) && RD32(g_cur_sd, 0xFC)) {
        ModelHookData hd;
        hd.object = o;
        hd.hook_flags = 1;
        hd.object_flags = RD32(o, OBJ_FLAGS);
        hd.light_add = add;
        hd.object_color = col;
        ((PFN_ModelHook)(void *)(uintptr_t)RD32(g_cur_sd, 0xFC))(&hd, (void *)(uintptr_t)RD32(g_cur_sd, 0x100));
    }
}

static void draw_models(const float *campos, const float *cr, const float *cu, const float *cf)
{
    const uint8_t *world = g_cur_ctx ? (const uint8_t *)g_cur_ctx->world : NULL;
    if (g_rs && readable((const uint8_t *)g_rs + 0x138, 0x18)) {
        const float *d = (const float *)((const uint8_t *)g_rs + 0x138), *c = d + 3;
        int q;
        /* RenderStruct holds the direction the light travels; shading needs the
         * direction towards the light (the stock renderer stores it negated). */
        for (q = 0; q < 3; q++) { g_sun_dir[q] = -d[q]; g_sun_col[q] = c[q] / 255.0f; }
        g_sun_on = (c[0] > 0 || c[1] > 0 || c[2] > 0);
    } else {
        g_sun_on = 0;
    }
    static MdlDraw list[1024];
    static float skinned[4096 * 3];
    static uint32_t light[4096];
    D3D11_MAPPED_SUBRESOURCE mv, mc;
    WVertex *vv;
    uint32_t *cc, i, nv_out = 0, nd = 0, nmodels = 0, k, malpha = 0, ntrans = 0;
    int mblend = 0;
    static int logged, glog, mlog;
    float bf[4] = { 0, 0, 0, 0 }, mamb[3] = { 1, 1, 1 }, mcol[3] = { 1, 1, 1 };

    if (!g_mdl_vb) return;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_mdl_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mv))) return;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_mdl_cvb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_mdl_vb, 0);
        return;
    }
    vv = (WVertex *)mv.pData;
    cc = (uint32_t *)mc.pData;
    for (i = 0; i < g_nvis_objs; i++) {
        uint8_t *o = g_vis_objs[i];
        const uint8_t *m, *pieces;
        const float *nodes;
        uint32_t nnodes, npieces, pi, hidden;
        float om[12];
        if (o[OBJ_TYPE] != 1 || !readable(o, 0x200) || o[0x97] == 0) continue;
        m = obj_model(o);
        if (!readable(m, 0x80)) continue;
        nnodes = RD32(m, 0x2C);
        npieces = RD32(m, 0x3C);
        pieces = RDPTR(m, 0x38);
        if (!nnodes || nnodes > 512 || !npieces || npieces > 64 || !readable(pieces, npieces * 4)) continue;
        if (!readable(RDPTR(o, 0), 0x4C)) continue;
        nodes = ((PFN_NodeXforms)(void *)(uintptr_t)RD32(RDPTR(o, 0), 0x48))(o, NULL);
        if (!readable(nodes, nnodes * 64)) continue;
        {   /* diagnostics: nodes with unusual scale or large offsets (PH cutscene spikes) */
            static const uint8_t *seen[48];
            static int nseen;
            int q;
            uint32_t ni, nbad = 0;
            for (q = 0; q < nseen && seen[q] != o; q++) ;
            if (q == nseen && nseen < 48) {
                seen[nseen++] = o;
                for (ni = 0; ni < nnodes; ni++) {
                    const float *x = nodes + ni * 16;
                    float sx = sqrtf(x[0] * x[0] + x[4] * x[4] + x[8] * x[8]);
                    float sy = sqrtf(x[1] * x[1] + x[5] * x[5] + x[9] * x[9]);
                    float sz = sqrtf(x[2] * x[2] + x[6] * x[6] + x[10] * x[10]);
                    float tl = sqrtf(x[3] * x[3] + x[7] * x[7] + x[11] * x[11]);
                    if (sx < 0.8f || sx > 1.25f || sy < 0.8f || sy > 1.25f || sz < 0.8f || sz > 1.25f || tl > 300.0f || x[3] != x[3]) {
                        if (nbad++ < 12)
                            rlog("  odd node %lu/%lu of model %p: scale x100 %d,%d,%d offset %d,%d,%d", (unsigned long)ni, (unsigned long)nnodes,
                                 (const void *)o, (int)(sx * 100), (int)(sy * 100), (int)(sz * 100), (int)x[3], (int)x[7], (int)x[11]);
                    }
                }
                if (nbad) rlog("  model %p: %lu of %lu nodes odd", (const void *)o, (unsigned long)nbad, (unsigned long)nnodes);
            }
        }
        object_matrix(o, campos, cr, cu, cf, om);
        nmodels++;
        {   /* name each model once (diagnostics) */
            static const uint8_t *named[256];
            static uint32_t nnamed;
            uint32_t j;
            for (j = 0; j < nnamed && named[j] != o; j++) ;
            if (j == nnamed && nnamed < 256) {
                const char *fn = (const char *)RDPTR(m, 0x04);
                if (!readable(fn, 8) || fn[0] < 0x20 || fn[0] > 0x7E) fn = (const char *)(m + 0x04);   /* inline name */
                const float *op = (const float *)(o + OBJ_POS);
                named[nnamed++] = o;
                rlog("model %p: %s at %d,%d,%d flags %08lx flags2 %08lx rgba %02x%02x%02x%02x", (const void *)o,
                     (fn[0] >= 0x20 && fn[0] <= 0x7E) ? fn : "?", (int)op[0], (int)op[1], (int)op[2],
                     (unsigned long)RD32(o, OBJ_FLAGS), (unsigned long)RD32(o, OBJ_FLAGS2), o[0x94], o[0x95], o[0x96], o[0x97]);
            }
        }
        {   /* the stock renderer sorts models with alpha < 255 into a translucent list */
            uint32_t a8 = o[0x97];
            mblend = (RD32(o, OBJ_FLAGS2) & 4) ? 2 : (a8 < 255 && g_ps_mdl) ? 1 : 0;
            malpha = mblend ? (a8 << 24) : 0;
        }
        {   /* ambient from the world light grid at the model, tinted by the object color */
            float amb[3] = { 160, 160, 160 };
            const float *lp = (RD32(o, OBJ_FLAGS) & 0x40) ? campos : (const float *)(o + OBJ_POS);
            if (world) {
                int ok = lightgrid_sample(world, lp, amb);
                if (mlog < 12 && (lp[0] != 0 || lp[1] != 0 || lp[2] != 0))
                    rlog("  model %p at %d,%d,%d: light grid %s %d,%d,%d", (void *)o, (int)lp[0], (int)lp[1], (int)lp[2],
                         ok ? "ok" : "unavailable", (int)amb[0], (int)amb[1], (int)amb[2]);
                if (glog < 1 && nmodels == 1) {
                    const uint8_t *g = world + 0xFC;
                    rlog_dump("  lightgrid", g, 0x48);
                }
            }
            /* Stock formula (measured from the original renderer with the trace probe):
             * light = add + color x (grid + global light x max(0, N.dir) + point lights),
             * with color = object color / 255 x 2. */
            {
                float hadd[3], hcol[3];
                run_model_hook(o, hadd, hcol);
                if (mlog < 12 && (lp[0] != 0 || lp[1] != 0 || lp[2] != 0))
                    rlog("    hook: light add %d,%d,%d color %d,%d,%d", (int)hadd[0], (int)hadd[1], (int)hadd[2],
                         (int)hcol[0], (int)hcol[1], (int)hcol[2]);
                for (k = 0; k < 3; k++) {
                    float c = hcol[k] / 255.0f;
                    mcol[k] = c < 0 ? 0 : c;
                    mcol[k] *= g_model_sat;
                    mamb[k] = hadd[k] / 255.0f + amb[k] / 255.0f * mcol[k];
                }
            }
            gather_model_lights(world, lp, readable(m, 0xAC) && *(const float *)(m + 0xA8) > 0 ?
                                *(const float *)(m + 0xA8) : 64.0f);
            if (mlog < 12 && (lp[0] != 0 || lp[1] != 0 || lp[2] != 0)) {
                rlog("    %d lights%s, global light %d,%d,%d dir %d,%d,%d (x100)", g_nmlights,
                     (RD32(o, OBJ_FLAGS) & 0x40) ? " (camera-space model)" : "",
                     (int)(g_sun_col[0] * 25500), (int)(g_sun_col[1] * 25500), (int)(g_sun_col[2] * 25500),
                     (int)(g_sun_dir[0] * 100), (int)(g_sun_dir[1] * 100), (int)(g_sun_dir[2] * 100));
                mlog++;
            }
        }
        {   /* diagnostics: vertex weights that point past the node list, too many weights, or don't sum to 1 */
            static const uint8_t *seenw[64];
            static int nseenw;
            int q;
            for (q = 0; q < nseenw && seenw[q] != o; q++) ;
            if (q == nseenw && nseenw < 64) {
                uint32_t p2, badnode = 0, toomany = 0, badsum = 0, total = 0, maxnode = 0;
                seenw[nseenw++] = o;
                for (p2 = 0; p2 < npieces; p2++) {
                    const uint8_t *pc2 = RDPTR(pieces, p2 * 4), *vs;
                    uint32_t nvs, v2;
                    if (!readable(pc2, 0x44)) continue;
                    vs = RDPTR(pc2, 0x04); nvs = RD32(pc2, 0x08);
                    if (!nvs || nvs > 4096 || !readable(vs, nvs * 0x20)) continue;
                    for (v2 = 0; v2 < nvs; v2++) {
                        const uint8_t *vv2 = vs + v2 * 0x20, *w2 = RDPTR(vv2, 0);
                        uint32_t nw2 = *(const uint16_t *)(vv2 + 4), k2;
                        float sum = 0;
                        total++;
                        if (nw2 > 32) { toomany++; continue; }
                        if (!readable(w2, nw2 * 0x14)) continue;
                        for (k2 = 0; k2 < nw2; k2++) {
                            uint32_t nd = *(const uint32_t *)(w2 + k2 * 0x14 + 0x10);
                            sum += ((const float *)(w2 + k2 * 0x14))[3];
                            if (nd >= nnodes) badnode++;
                            if (nd > maxnode) maxnode = nd;
                        }
                        if (nw2 && (sum < 0.95f || sum > 1.05f)) badsum++;   /* normalised when skinning */
                    }
                }
                if (badnode || toomany || badsum)
                    rlog("  weights of model %p: %lu verts, %lu weights past node %lu (max node %lu), %lu with >32 weights, %lu sums off",
                         (const void *)o, (unsigned long)total, (unsigned long)badnode, (unsigned long)nnodes,
                         (unsigned long)maxnode, (unsigned long)toomany, (unsigned long)badsum);
            }
        }
        /* hidden pieces: one bit per piece index at object +0x24C (AVP2) or
           +0x260 (Primal Hunt), tested by the stock renderer before drawing */
        hidden = readable(o, 0x264) ? RD32(o, g_model_off == 0x1FC ? 0x260 : 0x24C) : 0;
        {   /* diagnostics: describe each model object's pieces once */
            static const uint8_t *seenm[64];
            static int nseenm;
            int q;
            for (q = 0; q < nseenm && seenm[q] != o; q++) ;
            if (q == nseenm && nseenm < 32) {
                char line[512];
                int len;
                uint32_t pj;
                seenm[nseenm++] = o;
                len = sprintf(line, "model %p pieces %lu hidden %08lx skins %08lx %08lx %08lx %08lx %08lx:", (const void *)o,
                              (unsigned long)npieces, (unsigned long)hidden, (unsigned long)RD32(o, 0x1C0), (unsigned long)RD32(o, 0x1C4),
                              (unsigned long)RD32(o, 0x1C8), (unsigned long)RD32(o, 0x1CC), (unsigned long)RD32(o, 0x1D0));
                for (pj = 0; pj < npieces && pj < 24 && len < 440; pj++) {
                    const uint8_t *pp = RDPTR(pieces, pj * 4);
                    len += sprintf(line + len, " %lu", readable(pp, 0x44) ? (unsigned long)RD32(pp, 0x24) : 99ul);
                }
                rlog("%s", line);
            }
        }
        for (pi = 0; pi < npieces; pi++) {
            const uint8_t *pc = RDPTR(pieces, pi * 4), *verts, *tris;
            uint32_t nverts, ntris, vi, ti, skin;
            RTexture *tex = NULL;
            if (pi < 32 && (hidden & (1u << pi))) continue;
            if (!readable(pc, 0x44)) continue;
            verts = RDPTR(pc, 0x04); nverts = RD32(pc, 0x08);
            tris = RDPTR(pc, 0x14); ntris = RD32(pc, 0x18);
            if (!nverts || nverts > 4096 || !ntris || !readable(verts, nverts * 0x20) || !readable(tris, ntris * 0x20))
                continue;
            if (nv_out + ntris * 3 > MAX_MODEL_VERTS || nd >= 1024) break;
            skin = RD32(pc, 0x24);   /* skin index: piece +0x24, as both stock renderers read it (d3d.ren 0x1000447a) */
            if (skin > 3) skin = 0;
            if (RD32(o, 0x1C0 + skin * 4)) tex = get_texture((void *)(uintptr_t)RD32(o, 0x1C0 + skin * 4));
            if (!tex && RD32(o, 0x1C0)) tex = get_texture((void *)(uintptr_t)RD32(o, 0x1C0));
            for (vi = 0; vi < nverts; vi++) {
                const uint8_t *v = verts + vi * 0x20, *w = RDPTR(v, 0);
                uint32_t nw = *(const uint16_t *)(v + 4), k;
                float acc[3] = { 0, 0, 0 }, nrm[3] = { 0, 0, 0 }, wp[3], n2[3], wsum = 0;
                if (nw > 32 || !readable(w, nw * 0x14)) nw = 0;
                for (k = 0; k < nw; k++) {
                    const float *ww = (const float *)(w + k * 0x14);
                    uint32_t node = *(const uint32_t *)(w + k * 0x14 + 0x10);
                    float t[3], tn[3];
                    if (node >= nnodes) continue;
                    xform34(nodes + node * 16, ww, t);
                    rot33(nodes + node * 16, (const float *)(v + 0x14), tn);
                    acc[0] += t[0] * ww[3]; acc[1] += t[1] * ww[3]; acc[2] += t[2] * ww[3];
                    nrm[0] += tn[0] * ww[3]; nrm[1] += tn[1] * ww[3]; nrm[2] += tn[2] * ww[3];
                    wsum += ww[3];
                }
                /* Some Primal Hunt models (dunya.abc, merc*.abc) list the same weight several
                 * times, e.g. head node x6 at 1.0 each; a plain sum pushes those vertices out
                 * along their bone (the stretched head spikes the stock renderer also shows).
                 * Normalising by the weight total puts them back where the artist placed them. */
                if (g_skin_normalize && wsum > 0.001f && (wsum < 0.99f || wsum > 1.01f)) {
                    float inv = 1.0f / wsum;
                    for (k = 0; k < 3; k++) { acc[k] *= inv; nrm[k] *= inv; }
                }
                if (!nw) memcpy(acc, v + 8, sizeof acc);
                xform34(om, acc, wp);
                memcpy(skinned + vi * 3, wp, sizeof wp);
                rot33(om, nrm, n2);
                {   /* light grid ambient (gentle top-down term) plus point lights */
                    float len = sqrtf(n2[0] * n2[0] + n2[1] * n2[1] + n2[2] * n2[2]) + 1e-6f;
                    float pl[3] = { 0, 0, 0 };
                    uint32_t ch[3];
                    int li;
                    for (li = 0; li < g_nmlights; li++) {
                        const MLight *L = &g_mlights[li];
                        float d[3] = { L->pos[0] - wp[0], L->pos[1] - wp[1], L->pos[2] - wp[2] };
                        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
                        float ndl = (d[0] * n2[0] + d[1] * n2[1] + d[2] * n2[2]) / (dist * len);
                        float att = 1.0f - dist / L->radius;
                        if (att <= 0 || ndl <= 0) continue;
                        pl[0] += L->col[0] * ndl * att;
                        pl[1] += L->col[1] * ndl * att;
                        pl[2] += L->col[2] * ndl * att;
                    }
                    if (g_sun_on) {   /* global (sky) light */
                        float sd = (n2[0] * g_sun_dir[0] + n2[1] * g_sun_dir[1] + n2[2] * g_sun_dir[2]) / len;
                        if (sd > 0) for (k = 0; k < 3; k++) pl[k] += g_sun_col[k] * sd;
                    }
                    for (k = 0; k < 3; k++) {
                        float l = (mamb[k] + pl[k] * mcol[k]) * g_lmscale;
                        ch[k] = (uint32_t)((l < 0 ? 0 : l > 1 ? 1 : l) * 255.0f);
                    }
                    light[vi] = ch[0] | (ch[1] << 8) | (ch[2] << 16);
                }
            }
            list[nd].tex = tex;
            list[nd].first = nv_out;
            list[nd].blend = mblend;
            for (ti = 0; ti < ntris; ti++) {
                const uint8_t *t = tris + ti * 0x20;
                const float *uv = (const float *)(t + 8);
                int c;
                for (c = 0; c < 3; c++) {
                    uint32_t idx = ((const uint16_t *)t)[c];
                    WVertex *w = vv + nv_out;
                    if (idx >= nverts) idx = 0;
                    w->x = skinned[idx * 3]; w->y = skinned[idx * 3 + 1]; w->z = skinned[idx * 3 + 2];
                    w->u = uv[c * 2]; w->v = uv[c * 2 + 1];
                    w->lu = w->lv = 0;
                    w->rgba = 0;
                    cc[nv_out] = light[idx] | malpha;
                    nv_out++;
                }
            }
            list[nd].count = nv_out - list[nd].first;
            nd++;
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_mdl_vb, 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_mdl_cvb, 0);
    if (logged < 2) { rlog("models: %lu drawn, %lu pieces, %lu vertices", (unsigned long)nmodels, (unsigned long)nd, (unsigned long)nv_out); logged++; }
    if (world && nmodels && glog < 1) {
        rlog_dump("  rs+100", (const uint8_t *)g_rs + 0x100, 0x60);
        glog++;
    }
    if (!nd) return;
    {
        ID3D11Buffer *vbs[2] = { g_mdl_vb, g_mdl_cvb };
        UINT strides[2] = { sizeof(WVertex), 4 }, offs[2] = { 0, 0 };
        ID3D11SamplerState *ss[2] = { g_samp_wrap, g_samp_linear };
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 2, vbs, strides, offs);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps3d, NULL, 0);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 2, ss);
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_on, 0);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[OPT2D_SOLID], bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
    }
    for (i = 0; i < nd; i++) {
        ID3D11ShaderResourceView *srv[2];
        if (list[i].blend) { ntrans++; continue; }
        srv[0] = list[i].tex ? list[i].tex->srv : g_white_srv;
        srv[1] = g_white_srv;
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srv);
        ID3D11DeviceContext_Draw(g_ctx, list[i].count, list[i].first);
    }
    if (ntrans) {   /* translucent and additive models: after the solid ones, depth test without writes */
        int cur = -1;
        static int tlog;
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_mdl, NULL, 0);
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_test, 0);
        for (i = 0; i < nd; i++) {
            ID3D11ShaderResourceView *srv[2];
            if (!list[i].blend) continue;
            if (list[i].blend != cur) {
                cur = list[i].blend;
                ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[cur == 2 ? OPT2D_ADD : OPT2D_ALPHA], bf, 0xFFFFFFFFu);
            }
            srv[0] = list[i].tex ? list[i].tex->srv : g_white_srv;
            srv[1] = g_white_srv;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srv);
            ID3D11DeviceContext_Draw(g_ctx, list[i].count, list[i].first);
        }
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_on, 0);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[OPT2D_SOLID], bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps3d, NULL, 0);
        if (tlog < 5) { rlog("models: %lu translucent or additive pieces", (unsigned long)ntrans); tlog++; }
    }
}

/* ------------------------------------------------------------------ */
/* Sprites                                                              */
/* ------------------------------------------------------------------ */
/* Sprite objects (type 3): +0x1B4 sprite, +0x1B8 pointer to the current
 * frame entry whose first dword is the frame's SharedTexture. Color bytes at
 * +0x94..+0x97 (r, g, b, a). Flags (+0x88): 0x08 rotatable (oriented by the
 * object's rotation), 0x40 "really close" (position in camera space).
 * Flags 2 (+0x8C): 0x04 additive, 0x08 multiply, otherwise alpha blend. */
#define SPR_ADD   0
#define SPR_MUL   1
#define SPR_ALPHA 2
#define SPR_SOLID 3
typedef struct SprDraw { RTexture *tex; int blend; float depth; UINT first, count; int nodepth; } SprDraw;
static float g_spr_scale = 1.0f;        /* half size per texel per unit scale: the stock renderer puts the corners at +-texture size (d3d.ren 0x1002e310) */
static float g_particle_scale = 1.0f;   /* extra particle size multiplier; cvar avp2m_psize x100 */
static float g_cur_xfov = 1.5708f;      /* horizontal FOV of the current scene (radians) */

static int cmp_spr(const void *a, const void *b)
{
    const SprDraw *p = (const SprDraw *)a, *q = (const SprDraw *)b;
    if (p->blend != q->blend) {   /* opaque, additive, multiply, then far-to-near alpha */
        int pa = p->blend == SPR_SOLID ? -1 : p->blend, qa = q->blend == SPR_SOLID ? -1 : q->blend;
        return pa - qa;
    }
    return (p->depth < q->depth) - (p->depth > q->depth);
}

/* Canvases (object type 10) are drawn by the game itself: the renderer calls
 * the client function at canvas+0x1B0 as fn(iface, canvas, user data at
 * +0x1B4), and the client draws through iface, a C++ object whose vtable has
 * five thiscall methods (stock d3d.ren vtable 0x100463C8):
 *   0 DrawPrimitive(verts, count, flags): triangle fan of 0x20-byte vertices
 *     {x, y, z, rhw, diffuse ARGB, specular, u, v}; world space unless the
 *     canvas is "really close" (object flag 0x40) or flags & 0x40, then
 *     camera space
 *   1 SetState(state, value)   2 GetState(state, *value)
 *     0 alpha blend on/off, 1 z test, 2 z write, 3 src blend, 4 dest blend
 *     (1 zero 2 one 3 src color 4 inv src color 5 src alpha 6 inv src alpha),
 *     5 texture address, 6 colour op, 7 alpha op
 *     (1 texture 2 diffuse 3 modulate 4 add 5 add signed)
 *   3 SetTexture(handle; engine callback 0 maps it to a SharedTexture)
 *   4 GetTextureSize(int *w, int *h) */
typedef struct CanvasCtx {
    WVertex *vv; uint32_t *cc; uint32_t *nv; SprDraw *list; uint32_t *n;
    const float *campos, *cr, *cu, *cf;
    const uint8_t *obj; int camspace;
} CanvasCtx;
typedef void (__cdecl *CanvasFn)(void *iface, void *canvas, void *user);
typedef struct CanvasIface { void **vtbl; } CanvasIface;
static CanvasCtx *g_cv;
static int32_t g_cv_state[8];
static RTexture *g_cv_tex;
static void *g_cv_lasttex;
static uint32_t g_cv_stat[3];
static int g_cv_log;

static void cv_reset(void)
{
    static const int32_t def[8] = { 0, 1, 1, 5, 6, 1, 3, 3 };
    memcpy(g_cv_state, def, sizeof def);
    g_cv_tex = NULL;
}

static int __attribute__((thiscall)) cv_draw(void *self, const float *v, int count, uint32_t flags)
{
    CanvasCtx *c = g_cv;
    int t, k, cam, blend;
    float dsum = 0;
    (void)self;
    g_cv_stat[0]++;
    if (!c || !v || count < 3) return 1;
    if (*c->n >= MAX_SPRITES || *c->nv + (uint32_t)(count - 2) * 3 > MAX_SPRITES * 6u) return 0;
    if (!readable(v, (UINT_PTR)count * 32)) return 1;
    cam = c->camspace || (flags & 0x40);
    {
        static int vlog;
        if (vlog < 4) {
            int q;
            vlog++;
            rlog("cv prim: %d verts flags %lx cam %d states %ld %ld %ld %ld %ld %ld %ld %ld tex %p campos %d,%d,%d",
                 count, (unsigned long)flags, cam, (long)g_cv_state[0], (long)g_cv_state[1], (long)g_cv_state[2],
                 (long)g_cv_state[3], (long)g_cv_state[4], (long)g_cv_state[5], (long)g_cv_state[6], (long)g_cv_state[7],
                 (void *)g_cv_tex, (int)c->campos[0], (int)c->campos[1], (int)c->campos[2]);
            for (q = 0; q < count && q < 6; q++)
                rlog("   v%d %d,%d,%d argb %08lx uv %d,%d (x100)", q, (int)v[q * 8], (int)v[q * 8 + 1], (int)v[q * 8 + 2],
                     (unsigned long)((const uint32_t *)v)[q * 8 + 4], (int)(v[q * 8 + 6] * 100), (int)(v[q * 8 + 7] * 100));
        }
    }
    {
        int en = g_cv_state[0], sb = g_cv_state[3], db = g_cv_state[4];
        if (!en) blend = SPR_SOLID;
        else if (db == 2) blend = SPR_ADD;                          /* src x factor + dest */
        else if ((sb == 1 && db == 3) || (sb == 3 && db == 1)) blend = SPR_MUL;
        else blend = SPR_ALPHA;
    }
    {
        SprDraw *d = &c->list[*c->n];
        WVertex *w = c->vv + *c->nv;
        uint32_t *cc = c->cc + *c->nv;
        int cop = g_cv_state[6], aop = g_cv_state[7];
        for (t = 0; t < count - 2; t++) {
            int idx[3];
            idx[0] = 0; idx[1] = t + 1; idx[2] = t + 2;
            for (k = 0; k < 3; k++) {
                const float *sv = v + idx[k] * 8;
                uint32_t argb = ((const uint32_t *)sv)[4], a, col;
                float p[3];
                int q;
                if (cam) for (q = 0; q < 3; q++) p[q] = c->campos[q] + sv[0] * c->cr[q] + sv[1] * c->cu[q] + sv[2] * c->cf[q];
                else for (q = 0; q < 3; q++) p[q] = sv[q];
                w->x = p[0]; w->y = p[1]; w->z = p[2];
                w->u = sv[6]; w->v = sv[7];
                w->lu = w->lv = 0; w->rgba = 0;
                a = argb >> 24;
                col = ((argb >> 16) & 0xFF) | (argb & 0xFF00) | ((argb & 0xFF) << 16);
                if (cop == 1) col = 0xFFFFFF;               /* texture only */
                if (aop == 1) a = 255;
                *cc++ = col | (a << 24);
                dsum += (p[0] - c->campos[0]) * c->cf[0] + (p[1] - c->campos[1]) * c->cf[1] + (p[2] - c->campos[2]) * c->cf[2];
                w++;
            }
        }
        d->tex = (cop == 2) ? NULL : g_cv_tex;
        d->blend = blend;
        d->depth = dsum / (float)((count - 2) * 3);
        d->first = *c->nv;
        d->count = (uint32_t)(count - 2) * 3;
        d->nodepth = !g_cv_state[1];
        *c->nv += d->count;
        (*c->n)++;
        g_cv_stat[1] += (uint32_t)(count - 2);
    }
    return 0;
}

static int __attribute__((thiscall)) cv_setstate(void *self, uint32_t st, int32_t val)
{
    (void)self;
    if (st > 7) return 1;
    g_cv_state[st] = val;
    return 0;
}

static int __attribute__((thiscall)) cv_getstate(void *self, uint32_t st, int32_t *val)
{
    (void)self;
    if (st > 7) return 1;
    if (val) *val = g_cv_state[st];
    return 0;
}

static int __attribute__((thiscall)) cv_settexture(void *self, void *shared)
{
    (void)self;
    g_cv_stat[2]++;
    /* the client passes a texture handle; engine callback 0 (RenderStruct
       +0x04) turns it into the SharedTexture, as the stock renderer does */
    g_cv_lasttex = shared;
    if (shared && g_rs && g_rs->engine_callbacks[0])
        shared = ((void *(__cdecl *)(void *))(void *)g_rs->engine_callbacks[0])(shared);
    else
        shared = NULL;
    g_cv_tex = shared ? get_texture(shared) : NULL;
    return !g_cv_tex;
}

static int __attribute__((thiscall)) cv_texsize(void *self, int32_t *w, int32_t *h)
{
    (void)self;
    if (w) *w = g_cv_tex ? (int32_t)g_cv_tex->w : 0;
    if (h) *h = g_cv_tex ? (int32_t)g_cv_tex->h : 0;
    return 0;
}

static void *g_cv_vtbl[5] = { (void *)cv_draw, (void *)cv_setstate, (void *)cv_getstate,
                              (void *)cv_settexture, (void *)cv_texsize };
static CanvasIface g_cv_iface = { g_cv_vtbl };

static void draw_sprites(const float *campos, const float *cr, const float *cu, const float *cf)
{
    static SprDraw list[MAX_SPRITES];
    D3D11_MAPPED_SUBRESOURCE mv, mc;
    WVertex *vv;
    uint32_t *cc, i, n = 0, nspr = 0, nv = 0, ncv = 0;
    float bf[4] = { 0, 0, 0, 0 };
    static int logged;

    if (!g_ps_spr || !g_spr_vb) return;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_spr_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mv))) return;
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_spr_cvb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mc))) {
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_spr_vb, 0);
        return;
    }
    vv = (WVertex *)mv.pData;
    cc = (uint32_t *)mc.pData;
    for (i = 0; i < g_nvis_objs && n < MAX_SPRITES; i++) {
        const uint8_t *o = g_vis_objs[i], *frame;
        RTexture *tex;
        const float *pos, *sc;
        float p[3], r[3], u[3], hx, hy, d;
        uint32_t fl = RD32(o, OBJ_FLAGS), fl2 = RD32(o, OBJ_FLAGS2), col;
        int k;
        if (o[OBJ_TYPE] != 3 || !readable(o, 0x1C0)) continue;
        frame = RDPTR(o, 0x1B8);
        if (!RD32(o, 0x1B4) || !readable(frame, 4) || !RD32(frame, 0)) continue;
        tex = get_texture((void *)(uintptr_t)RD32(frame, 0));
        if (!tex) continue;
        pos = (const float *)(o + OBJ_POS);
        sc = (const float *)(o + 0xAC);
        if (fl & 0x40) {          /* camera space */
            for (k = 0; k < 3; k++) p[k] = campos[k] + pos[0] * cr[k] + pos[1] * cu[k] + pos[2] * cf[k];
        } else {
            for (k = 0; k < 3; k++) p[k] = pos[k];
        }
        if (fl & 0x08) {          /* rotatable: oriented by the object's rotation */
            float rf[3];
            quat_basis((const float *)(o + 0x9C), r, u, rf);
            if (fl & 0x40) {
                float rr[3], uu[3];
                for (k = 0; k < 3; k++) {
                    rr[k] = r[0] * cr[k] + r[1] * cu[k] + r[2] * cf[k];
                    uu[k] = u[0] * cr[k] + u[1] * cu[k] + u[2] * cf[k];
                }
                memcpy(r, rr, sizeof r); memcpy(u, uu, sizeof u);
            }
        } else {
            memcpy(r, cr, sizeof r); memcpy(u, cu, sizeof u);
        }
        hx = (float)tex->bw * sc[0] * g_spr_scale;   /* base size: sprite size doesn't follow texture detail */
        hy = (float)tex->bh * sc[1] * g_spr_scale;
        d = (p[0] - campos[0]) * cf[0] + (p[1] - campos[1]) * cf[1] + (p[2] - campos[2]) * cf[2];
        {   /* menu sprites (draw mode 2): describe each once (diagnostics) */
            static const uint8_t *seen[32];
            static int nseen;
            int q;
            if (g_cur_sd && RD32(g_cur_sd, 0) == 2) {
                for (q = 0; q < nseen && seen[q] != o; q++) ;
                if (q == nseen && nseen < 32) {
                    seen[nseen++] = o;
                    rlog("menu sprite %p: pos x100 %d,%d,%d scale x100 %d,%d flags %08lx flags2 %08lx tex %lux%lu (base %lux%lu) rgba %02x%02x%02x%02x xfov x100 %d",
                         (const void *)o, (int)(pos[0] * 100), (int)(pos[1] * 100), (int)(pos[2] * 100),
                         (int)(sc[0] * 100), (int)(sc[1] * 100), (unsigned long)fl, (unsigned long)fl2,
                         (unsigned long)tex->w, (unsigned long)tex->h, (unsigned long)tex->bw, (unsigned long)tex->bh,
                         o[0x94], o[0x95], o[0x96], o[0x97], (int)(g_cur_xfov * 100));
                }
            }
        }
        if (d < 1.0f && !(fl & 0x08)) continue;
        col = (uint32_t)o[0x94] | ((uint32_t)o[0x95] << 8) | ((uint32_t)o[0x96] << 16) | ((uint32_t)o[0x97] << 24);
        {
            static const float cx[6] = { -1, 1, 1, -1, 1, -1 }, cy[6] = { 1, 1, -1, 1, -1, -1 };
            WVertex *w = vv + nv;
            for (k = 0; k < 6; k++) {
                int a;
                for (a = 0; a < 3; a++) (&w[k].x)[a] = p[a] + r[a] * hx * cx[k] + u[a] * hy * cy[k];
                w[k].u = cx[k] > 0 ? 1.0f : 0.0f;
                w[k].v = cy[k] > 0 ? 0.0f : 1.0f;
                w[k].lu = w[k].lv = 0;
                w[k].rgba = 0;
                cc[nv + k] = col;
            }
        }
        list[n].tex = tex;
        list[n].blend = (fl2 & 4) ? SPR_ADD : (fl2 & 8) ? SPR_MUL : SPR_ALPHA;
        list[n].depth = d;
        list[n].first = nv; list[n].count = 6; list[n].nodepth = 0;
        nv += 6;
        n++;
    }

    /* Particle systems (type 6): +0x1F0 SharedTexture, +0x1D0 first particle,
     * +0x230 particle count; the list ends at the sentinel at +0x1B0.
     * Particle: +0x0C color (3 floats, 0..255), +0x18 alpha (0..1), +0x1C size,
     * +0x20 next, +0x24 position in system space (the object's transform
     * applies). Drawn as camera-facing quads. */
    nspr = n;
    for (i = 0; i < g_nvis_objs && n < MAX_SPRITES; i++) {
        const uint8_t *o = g_vis_objs[i], *pt, *sentinel;
        RTexture *tex;
        float om[12];
        uint32_t fl2, cnt, guard = 0;
        int blend;
        if (o[OBJ_TYPE] != 6 || !readable(o, 0x240) || !RD32(o, 0x1F0)) continue;
        tex = get_texture((void *)(uintptr_t)RD32(o, 0x1F0));
        if (!tex) continue;
        object_matrix(o, campos, cr, cu, cf, om);
        fl2 = RD32(o, OBJ_FLAGS2);
        blend = (fl2 & 4) ? SPR_ADD : (fl2 & 8) ? SPR_MUL : SPR_ALPHA;
        sentinel = o + 0x1B0;
        cnt = RD32(o, 0x230);
        for (pt = RDPTR(o, 0x1D0); pt && pt != sentinel && guard < cnt + 1 && n < MAX_SPRITES; pt = RDPTR(pt, 0x20), guard++) {
            const float *pf;
            float p[3], hs, d;
            uint32_t col, cr8, cg8, cb8, ca8;
            int k;
            if (!readable(pt, 0x30)) break;
            pf = (const float *)pt;
            xform34(om, pf + 9, p);                    /* +0x24 */
            d = (p[0] - campos[0]) * cf[0] + (p[1] - campos[1]) * cf[1] + (p[2] - campos[2]) * cf[2];
            if (d < 1.0f) continue;
            /* The stock renderer sizes particles in screen space: half size in pixels =
             * size x viewport width x K / depth, with K = 1.0 (measured with the trace
             * probe). In world units that is size x 2 x tan(xfov / 2). */
            hs = pf[7] * 2.0f * (float)tan(g_cur_xfov * 0.5) * g_particle_scale;   /* +0x1C */
            if (!(hs > 0)) continue;
            cr8 = (uint32_t)(fminf(fmaxf(pf[3] * o[0x94] / 255.0f, 0), 255));
            cg8 = (uint32_t)(fminf(fmaxf(pf[4] * o[0x95] / 255.0f, 0), 255));
            cb8 = (uint32_t)(fminf(fmaxf(pf[5] * o[0x96] / 255.0f, 0), 255));
            ca8 = (uint32_t)(fminf(fmaxf(pf[6] * o[0x97], 0), 255));
            col = cr8 | (cg8 << 8) | (cb8 << 16) | (ca8 << 24);
            {
                static const float cx[6] = { -1, 1, 1, -1, 1, -1 }, cy[6] = { 1, 1, -1, 1, -1, -1 };
                WVertex *w = vv + nv;
                for (k = 0; k < 6; k++) {
                    int a;
                    for (a = 0; a < 3; a++) (&w[k].x)[a] = p[a] + cr[a] * hs * cx[k] + cu[a] * hs * cy[k];
                    w[k].u = cx[k] > 0 ? 1.0f : 0.0f;
                    w[k].v = cy[k] > 0 ? 0.0f : 1.0f;
                    w[k].lu = w[k].lv = 0;
                    w[k].rgba = 0;
                    cc[nv + k] = col;
                }
            }
            list[n].tex = tex;
            list[n].blend = blend;
            list[n].depth = d;
            list[n].first = nv; list[n].count = 6; list[n].nodepth = 0;
            nv += 6;
            n++;
        }
    }
    {   /* canvases (type 10): the game draws them itself through g_cv_iface */
        CanvasCtx cx;
        cx.vv = vv; cx.cc = cc; cx.nv = &nv; cx.list = list; cx.n = &n;
        cx.campos = campos; cx.cr = cr; cx.cu = cu; cx.cf = cf;
        g_cv = &cx;
        for (i = 0; i < g_nvis_objs && n < MAX_SPRITES; i++) {
            const uint8_t *o = g_vis_objs[i];
            CanvasFn fn;
            if (o[OBJ_TYPE] != 10 || !readable(o, 0x1B8)) continue;
            if (g_cv_log < 3) {
                rlog("canvas %p: fn %08lx user %08lx flags %08lx flags2 %08lx +188 %08lx pos %d,%d,%d", (const void *)o,
                     (unsigned long)RD32(o, 0x1B0), (unsigned long)RD32(o, 0x1B4), (unsigned long)RD32(o, OBJ_FLAGS),
                     (unsigned long)RD32(o, OBJ_FLAGS2), (unsigned long)RD32(o, 0x188),
                     (int)((const float *)(o + OBJ_POS))[0], (int)((const float *)(o + OBJ_POS))[1], (int)((const float *)(o + OBJ_POS))[2]);
                g_cv_log++;
            }
            if (!RD32(o, 0x1B0)) continue;
            if (RD32(o, OBJ_FLAGS2) & 0x100) continue;
            fn = (CanvasFn)(uintptr_t)RD32(o, 0x1B0);
            cx.obj = o;
            cx.camspace = (RD32(o, OBJ_FLAGS) & 0x40) != 0;
            cv_reset();
            fn(&g_cv_iface, (void *)o, (void *)(uintptr_t)RD32(o, 0x1B4));
            ncv++;
        }
        g_cv = NULL;
        if (ncv && g_cv_log < 8) {
            const uint8_t *co = NULL;
            const float *op;
            for (i = 0; i < g_nvis_objs && !co; i++) if (g_vis_objs[i][OBJ_TYPE] == 10) co = g_vis_objs[i];
            op = (const float *)(co + OBJ_POS);
            rlog("canvas: %lu drawn, %lu prims, %lu tris, settex %lu (last %p ok %d), fn %08lx flags %08lx pos %d,%d,%d",
                 (unsigned long)ncv, (unsigned long)g_cv_stat[0], (unsigned long)g_cv_stat[1], (unsigned long)g_cv_stat[2],
                 g_cv_lasttex, g_cv_tex != NULL, (unsigned long)RD32(co, 0x1B0),
                 (unsigned long)RD32(co, OBJ_FLAGS), (int)op[0], (int)op[1], (int)op[2]);
            g_cv_log++;
        }
    }
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_spr_vb, 0);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_spr_cvb, 0);
    if (logged < 2) { rlog("sprites: %lu drawn", (unsigned long)n); logged++; }
    {   /* in-game particle diagnostics (menu frames don't count): log when the
           number of visible systems changes, or the quad count jumps, or every 5 s */
        static int plog;
        static DWORD plast;
        static uint32_t prev_quads, prev_sys = 0xFFFFFFFFu;
        if (g_cur_sd && RD32(g_cur_sd, 0) == 1) {
            uint32_t q, nsys = 0, quads = n - nspr;
            DWORD now = GetTickCount();
            for (q = 0; q < g_nvis_objs; q++) if (g_vis_objs[q][OBJ_TYPE] == 6) nsys++;
            {   /* describe each newly seen particle system once (up to 300) and
                   count object types the renderer doesn't draw yet */
                static const uint8_t *seen[64];
                static uint32_t nseen, ndesc, tlast_log;
                uint32_t tc[16] = { 0 };
                for (q = 0; q < g_nvis_objs; q++) {
                    const uint8_t *o = g_vis_objs[q];
                    uint32_t t = o[OBJ_TYPE], j;
                    if (t < 16) tc[t]++;
                    if (t != 6 || ndesc >= 300) continue;
                    for (j = 0; j < nseen && seen[j] != o; j++) ;
                    if (j < nseen) continue;
                    seen[nseen++ % 64] = o; if (nseen > 64) nseen = 64;
                    {
                        const uint8_t *pt = readable(o, 0x240) ? RDPTR(o, 0x1D0) : NULL;
                        void *st = readable(o, 0x240) ? (void *)(uintptr_t)RD32(o, 0x1F0) : NULL;
                        RTexture *tx = st ? get_texture(st) : NULL;
                        int sz = -1, a = -1, r = -1, g = -1, b = -1;
                        if (pt && pt != o + 0x1B0 && readable(pt, 0x30)) {
                            const float *pf = (const float *)pt;
                            sz = (int)(pf[7] * 100); a = (int)(pf[6] * 100);
                            r = (int)pf[3]; g = (int)pf[4]; b = (int)pf[5];
                        }
                        rlog("psys %p: tex %p (%s %ux%u) count %lu flags %08lx flags2 %08lx objcol %02x%02x%02x%02x first size x100 %d rgb %d,%d,%d alpha x100 %d",
                             (void *)o, st, tx ? "ok" : "NONE", tx ? tx->w : 0, tx ? tx->h : 0,
                             (unsigned long)RD32(o, 0x230), (unsigned long)RD32(o, OBJ_FLAGS),
                             (unsigned long)RD32(o, OBJ_FLAGS2), o[0x94], o[0x95], o[0x96], o[0x97],
                             sz, r, g, b, a);
                        ndesc++;
                    }
                }
                if ((tc[7] || tc[8] || tc[10]) && now - tlast_log > 1000 && ndesc < 300) {
                    rlog("undrawn types visible: polygrid %lu linesystem %lu canvas %lu",
                         (unsigned long)tc[7], (unsigned long)tc[8], (unsigned long)tc[10]);
                    tlast_log = now; ndesc++;
                }
            }
            int jump = quads > prev_quads + 16 || quads + 16 < prev_quads;
            if (plog < 2000 && (nsys != prev_sys || jump || now - plast > 5000)) {
                rlog("particles: %lu systems visible, %lu particle quads, %lu sprite quads",
                     (unsigned long)nsys, (unsigned long)quads, (unsigned long)nspr);
                plog++;
                plast = now;
                prev_sys = nsys;
                prev_quads = quads;
            }
        }
    }
    if (!n) return;
    qsort(list, n, sizeof list[0], cmp_spr);
    {
        ID3D11Buffer *vbs[2] = { g_spr_vb, g_spr_cvb };
        UINT strides[2] = { sizeof(WVertex), 4 }, offs[2] = { 0, 0 };
        ID3D11SamplerState *ss[2] = { g_samp_linear, g_samp_linear };
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 2, vbs, strides, offs);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_spr, NULL, 0);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 2, ss);
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_test, 0);
    }
    {
        int cur = -1, curnd = 0;
        for (i = 0; i < n; i++) {
            if (list[i].blend != cur) {
                cur = list[i].blend;
                ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[cur == SPR_ADD ? OPT2D_ADD : cur == SPR_MUL ? OPT2D_MULTIPLY :
                                                                   cur == SPR_SOLID ? OPT2D_SOLID : OPT2D_ALPHA],
                                                    bf, 0xFFFFFFFFu);
            }
            if (list[i].nodepth != curnd) {
                curnd = list[i].nodepth;
                ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, curnd ? g_ds_off : g_ds_test, 0);
            }
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, list[i].tex ? &list[i].tex->srv : &g_white_srv);
            ID3D11DeviceContext_Draw(g_ctx, list[i].count, list[i].first);
        }
        if (curnd) ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_test, 0);
    }
}

/* Object-to-world transform for the 3D shader: rows of the engine's 4x4
 * (row-major, translation in the last column). NULL = identity. */
static void set_world_rows(const float *m)
{
    static const float ident[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    D3D11_MAPPED_SUBRESOURCE map;
    if (!g_cbw) return;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cbw, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, m ? m : ident, 48);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cbw, 0);
    }
}

/* World models (type 2): +0x1B0 is their WorldBsp; +0x1F8 their current
 * transform (identity until a door or lift moves). */
#define WM_BSP 0x1B0
#define WM_XF  0x1F8
static void update_world_models(RenderContext *rc)
{
    uint32_t i, b;
    static int logged;
    uint32_t matched = 0, moved = 0;
    if (!rc->nbsps) return;
    memset(rc->bsp_xf, 0, rc->nbsps * sizeof *rc->bsp_xf);
    for (i = 0; i < g_nvis_objs; i++) {
        const uint8_t *o = g_vis_objs[i], *bsp;
        const float *m;
        if (o[OBJ_TYPE] != 2 || !readable(o, WM_XF + 64)) continue;
        bsp = RDPTR(o, WM_BSP);
        for (b = 0; b < rc->nbsps; b++) if (rc->bsps[b] == bsp) break;
        if (b == rc->nbsps) continue;
        matched++;
        m = (const float *)(o + WM_XF);
        if (m[0] != 1 || m[5] != 1 || m[10] != 1 || m[1] != 0 || m[2] != 0 || m[4] != 0 ||
            m[6] != 0 || m[8] != 0 || m[9] != 0 || m[3] != 0 || m[7] != 0 || m[11] != 0) {
            rc->bsp_xf[b] = m;
            moved++;
            if (logged < 1) {
                int k;
                for (k = 0; k < 4; k++)
                    rlog("  wm %p bsp %lu m[%d] = %d %d %d %d (x1000)", (const void *)o, (unsigned long)b, k,
                         (int)(m[k * 4] * 1000), (int)(m[k * 4 + 1] * 1000), (int)(m[k * 4 + 2] * 1000), (int)(m[k * 4 + 3] * 1000));
                rlog("  wm pos %d,%d,%d", (int)((const float *)(o + OBJ_POS))[0], (int)((const float *)(o + OBJ_POS))[1],
                     (int)((const float *)(o + OBJ_POS))[2]);
            }
        }
    }
    if (logged < 3 && (moved || logged == 0)) {
        logged++;
        rlog("world models: %lu matched to BSPs, %lu transformed", (unsigned long)matched, (unsigned long)moved);
    }
}

/* View * projection for a camera, written to the 3D constant buffer. */
static void write_camera(const float *pos, const float *rot, float xfov, float yfov, float nearz, float farz,
                         int hdr_on, float *r, float *u, float *f)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float m[16], sx, sy, a, bq;
    quat_basis(rot, r, u, f);
    sx = 1.0f / (float)tan(xfov * 0.5);
    sy = 1.0f / (float)tan(yfov * 0.5);
    a = farz / (farz - nearz);
    bq = -nearz * farz / (farz - nearz);
    /* row-vector convention: clip = [x y z 1] * M, M = View * Proj */
    m[0] = r[0] * sx; m[1] = u[0] * sy; m[2]  = f[0] * a; m[3]  = f[0];
    m[4] = r[1] * sx; m[5] = u[1] * sy; m[6]  = f[1] * a; m[7]  = f[1];
    m[8] = r[2] * sx; m[9] = u[2] * sy; m[10] = f[2] * a; m[11] = f[2];
    {
        float tx = -(pos[0] * r[0] + pos[1] * r[1] + pos[2] * r[2]);
        float ty = -(pos[0] * u[0] + pos[1] * u[1] + pos[2] * u[2]);
        float tz = -(pos[0] * f[0] + pos[1] * f[1] + pos[2] * f[2]);
        m[12] = tx * sx; m[13] = ty * sy; m[14] = tz * a + bq; m[15] = tz;
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb3d, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        /* HLSL mul(v, M) with default column-major packing needs the transpose */
        float *o = (float *)map.pData;
        int i, j;
        for (i = 0; i < 4; i++) for (j = 0; j < 4; j++) o[i * 4 + j] = m[j * 4 + i];
        o[16] = g_lmscale; o[17] = (float)g_lmdebug; o[18] = hdr_on ? 1.0f : 0.0f; o[19] = 0;
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb3d, 0);
    }
}

/* Sky (draw mode 1): SceneDesc +0x84/+0x90 sky box min/max, +0x9C/+0xA8 sky
 * view min/max (the sky camera position range), +0xB4/+0xB8 sky objects.
 * The sky objects (world models placed far outside the level) are drawn
 * first from the sky camera with the main camera's rotation; the level's
 * sky-flagged polygons are left out, so the sky shows through them. */
#define SD_SKY_MIN      0x84
#define SD_SKY_VIEWMIN  0x9C
#define SD_SKY_VIEWMAX  0xA8
#define SD_SKY_OBJS     0xB4
#define SD_SKY_COUNT    0xB8

static int mark_sky_bsps(RenderContext *rc, const uint8_t *sd, const float *campos, float *skypos)
{
    const uint8_t *list = RDPTR(sd, SD_SKY_OBJS);
    uint32_t n = RD32(sd, SD_SKY_COUNT), i, b, found = 0;
    const float *vmin = (const float *)(sd + SD_SKY_VIEWMIN), *vmax = (const float *)(sd + SD_SKY_VIEWMAX);
    int k;
    if (!rc->sky) return 0;
    memset(rc->sky, 0, rc->nbsps);
    if (!n || n > 4096 || !readable(list, n * 4)) return 0;
    for (i = 0; i < n; i++) {
        const uint8_t *o = RDPTR(list, i * 4), *bsp;
        if (!readable(o, 0x200) || o[OBJ_TYPE] != 2) continue;
        bsp = RDPTR(o, WM_BSP);
        for (b = 0; b < rc->nbsps; b++)
            if (rc->bsps[b] == bsp) { rc->sky[b] = 1; found++; break; }
    }
    /* sky camera: the view range is usually a single point; otherwise move
     * within it in proportion to the camera's place in the level */
    for (k = 0; k < 3; k++) {
        (void)campos;
        skypos[k] = 0.5f * (vmin[k] + vmax[k]);
    }
    return (int)found;
}

/* Dynamic lights on level surfaces: visible light objects (type 4) go to the
 * world shader (b2), nearest first, at most 16. Light = colour / 255 x
 * (1 - distance / radius) x N.L with the face normal; models get the same
 * lights on the CPU (gather_model_lights). count 0 turns them off (sky pass,
 * models). */
#define MAX_DLIGHTS 16
static float g_dl_scale = 1.0f;   /* cvar avp2m_dlscale x100 */
static int g_world_sun = 1;       /* cvar avp2m_worldsun */
static void write_dlights(const float *campos, int enable)
{
    D3D11_MAPPED_SUBRESOURCE map;
    float buf[4 + 8 * MAX_DLIGHTS + 8];
    float dd[MAX_DLIGHTS];
    uint32_t i, n = 0;
    static int dlog;
    if (!g_cbdl) return;
    memset(buf, 0, sizeof buf);
    if (enable && g_dl_scale > 0) {
        for (i = 0; i < g_nvis_objs; i++) {
            const uint8_t *o = g_vis_objs[i];
            const float *p;
            float rad, d2;
            uint32_t j, slot;
            if (o[OBJ_TYPE] != 4 || !readable(o, 0x1B4)) continue;
            p = (const float *)(o + OBJ_POS);
            rad = *(const float *)(o + 0x1B0);
            if (!(rad > 0) || (o[0x94] | o[0x95] | o[0x96]) == 0) continue;
            d2 = (p[0] - campos[0]) * (p[0] - campos[0]) + (p[1] - campos[1]) * (p[1] - campos[1]) + (p[2] - campos[2]) * (p[2] - campos[2]);
            if (n < MAX_DLIGHTS) slot = n++;
            else {   /* replace the farthest if this one is nearer */
                slot = 0;
                for (j = 1; j < n; j++) if (dd[j] > dd[slot]) slot = j;
                if (dd[slot] <= d2) continue;
            }
            dd[slot] = d2;
            buf[4 + slot * 4 + 0] = p[0]; buf[4 + slot * 4 + 1] = p[1]; buf[4 + slot * 4 + 2] = p[2];
            buf[4 + slot * 4 + 3] = rad;
            buf[4 + 4 * MAX_DLIGHTS + slot * 4 + 0] = o[0x94] / 255.0f;
            buf[4 + 4 * MAX_DLIGHTS + slot * 4 + 1] = o[0x95] / 255.0f;
            buf[4 + 4 * MAX_DLIGHTS + slot * 4 + 2] = o[0x96] / 255.0f;
            {   /* log each light object once */
                static const uint8_t *seen[128];
                static uint32_t nseen;
                uint32_t q;
                for (q = 0; q < nseen && seen[q] != o; q++) ;
                if (q == nseen && nseen < 128) seen[nseen++] = o; else q = 0xFFFFFFFFu;
                if (q != 0xFFFFFFFFu && dlog < 100) dlog++; else goto logged_dl;
            }
            {
                rlog("dlight %p at %d,%d,%d radius %d color %d,%d,%d flags %08lx", (const void *)o, (int)p[0], (int)p[1], (int)p[2],
                     (int)rad, o[0x94], o[0x95], o[0x96], (unsigned long)RD32(o, OBJ_FLAGS));
            }
logged_dl:;
        }
    }
    buf[0] = (float)n;
    buf[1] = g_dl_scale;
    /* global (sky) light on vertex-lit level surfaces: RenderStruct +0x138 is
       the direction the light travels, +0x144 its colour (0-255). The stock
       renderer lights upward-facing vertex-lit ground with it (moonlight in
       Unwelcome Guests); lightmapped surfaces already have it baked. */
    if (enable && g_world_sun && g_rs && readable((const uint8_t *)g_rs + 0x138, 0x18)) {
        const float *d = (const float *)((const uint8_t *)g_rs + 0x138), *c = d + 3;
        float *sd = buf + 4 + 8 * MAX_DLIGHTS;
        if (c[0] > 0 || c[1] > 0 || c[2] > 0) {
            sd[0] = -d[0]; sd[1] = -d[1]; sd[2] = -d[2];
            sd[4] = c[0] / 255.0f; sd[5] = c[1] / 255.0f; sd[6] = c[2] / 255.0f; sd[7] = 1;
        }
    }
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cbdl, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
        memcpy(map.pData, buf, sizeof buf);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cbdl, 0);
    }
}

/* Animated light: when any baked light animation changes frame, blend or
 * intensity, relight the polygons that use one (lightmap rectangles and
 * vertex colours) with the same code that built them. */
static void update_light_anims(RenderContext *rc)
{
    const uint8_t *world = (const uint8_t *)rc->world, *anims;
    uint32_t na, ai, changed = 0, i;
    static uint32_t lmpix[LM_MAX_TEXELS];
    static int ulog;
    if ((!rc->ndlm && !rc->ndgv) || !readable(world, 0x10)) return;
    anims = RDPTR(world, 0x08);
    na = RD32(world, 0x0C);
    if (!na || na > 4096 || !readable(anims, (UINT_PTR)na * 0x60)) return;
    if (rc->nanim_state != na) {
        free(rc->anim_state);
        rc->anim_state = (uint32_t *)calloc(na * 4, 4);
        rc->nanim_state = rc->anim_state ? na : 0;
        changed = 1;
    }
    if (!rc->anim_state) return;
    for (ai = 0; ai < na; ai++) {
        const uint32_t *a = (const uint32_t *)(anims + ai * 0x60);
        uint32_t *st = rc->anim_state + ai * 4;
        if (st[0] != a[13] || st[1] != a[14] || st[2] != a[15] || st[3] != a[16]) {
            st[0] = a[13]; st[1] = a[14]; st[2] = a[15]; st[3] = a[16];
            changed = 1;
        }
    }
    if (!changed) return;
    for (i = 0; i < rc->ndlm; i++) {
        const struct DynLM *d = &rc->dlm[i];
        D3D11_BOX box;
        if (d->page < 0 || d->page >= rc->nlmpages || !rc->lmpages[d->page].tex) continue;
        if (!lm_gather(world, d->poly, (uint32_t)d->w, (uint32_t)d->h, lmpix)) memset(lmpix, 0, (size_t)d->w * d->h * 4);
        box.left = (UINT)d->x; box.top = (UINT)d->y; box.right = (UINT)(d->x + d->w); box.bottom = (UINT)(d->y + d->h);
        box.front = 0; box.back = 1;
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)rc->lmpages[d->page].tex, 0, &box, lmpix, (UINT)d->w * 4, 0);
    }
    if (rc->ndgv && rc->vcol && rc->cvb) {
        static uint32_t *cols;
        static uint32_t ncols;
        D3D11_MAPPED_SUBRESOURCE map;
        if (ncols < rc->ndgv_cols) { free(cols); cols = (uint32_t *)malloc(rc->ndgv_cols * 4); ncols = cols ? rc->ndgv_cols : 0; }
        if (cols) {
            for (i = 0; i < rc->ndgv; i++) {
                const struct DynGV *g = &rc->dgv[i];
                if (!lm_vertex_colors(world, g->poly, g->nv, cols + g->first)) memset(cols + g->first, 0, g->nv * 4);
            }
            for (i = 0; i < rc->ndvert; i++) rc->vcol[rc->dvert[i].vcol] = cols[rc->dvert[i].col];
            if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)rc->cvb, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) {
                memcpy(map.pData, rc->vcol, (size_t)rc->vcount * 4);
                ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)rc->cvb, 0);
            }
        }
    }
    if (ulog < 3) {
        rlog("light anims changed: relit %lu lightmaps, %lu vertex-lit polygons", (unsigned long)rc->ndlm, (unsigned long)rc->ndgv);
        ulog++;
    }
    {   /* every 2 s for a while: anim intensities and mean relit vertex brightness */
        static DWORD last;
        static int plog;
        DWORD now = GetTickCount();
        if (plog < 12 && now - last > 2000 && rc->ndvert && rc->vcol) {
            uint32_t sum = 0;
            last = now; plog++;
            for (i = 0; i < rc->ndvert; i++) {
                uint32_t c = rc->vcol[rc->dvert[i].vcol];
                sum += (c & 0xFF) + ((c >> 8) & 0xFF) + ((c >> 16) & 0xFF);
            }
            rlog("light anims: intensity x100 %d %d, mean relit vertex light %lu",
                 na > 1 ? (int)(*(const float *)(anims + 0x60 + 0x40) * 100) : -1,
                 na > 2 ? (int)(*(const float *)(anims + 0xC0 + 0x40) * 100) : -1,
                 (unsigned long)(sum / (rc->ndvert * 3)));
        }
    }
}

static void render_world(const uint8_t *sd)
{
    RenderContext *rc;
    const float *pos, *rot;
    float xfov, yfov, farz, nearz = 1.0f;
    float r[3], u[3], f[3];
    const int32_t *rect;
    D3D11_VIEWPORT vp;
    float bf[4] = { 0, 0, 0, 0 };
    static int logged;
    int hdr_on = g_hdr && g_hdr_rtv && g_hdr_srv && g_ps_tonemap && g_vs_fs && g_cb_tm;

    if (!readable(sd, 0x100)) return;
    if (RD32(sd, SD_DRAWMODE) == 1) {
        rc = (RenderContext *)RDPTR(sd, SD_CONTEXT);
        if (!rc || rc != g_cur_ctx) return;
        if (!rc->built) build_world(rc);
        if (!rc->vb) return;
    } else if (RD32(sd, SD_DRAWMODE) == 2) {
        rc = NULL;                     /* object list only (menus) */
    } else {
        return;
    }

    pos = (const float *)(sd + SD_POS);
    rot = (const float *)(sd + SD_ROT);
    rect = (const int32_t *)(sd + SD_RECT);
    xfov = *(const float *)(sd + SD_XFOV);
    yfov = *(const float *)(sd + SD_YFOV);
    farz = *(const float *)(sd + SD_FARZ);
    if (!(farz > 10.0f && farz < 1e7f)) farz = 10000.0f;
    if (!(xfov > 0.05f && xfov < 3.1f)) xfov = 1.5708f;
    g_cur_xfov = xfov;
    if (!(yfov > 0.05f && yfov < 3.1f)) yfov = 1.1781f;
    if (logged < 3) {
        rlog("scene: pos %d,%d,%d rot %d,%d,%d,%d (x1000) fov %d,%d (mrad) far %d rect %ld,%ld,%ld,%ld",
             (int)pos[0], (int)pos[1], (int)pos[2], (int)(rot[0] * 1000), (int)(rot[1] * 1000),
             (int)(rot[2] * 1000), (int)(rot[3] * 1000), (int)(xfov * 1000), (int)(yfov * 1000), (int)farz,
             (long)rect[0], (long)rect[1], (long)rect[2], (long)rect[3]);
        logged++;
    }

    flush_batch();
    {   /* live-tunable from the console */
        static DWORD last;
        DWORD now = GetTickCount();
        if (now - last > 500) {
            float sc = (float)cvar_int("avp2m_lmscale", 100) / 100.0f;
            g_lmscale = (sc > 0.05f && sc < 8.0f) ? sc : 1.0f;
            g_lmdebug = cvar_int("avp2m_lmdebug", 0);
            g_hdr = cvar_int("avp2m_hdr", 0);
            g_particle_scale = (float)cvar_int("avp2m_psize", 100) / 100.0f;
            g_dl_scale = (float)cvar_int("avp2m_dlscale", 100) / 100.0f;
            g_world_sun = cvar_int("avp2m_worldsun", 1);
            g_skin_normalize = cvar_int("avp2m_skinnorm", 1);
            if (!(g_particle_scale > 0.01f && g_particle_scale < 100.0f)) g_particle_scale = 1.0f;
            g_model_sat = (float)cvar_int("avp2m_modelsat", 200) / 100.0f;
            if (!(g_model_sat > 0.05f && g_model_sat < 8.0f)) g_model_sat = 2.0f;
            g_exposure = (float)cvar_int("avp2m_exposure", 100) / 100.0f;
            if (!(g_exposure > 0.05f && g_exposure < 16.0f)) g_exposure = 1.0f;
            last = now;
        }
    }
    write_camera(pos, rot, xfov, yfov, nearz, farz, hdr_on, r, u, f);

    vp.TopLeftX = (float)rect[0];
    vp.TopLeftY = (float)rect[1];
    vp.Width = (float)(rect[2] - rect[0]);
    vp.Height = (float)(rect[3] - rect[1]);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    if (vp.Width <= 0 || vp.Height <= 0) { vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)g_w; vp.Height = (float)g_h; }
    if (g_tw != g_w) {
        if (vp.TopLeftX <= 0 && vp.TopLeftY <= 0 && vp.Width >= (float)g_w && vp.Height >= (float)g_h) {
            /* full-screen scene: fill the whole wide target, Hor+ (same vertical FOV, wider horizontal) */
            xfov = 2.0f * (float)atan(tan(xfov * 0.5) * (double)g_tw / (double)g_w);
            vp.TopLeftX = 0;
            vp.Width = (float)g_tw;
            write_camera(pos, rot, xfov, yfov, nearz, farz, hdr_on, r, u, f);
            g_hud_frame = g_hud_edges;
        } else {
            vp.TopLeftX += (float)g_xoff;   /* sub-rect scenes stay inside the 4:3 area */
        }
    }

    if (hdr_on) {
        float black[4] = { 0, 0, 0, 1 };
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_hdr_rtv, g_dsv);
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_hdr_rtv, black);
    } else {
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rt_rtv, g_dsv);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_on, 0);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[OPT2D_SOLID], bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout3d);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (rc) {
        ID3D11Buffer *vbs[2] = { rc->vb, rc->cvb };
        UINT strides[2] = { sizeof(WVertex), 4 }, offs[2] = { 0, 0 };
        ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 2, vbs, strides, offs);
    }
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs3d, NULL, 0);
    {
        ID3D11Buffer *cbs[2] = { g_cb3d, g_cbw };
        ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 2, cbs);
    }
    set_world_rows(NULL);
    if (rc) update_world_models(rc);
    if (rc) update_light_anims(rc);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps3d, NULL, 0);
    {
        ID3D11Buffer *pcbs[3] = { g_cb3d, g_cbw, g_cbdl };
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 3, pcbs);
    }
    write_dlights(pos, 0);
    {
        ID3D11SamplerState *ss[2] = { g_samp_wrap, g_samp_linear };
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 2, ss);
    }
    if (rc) {
        UINT bi;
        const float *cur_xf = NULL;
        float skypos[3], sr[3], su[3], sf[3];
        int pass, nsky = mark_sky_bsps(rc, sd, pos, skypos);
        static int sky_logged;
        if (sky_logged < 2) {
            rlog("sky: %d sky BSPs, sky camera %d,%d,%d", nsky, (int)skypos[0], (int)skypos[1], (int)skypos[2]);
            sky_logged++;
        }
      for (pass = nsky ? 0 : 1; pass < 2; pass++) {
        if (pass == 0) {
            write_camera(skypos, rot, xfov, yfov, 1.0f, 200000.0f, hdr_on, sr, su, sf);
        } else if (nsky) {
            ID3D11DeviceContext_ClearDepthStencilView(g_ctx, g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
            write_camera(pos, rot, xfov, yfov, nearz, farz, hdr_on, r, u, f);
        }
        if (pass == 1) write_dlights(pos, 1);
        for (bi = 0; bi < rc->nbatches; bi++) {
            ID3D11ShaderResourceView *srv[2];
            int lp = rc->batches[bi].lmpage;
            int bb = rc->batches[bi].bsp;
            const float *xf = (bb >= 0 && (uint32_t)bb < rc->nbsps) ? rc->bsp_xf[bb] : NULL;
            int is_sky = (bb >= 0 && (uint32_t)bb < rc->nbsps) && rc->sky[bb];
            if (is_sky != (pass == 0)) continue;
            if (xf != cur_xf) { set_world_rows(xf); cur_xf = xf; }
            srv[0] = rc->batches[bi].tex ? rc->batches[bi].tex->srv : g_white_srv;
            srv[1] = (lp >= 0 && lp < rc->nlmpages && rc->lmpages[lp].srv) ? rc->lmpages[lp].srv : g_white_srv;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, srv);
            ID3D11DeviceContext_Draw(g_ctx, rc->batches[bi].count, rc->batches[bi].first);
        }
        cur_xf = NULL;
        set_world_rows(NULL);
      }
    }

    set_world_rows(NULL);
    write_dlights(pos, 0);   /* models are lit on the CPU */
    draw_models(pos, r, u, f);
    draw_sprites(pos, r, u, f);

    if (hdr_on) {   /* tone map the scene into the game target; the HUD is drawn on top later */
        ID3D11ShaderResourceView *none[2] = { NULL, NULL };
        D3D11_MAPPED_SUBRESOURCE tm;
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rt_rtv, NULL);
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cb_tm, 0, D3D11_MAP_WRITE_DISCARD, 0, &tm))) {
            float *t = (float *)tm.pData;
            t[0] = g_exposure; t[1] = (float)g_hdr; t[2] = t[3] = 0;
            ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb_tm, 0);
        }
        ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_off, 0);
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[OPT2D_SOLID], bf, 0xFFFFFFFFu);
        ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
        ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_VSSetShader(g_ctx, g_vs_fs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(g_ctx, g_ps_tonemap, NULL, 0);
        ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, &g_cb_tm);
        ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp_point);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &g_hdr_srv);
        ID3D11DeviceContext_Draw(g_ctx, 3, 0);
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, none);
    }

    /* back to the 2D pipeline */
    {
        ID3D11ShaderResourceView *none[2] = { NULL, NULL };
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 2, none);
    }
    g_batch_srv = NULL;
    begin_game_pass();
}

/* ------------------------------------------------------------------ */
/* Slot implementations                                                 */
/* ------------------------------------------------------------------ */
static int __cdecl r_init(RenderInit *init)
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr;

    init->version = AVP2M_RENDER_VERSION;
    g_hwnd = (HWND)init->main_window;
    g_w = init->mode.width ? init->mode.width : 640;
    g_h = init->mode.height ? init->mode.height : 480;
    g_windowed = cvar_int("windowed", 0) == 1;
    g_vsync = cvar_int("avp2m_vsync", 1) != 0;
    g_widescreen = cvar_int("avp2m_widescreen", 1) != 0;
    g_hud_edges = cvar_int("avp2m_hudedges", 1) != 0;
    g_lmscale = (float)cvar_int("avp2m_lmscale", 100) / 100.0f;
    if (!(g_lmscale > 0.05f && g_lmscale < 8.0f)) g_lmscale = 1.0f;
    g_lmdebug = cvar_int("avp2m_lmdebug", 0);
    rlog("Init %lux%lu windowed=%d vsync=%d hwnd=%p", (unsigned long)g_w, (unsigned long)g_h,
         g_windowed, g_vsync, (void *)g_hwnd);

    place_window();

    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                           levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) {
        rlog("hardware device failed (%08lX), trying WARP", (unsigned long)hr);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               levels, 3, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
    }
    if (FAILED(hr)) { rlog("ERROR: D3D11CreateDevice failed %08lX", (unsigned long)hr); return 1; }
    rlog("D3D11 device ready, feature level %lX", (unsigned long)got);

    if (!create_objects() || !create_swapchain() || !create_game_target()) {
        rlog("ERROR: device object creation failed");
        release_device();
        return 1;
    }

    g_rs->screen_width = g_w;
    g_rs->screen_height = g_h;
    g_in3d = g_in_opt2d = 0;
    g_vb_pos = 0;
    g_batch_verts = 0;
    g_batch_srv = NULL;
    g_batch_blend = -1;
    begin_game_pass();
    rlog("Init done: backbuffer %lux%lu", (unsigned long)g_bb_w, (unsigned long)g_bb_h);
    return 0;
}

static void __cdecl r_term(int full)
{
    rlog("Term(%d)", full);
    flush_batch();
    release_device();
}

static void __cdecl r_clear(LTRect *rect, uint32_t flags, const float *color)
{
    flush_hud();
    float c[4] = { 0, 0, 0, 1 };
    if (g_ctx && g_dsv && (flags & CLEAR_DEPTH))
        ID3D11DeviceContext_ClearDepthStencilView(g_ctx, g_dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    if (!g_ctx || !(flags & CLEAR_SCREEN)) return;
    if (color) { c[0] = color[0] / 255.0f; c[1] = color[1] / 255.0f; c[2] = color[2] / 255.0f; }
    if (!rect || (rect->left <= 0 && rect->top <= 0 &&
                  rect->right >= (int32_t)g_w && rect->bottom >= (int32_t)g_h)) {
        flush_batch();
        ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_rt_rtv, c);
    } else {
        uint32_t rgba = 0xFF000000u | ((uint32_t)(c[2] * 255) << 16) |
                        ((uint32_t)(c[1] * 255) << 8) | (uint32_t)(c[0] * 255);
        begin_game_pass();
        push_quad(g_white_srv, OPT2D_SOLID, (float)rect->left, (float)rect->top,
                  (float)rect->right, (float)rect->bottom, 0, 0, 1, 1, rgba);
    }
}

static int __cdecl r_start3d(void)        { g_in3d = 1; return 1; }
static int __cdecl r_end3d(void)          { flush_batch(); g_in3d = 0; return 1; }
static int __cdecl r_isin3d(void)         { return g_in3d; }
static int __cdecl r_start_opt2d(void)    { g_in_opt2d = 1; return 1; }
static void __cdecl r_end_opt2d(void)     { flush_batch(); g_in_opt2d = 0; }
static int __cdecl r_isin_opt2d(void)     { return g_in_opt2d; }
static int __cdecl r_set_opt2d_blend(int mode)
{
        if (mode < 0 || mode >= OPT2D_BLEND_COUNT) return 0;
    g_opt_blend = mode;
    return 1;
}
static int __cdecl r_get_opt2d_blend(int *mode)        { if (mode) *mode = g_opt_blend; return 1; }
static int __cdecl r_set_opt2d_color(uint32_t argb)    { g_opt_color = argb | 0xFF000000u; return 1; }
static int __cdecl r_get_opt2d_color(uint32_t *argb)   { if (argb) *argb = g_opt_color; return 1; }

/* Diagnostics: hex dump of a block of engine memory, 8 dwords per line. */
static void rlog_dump(const char *tag, const void *p, UINT_PTR n)
{
    UINT_PTR o;
    if (!readable(p, n)) { rlog("%s %p: unreadable", tag, p); return; }
    for (o = 0; o < n; o += 32) {
        const uint32_t *d = (const uint32_t *)((const uint8_t *)p + o);
        rlog("%s +%03lx: %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx", tag, (unsigned long)o,
             (unsigned long)d[0], (unsigned long)d[1], (unsigned long)d[2], (unsigned long)d[3],
             (unsigned long)d[4], (unsigned long)d[5], (unsigned long)d[6], (unsigned long)d[7]);
    }
}

/* ------------------------------------------------------------------ */
/* Objects: visibility query through the engine's world tree            */
/* ------------------------------------------------------------------ */
/* world+0x6C is the engine's world tree (vtable at its +0). vtable[5] (+0x14)
 * is a thiscall visibility query. It walks a quadtree: nodes that hold BSP
 * leaves hand their objects over through on_object_list (after the leaf's
 * own visibility test); other nodes report objects whose box overlaps
 * pos +/- max_dist through on_object. The engine uses 0x2C-0x47 as scratch. */
typedef struct VisQuery {
    uint32_t list_index;                  /* which per-node object list (0 = normal objects) */
    float pos[3];                         /* viewpoint */
    float max_dist;
    void (__cdecl *on_object)(uint8_t *obj, void *user);
    void (__cdecl *on_object_list)(void *list_head, uint8_t ***out, uint32_t *count);
    void *user;                           /* passed to on_object */
    void (__cdecl *on_leaf)(uint8_t *leaf);
    int (__cdecl *box_visible)(const uint8_t *box);     /* center +0x0C, half size +0x18 */
    int (__cdecl *node_visible)(const uint8_t *node);   /* center +0x30, radius +0x40 */
    uint32_t engine_scratch[8];           /* +0x2C: query box min/max, tree pointer */
} VisQuery;
typedef void (__fastcall *PFN_WorldTreeQuery)(void *self, void *edx, VisQuery *q);


static int obj_wanted(const uint8_t *obj)
{
    return obj && (RD32(obj, OBJ_FLAGS) & 1) && !(RD32(obj, OBJ_FLAGS2) & 0x100) && !(RD32(obj, 0x124) & 0x400);
}
static void __cdecl vis_on_object(uint8_t *obj, void *user)
{
    (void)user;
    g_nvis_seen++;
    if (!obj_wanted(obj) || g_nvis_objs >= MAX_VIS_OBJECTS) return;
    g_vis_objs[g_nvis_objs++] = obj;
}
static uint32_t g_nvis_listed;
/* Linked list of {next, prev, obj}; the list ends at the head or at an object
 * whose +0x190 is set. Objects are also appended to the engine's array, as the
 * stock renderer does. */
static void __cdecl vis_on_object_list(void *list_head, uint8_t ***out, uint32_t *count)
{
    uint8_t *it = (uint8_t *)RDPTR(list_head, 0);
    while (it && it != (uint8_t *)list_head) {
        uint8_t *obj = (uint8_t *)RDPTR(it, 8);
        if (!obj || RD32(obj, 0x190)) break;
        g_nvis_listed++;
        if (obj_wanted(obj)) {
            if (out && count && *out) (*out)[(*count)++] = obj;
            if (g_nvis_objs < MAX_VIS_OBJECTS) g_vis_objs[g_nvis_objs++] = obj;
        }
        it = (uint8_t *)RDPTR(it, 0);
    }
}
static void __cdecl vis_on_leaf(uint8_t *leaf) { (void)leaf; }
static int __cdecl vis_always(const uint8_t *p) { (void)p; return 1; }

static int cmp_ptr(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)*(uint8_t *const *)a, y = (uintptr_t)*(uint8_t *const *)b;
    return (x > y) - (x < y);
}

/* Model objects keep their Model pointer at +0x1F8 in AVP2 and at +0x1FC in
 * Primal Hunt, whose engine build inserts a field near +0x1D0 (the stock PH
 * d3d.ren reads +0x1FC and +0x1D4 where AVP2's reads +0x1F8 and +0x1D0).
 * Decided on the first model seen: the right slot points at a Model whose
 * first dword is a vtable inside the engine executable. */
static int in_exe_image(uintptr_t v)
{
    static uintptr_t lo, hi;
    if (!lo) {
        const uint8_t *b = (const uint8_t *)GetModuleHandleA(NULL);
        const uint8_t *nt = b + *(const uint32_t *)(b + 0x3C);
        lo = (uintptr_t)b;
        hi = lo + *(const uint32_t *)(nt + 0x50);   /* SizeOfImage */
    }
    return v >= lo && v < hi;
}
static const uint8_t *obj_model(const uint8_t *o)
{
    if (!g_model_off) {
        static const uint32_t cand[2] = { 0x1F8, 0x1FC };
        int k;
        for (k = 0; k < 2; k++) {
            const uint8_t *m = RDPTR(o, cand[k]);
            if (readable(m, 0x80) && in_exe_image(RD32(m, 0))) {
                g_model_off = cand[k];
                rlog("model objects: Model pointer at +0x%lX", (unsigned long)g_model_off);
                break;
            }
        }
        if (!g_model_off) return RDPTR(o, 0x1F8);
    }
    return RDPTR(o, g_model_off);
}

/* Diagnostics for the model format: the object's model (+0x1F8), its
 * animated node transforms (object vtable slot 18, thiscall, no arguments;
 * count at model+0x78) and candidate piece arrays. */
typedef const float *(__fastcall *PFN_GetNodeTransforms)(void *self, void *edx);
static void dump_model(uint8_t *o)
{
    const uint8_t *m = obj_model(o);
    uint32_t nn, k;
    rlog("model object %p, model %p, tracker +1d0:", (void *)o, (void *)m);
    rlog_dump("  trk", o + 0x1D0, 0x60);
    rlog_dump("  obj+1a0", o + 0x1A0, 0x60);
    {
        uint32_t q;
        for (q = 0x1A0; q < 0x200; q += 4) {
            const uint8_t *t = RDPTR(o, q);
            if ((uintptr_t)t > 0x10000 && readable(t, 0x20)) {
                char tag[24];
                sprintf(tag, "  obj+%03lx", (unsigned long)q);
                rlog_dump(tag, t, 0x20);
            }
        }
    }
    if (!readable(m, 0x180)) { rlog("  mdl %p: unreadable", (const void *)m); return; }
    rlog_dump("  mdl", m, 0x180);
    nn = RD32(m, 0x78);
    if (readable(RDPTR(o, 0), 0x4C) && nn && nn < 256) {
        const float *x = ((PFN_GetNodeTransforms)(void *)(uintptr_t)RD32(RDPTR(o, 0), 0x48))(o, NULL);
        rlog("  %lu nodes, transforms at %p", (unsigned long)nn, (const void *)x);
        for (k = 0; k < nn && k < 3 && readable(x, (k + 1) * 64); k++) {
            const float *q = x + k * 16;
            rlog("   node %lu: %d %d %d %d | %d %d %d %d | %d %d %d %d (x1000)", (unsigned long)k,
                 (int)(q[0] * 1000), (int)(q[1] * 1000), (int)(q[2] * 1000), (int)(q[3] * 1000),
                 (int)(q[4] * 1000), (int)(q[5] * 1000), (int)(q[6] * 1000), (int)(q[7] * 1000),
                 (int)(q[8] * 1000), (int)(q[9] * 1000), (int)(q[10] * 1000), (int)(q[11] * 1000));
        }
    }
    {   /* pieces: array at model+0x38 (count +0x3C); each piece has vertex and triangle arrays */
        const uint8_t *pa = RDPTR(m, 0x38);
        uint32_t np = RD32(m, 0x3C), pi;
        for (pi = 0; pi < np && pi < 2 && readable(pa, (pi + 1) * 4); pi++) {
            const uint8_t *pc = RDPTR(pa, pi * 4), *vv, *tt;
            rlog("  piece %lu at %p", (unsigned long)pi, (const void *)pc);
            rlog_dump("   pc", pc, 0x80);
            vv = RDPTR(pc, 0x04);
            tt = RDPTR(pc, 0x14);
            rlog_dump("   verts", vv, 0xC0);
            rlog_dump("   tris", tt, 0x80);
            if (readable(vv, 0x10)) rlog_dump("   v0 weights", RDPTR(vv, 0), 0x40);
        }
    }
    /* follow every pointer-looking dword of the model one level */
    for (k = 0; k < 0x180; k += 4) {
        const uint8_t *p = RDPTR(m, k);
        if ((uintptr_t)p > 0x10000 && readable(p, 0x40)) {
            char tag[24];
            sprintf(tag, "  mdl+%03lx", (unsigned long)k);
            rlog_dump(tag, p, 0x40);
        }
    }
}


/* Attachments (a marine's gun, a Predator's mask): object +0x98 is a list of
 * attachment records (next at +0x24). For each, RenderStruct +0x00 is the
 * engine's ProcessAttachment(parent, attachment), which places the attached
 * object and returns it; the stock renderer then draws it like any other
 * object (d3d.ren 0x10039900). Attached objects can have attachments too. */
typedef uint8_t *(__cdecl *PFN_ProcessAttachment)(const uint8_t *parent, const uint8_t *attachment);
static void add_attachments(void)
{
    uint32_t i, n0 = g_nvis_objs, added = 0;
    static int alog;
    PFN_ProcessAttachment pa;
    if (!g_rs || !g_rs->engine_0x00) return;
    pa = (PFN_ProcessAttachment)(uintptr_t)g_rs->engine_0x00;
    for (i = 0; i < g_nvis_objs && i < MAX_VIS_OBJECTS; i++) {   /* grows as children are added */
        const uint8_t *o = g_vis_objs[i], *at;
        uint32_t guard = 0;
        if (!readable(o, 0x9C)) continue;
        {
            static int clog;
            if (o[OBJ_TYPE] == 1 && (RD32(o, OBJ_FLAGS) & 0x40000) && clog < 4) {
                clog++;
                rlog("attach: character %p +98 = %08lx", (const void *)o, (unsigned long)RD32(o, 0x98));
            }
        }
        for (at = RDPTR(o, 0x98); at && guard < 32 && readable(at, 0x28); at = RDPTR(at, 0x24), guard++) {
            uint8_t *child = pa(o, at);
            uint32_t j;
            {
                static int dlog;
                if (dlog < 4) {
                    dlog++;
                    rlog("attach: parent %p type %d att %p -> child %p type %d flags %08lx flags2 %08lx +124 %08lx",
                         (const void *)o, o[OBJ_TYPE], (const void *)at, (void *)child,
                         child && readable(child, 0x1C0) ? child[OBJ_TYPE] : -1,
                         child && readable(child, 0x1C0) ? (unsigned long)RD32(child, OBJ_FLAGS) : 0ul,
                         child && readable(child, 0x1C0) ? (unsigned long)RD32(child, OBJ_FLAGS2) : 0ul,
                         child && readable(child, 0x1C0) ? (unsigned long)RD32(child, 0x124) : 0ul);
                    rlog_dump("  att", at, 0x28);
                }
            }
            /* +0x124 & 0x400 marks objects drawn through their parent; the stock
               renderer checks it on top-level objects only */
            if (!child || !readable(child, 0x1C0) || !(RD32(child, OBJ_FLAGS) & 1) || (RD32(child, OBJ_FLAGS2) & 0x100)) continue;
            for (j = 0; j < g_nvis_objs && g_vis_objs[j] != child; j++) ;
            if (j < g_nvis_objs || g_nvis_objs >= MAX_VIS_OBJECTS) continue;
            g_vis_objs[g_nvis_objs++] = child;
            added++;
        }
    }
    if (added && alog < 3) {
        rlog("attachments: %lu objects added to %lu", (unsigned long)added, (unsigned long)n0);
        alog++;
    }
}
static void collect_objects(const uint8_t *world, const float *eye)
{
    VisQuery q;
    uint8_t *tree;
    static int logged, calls;
    g_nvis_objs = g_nvis_seen = g_nvis_listed = 0;
    calls++;
    if (!readable(world, 0x70)) return;
    tree = (uint8_t *)world + 0x6C;
    if (!readable(RDPTR(tree, 0), 0x18)) return;
    memset(&q, 0, sizeof q);
    q.pos[0] = eye[0]; q.pos[1] = eye[1]; q.pos[2] = eye[2];
    q.max_dist = 1000000.0f;
    q.on_object = vis_on_object;
    q.on_object_list = vis_on_object_list;
    q.on_leaf = vis_on_leaf;
    q.box_visible = vis_always;
    q.node_visible = vis_always;
    ((PFN_WorldTreeQuery)(void *)(uintptr_t)RD32(RDPTR(tree, 0), 0x14))(tree, NULL, &q);
    {   /* an object can be reported by more than one node or leaf */
        uint32_t i, n = 0;
        qsort(g_vis_objs, g_nvis_objs, sizeof g_vis_objs[0], cmp_ptr);
        for (i = 0; i < g_nvis_objs; i++)
            if (!n || g_vis_objs[n - 1] != g_vis_objs[i]) g_vis_objs[n++] = g_vis_objs[i];
        g_nvis_objs = n;
    }
    if ((calls == 1 || calls == 400 || calls == 1500) && logged < 3) {
        uint32_t i, bytype[16] = { 0 };
        logged++;
        rlog("objects (frame %d): engine reported %lu directly, %lu through leaf lists", calls,
             (unsigned long)g_nvis_seen, (unsigned long)g_nvis_listed);
        rlog("  world tree %p vtable %p query fn %08lx", (void *)tree, (void *)RDPTR(tree, 0),
             (unsigned long)RD32(RDPTR(tree, 0), 0x14));
        for (i = 0; i < g_nvis_objs; i++) bytype[g_vis_objs[i][OBJ_TYPE] & 15]++;
        rlog("objects: %lu visible (model %lu, worldmodel %lu, sprite %lu, light %lu, particles %lu, polygrid %lu, lines %lu, container %lu, canvas %lu, normal %lu)",
             (unsigned long)g_nvis_objs, (unsigned long)bytype[1], (unsigned long)bytype[2], (unsigned long)bytype[3],
             (unsigned long)bytype[4], (unsigned long)bytype[6], (unsigned long)bytype[7], (unsigned long)bytype[8],
             (unsigned long)bytype[9], (unsigned long)bytype[10], (unsigned long)bytype[0]);
        for (i = 0; i < g_nvis_objs && i < 40; i++) {
            const float *pp = (const float *)(g_vis_objs[i] + OBJ_POS);
            rlog("  obj %p type %d flags %08lx pos %d,%d,%d", (void *)g_vis_objs[i], g_vis_objs[i][OBJ_TYPE],
                 (unsigned long)RD32(g_vis_objs[i], OBJ_FLAGS), (int)pp[0], (int)pp[1], (int)pp[2]);
        }
        if (logged == 2) {
            for (i = 0; i < g_nvis_objs; i++) {
                if (g_vis_objs[i][OBJ_TYPE] == 1 && !(RD32(g_vis_objs[i], OBJ_FLAGS) & 0x40)) {
                    dump_model(g_vis_objs[i]);
                    break;
                }
            }
            for (i = 0; i < g_nvis_objs; i++) {
                if (g_vis_objs[i][OBJ_TYPE] == 3) { rlog_dump("  sprite", g_vis_objs[i], 0x200); break; }
            }
            for (i = 0; i < g_nvis_objs; i++) {
                if (g_vis_objs[i][OBJ_TYPE] == 4) { rlog_dump("  light", g_vis_objs[i], 0x200); break; }
            }
            for (i = 0; i < g_nvis_objs; i++) {
                if (g_vis_objs[i][OBJ_TYPE] == 6) {
                    uint32_t q;
                    rlog_dump("  psys", g_vis_objs[i], 0x280);
                    for (q = 0x1A0; q < 0x280; q += 4) {
                        const uint8_t *t = RDPTR(g_vis_objs[i], q);
                        if ((uintptr_t)t > 0x10000 && readable(t, 0x40)) {
                            char tag[24];
                            sprintf(tag, "  psys+%03lx", (unsigned long)q);
                            rlog_dump(tag, t, 0x40);
                        }
                    }
                    break;
                }
            }
        }
    }
}

static void dump_scene(const uint8_t *sd)
{
    static int dumped[3];
    uint32_t mode;
    if (!readable(sd, 0x100)) return;
    mode = RD32(sd, 0);
    if (mode > 2 || dumped[mode] >= 1) return;
    dumped[mode]++;
    rlog("SceneDesc dump (mode %lu):", (unsigned long)mode);
    rlog_dump("  sd", sd, 0x120);
    {
        int k;
        for (k = 1; k <= 6; k++) {
            char tag[16];
            sprintf(tag, "  sd[%d]", k);
            rlog("  sd+%02x -> %p", k * 4, (void *)RDPTR(sd, k * 4));
            rlog_dump(tag, RDPTR(sd, k * 4), 0x40);
        }
        if (RD32(sd, 0x2C)) rlog_dump("  sd2c", RDPTR(sd, 0x2C), 0x80);
    }
    {
        const uint8_t *list = RDPTR(sd, mode == 1 ? 0xB4 : 0xF4);
        uint32_t cnt = RD32(sd, mode == 1 ? 0xB8 : 0xF8), i;
        rlog("  object list %p count %lu", (void *)list, (unsigned long)cnt);
        for (i = 0; i < cnt && i < 12 && readable(list, (i + 1) * 4); i++) {
            char tag[16];
            sprintf(tag, "  obj%lu", (unsigned long)i);
            rlog("  object %lu at %p", (unsigned long)i, (void *)RDPTR(list, i * 4));
            rlog_dump(tag, RDPTR(list, i * 4), 0x280);
        }
    }
}

static int __cdecl r_render_scene(void *scene)
{
    const uint8_t *sd = (const uint8_t *)scene;
    flush_hud();
    g_cur_sd = sd;
    dump_scene(sd);
    g_nvis_objs = 0;
    if (g_ctx && readable(sd, 0x100) && RD32(sd, 0) == 1 && g_cur_ctx && RDPTR(sd, 0x34) == (const uint8_t *)g_cur_ctx) {
        collect_objects((const uint8_t *)g_cur_ctx->world, (const float *)(sd + 0xD8));
    } else if (readable(sd, 0x100) && RD32(sd, 0) == 2) {
        const uint8_t *list = RDPTR(sd, 0xF4);
        uint32_t cnt = RD32(sd, 0xF8), i;
        if (cnt <= MAX_VIS_OBJECTS && readable(list, cnt * 4))
            for (i = 0; i < cnt; i++) {
                uint8_t *o = (uint8_t *)RDPTR(list, i * 4);
                if (readable(o, 0x1C0) && obj_wanted(o)) g_vis_objs[g_nvis_objs++] = o;
            }
    }
    add_attachments();
    if (g_ctx) render_world(sd);
    return 1;
}
static void __cdecl r_render_command(int argc, char **argv) { (void)argc; (void)argv; }

static void *__cdecl r_get_hook(const char *name)
{
    static int logged;
    if (logged < 4) { rlog("GetHook(%s) -> NULL (video playback not supported yet)", name ? name : "?"); logged++; }
    return NULL;
}

/* The engine resizes its own window after Init (for its old exclusive
 * fullscreen). In fullscreen, keep the window borderless and covering the
 * monitor; checked once per frame, only acts when something changed it. */
static void enforce_fullscreen_window(void)
{
    RECT wr;
    HMONITOR mon;
    MONITORINFO mi;
    LONG style;
    if (g_windowed || !g_hwnd || IsIconic(g_hwnd)) return;
    mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(mon, &mi)) return;
    GetWindowRect(g_hwnd, &wr);
    style = GetWindowLongA(g_hwnd, GWL_STYLE);
    if (!EqualRect(&wr, &mi.rcMonitor) || (style & (WS_CAPTION | WS_THICKFRAME))) {
        static int logged;
        if (logged++ < 3)
            rlog("fullscreen: window was %ld,%ld-%ld,%ld style %08lX, restoring borderless fullscreen",
                 (long)wr.left, (long)wr.top, (long)wr.right, (long)wr.bottom, (unsigned long)style);
        place_window();
    }
}


/* Write a B8G8R8A8 texture (the game target) to a 24-bit BMP. Returns 1 on success. */
static int save_texture_bmp(ID3D11Texture2D *src, const char *path)
{
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *st = NULL;
    D3D11_MAPPED_SUBRESOURCE m;
    FILE *f;
    uint32_t y, x, row, sz;
    uint8_t hdr[54], *line;
    int ok = 0;
    if (!src || !g_dev || !g_ctx) return 0;
    ID3D11Texture2D_GetDesc(src, &td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &st))) return 0;
    flush_batch();
    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
        row = (td.Width * 3 + 3) & ~3u;
        sz = row * td.Height;
        line = (uint8_t *)calloc(1, row);
        f = fopen(path, "wb");
        if (f && line) {
            memset(hdr, 0, sizeof hdr);
            hdr[0] = 'B'; hdr[1] = 'M';
            *(uint32_t *)(hdr + 2) = 54 + sz; *(uint32_t *)(hdr + 10) = 54; *(uint32_t *)(hdr + 14) = 40;
            *(int32_t *)(hdr + 18) = (int32_t)td.Width; *(int32_t *)(hdr + 22) = (int32_t)td.Height;
            *(uint16_t *)(hdr + 26) = 1; *(uint16_t *)(hdr + 28) = 24; *(uint32_t *)(hdr + 34) = sz;
            fwrite(hdr, 1, 54, f);
            for (y = td.Height; y-- > 0;) {   /* bottom-up */
                const uint8_t *px = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
                for (x = 0; x < td.Width; x++) { line[x * 3] = px[x * 4]; line[x * 3 + 1] = px[x * 4 + 1]; line[x * 3 + 2] = px[x * 4 + 2]; }
                fwrite(line, 1, row, f);
            }
            ok = 1;
        }
        if (f) fclose(f);
        free(line);
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)st, 0);
    }
    ID3D11Texture2D_Release(st);
    return ok;
}

/* Frame limiter (cvar avp2m_maxfps, 0 = off; read live). The stock engine runs uncapped and vsync
 * only caps at the refresh rate, which on 144-240 Hz monitors is still fast enough to upset the game's
 * movement code. Sleeps on a high-resolution waitable timer, then spins the last half millisecond. */
static void limit_frame_rate(void)
{
    static LARGE_INTEGER freq, next;
    static HANDLE timer;
    static int maxfps;
    static DWORD lastread;
    LARGE_INTEGER now;
    DWORD tick = GetTickCount();
    static DWORD fps_t0;
    static unsigned fps_n;
    fps_n++;
    if (tick - fps_t0 >= 10000) {   /* average frame rate, every 10 s */
        if (fps_t0) rlog("fps: %u (avp2m_maxfps %d, vsync %d)", fps_n * 1000u / (tick - fps_t0), maxfps, g_vsync);
        fps_t0 = tick; fps_n = 0;
    }
    if (tick - lastread > 500) {
        maxfps = cvar_int("avp2m_maxfps", 0);
        if (maxfps < 0 || maxfps > 1000) maxfps = 0;
        lastread = tick;
    }
    if (!maxfps) { next.QuadPart = 0; return; }
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    }
    QueryPerformanceCounter(&now);
    if (next.QuadPart && now.QuadPart < next.QuadPart) {
        LONGLONG left = next.QuadPart - now.QuadPart;
        LONGLONG ticks100ns = left * 10000000 / freq.QuadPart - 5000;   /* wake 0.5 ms early */
        if (timer && ticks100ns > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -ticks100ns;
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, 100);
        }
        do { QueryPerformanceCounter(&now); } while (now.QuadPart < next.QuadPart);
        next.QuadPart += freq.QuadPart / maxfps;
        if (now.QuadPart - next.QuadPart > freq.QuadPart / 10) next.QuadPart = now.QuadPart;   /* fell far behind: resync */
    } else {
        next.QuadPart = now.QuadPart + freq.QuadPart / maxfps;
    }
}

static void present_frame(void)
{
    uint32_t cw, ch;
    float sx, sy, s, dw, dh, x0, y0;
    float black[4] = { 0, 0, 0, 1 };
    ID3D11ShaderResourceView *none = NULL;

    flush_batch();
    enforce_fullscreen_window();
    client_size(&cw, &ch);
    if (cw != g_bb_w || ch != g_bb_h) {
        SAFE_RELEASE(g_bb_rtv, ID3D11RenderTargetView);
        ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
        IDXGISwapChain_ResizeBuffers(g_swap, 0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        create_backbuffer_view();
    }

    flush_hud();
    g_hud_frame = 0;
    /* Scale the game image to the window, preserving aspect ratio. */
    sx = (float)g_bb_w / (float)g_tw;
    sy = (float)g_bb_h / (float)g_h;
    s = sx < sy ? sx : sy;
    dw = g_tw * s;
    dh = g_h * s;
    x0 = ((float)g_bb_w - dw) * 0.5f;
    g_disp_x0 = x0 + g_xoff * s;   /* where game pixel 0 lands in the window (for cursor mapping) */
    g_disp_s = s;
    y0 = ((float)g_bb_h - dh) * 0.5f;
    g_disp_y0 = y0;

    bind_pipeline(g_bb_rtv, (float)g_bb_w, (float)g_bb_h,
                  (s == 1.0f) ? g_samp_point : g_samp_linear);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_bb_rtv, black);
    push_quad(g_rt_srv, OPT2D_SOLID, x0, y0, x0 + dw, y0 + dh, 0, 0, 1, 1, 0xFFFFFFFFu);
    flush_batch();
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &none);

    {   /* avp2m_shotevery N: save the game image every N seconds (unattended test runs), up to 20 */
        static DWORD t0, lastread;
        static int every, n;
        DWORD now = GetTickCount();
        if (now - lastread > 1000) { every = cvar_int("avp2m_shotevery", 0); lastread = now; if (!t0) t0 = now; }
        if (every > 0 && n < 20 && now - t0 >= (DWORD)every * 1000u) {
            char path[64];
            CreateDirectoryA("avp2m_shots", NULL);
            sprintf(path, "avp2m_shots\\shot_%02d.bmp", n);
            if (save_texture_bmp(g_rt_tex, path)) rlog("screenshot %s (%lux%lu)", path, (unsigned long)g_tw, (unsigned long)g_h);
            n++;
            t0 = now;
        }
    }
    limit_frame_rate();
    IDXGISwapChain_Present(g_swap, g_vsync ? 1 : 0, 0);
    g_batch_srv = NULL;
    begin_game_pass();
}

uint32_t g_frame_no;

/* Cursor calls from the game assume the window is the game's display mode. When the image is scaled
 * (borderless fullscreen), map game-pixel rectangles and points to where the image is shown. */
static void cursor_log(const char *line) { rlog("%s", line); }
static void cursor_map(void *user, int kind, long *d)
{
    POINT o = { 0, 0 };
    float s = g_disp_s;
    (void)user;
    if (!g_hwnd || !(s > 0) || (s == 1.0f && g_disp_x0 == 0 && g_disp_y0 == 0)) return;
    ClientToScreen(g_hwnd, &o);
    if (kind == CURSOR_MAP_RECT) {
        long w = d[2] - d[0], h = d[3] - d[1];
        /* only a game-mode-sized rectangle at the window origin (what the engine passes) */
        if (d[0] != o.x || d[1] != o.y || w != (long)g_w || h != (long)g_h) return;
        d[0] = o.x + (long)g_disp_x0;
        d[1] = o.y + (long)g_disp_y0;
        d[2] = o.x + (long)(g_disp_x0 + g_w * s);
        d[3] = o.y + (long)(g_disp_y0 + g_h * s);
    } else {
        long gx = d[0] - o.x, gy = d[1] - o.y;
        if (gx < 0 || gy < 0 || gx > (long)g_w || gy > (long)g_h) return;
        d[0] = o.x + (long)(g_disp_x0 + gx * s);
        d[1] = o.y + (long)(g_disp_y0 + gy * s);
    }
}

static void __cdecl r_swap_buffers(uint32_t flags)
{
    (void)flags;
    g_frame_no++;
    {   /* fixes for original game-module bugs (src/compat/game_fixes.c); cvar avp2m_gamefixes 0 disables */
        const char *fname; void *fwhere;
        static int checked;
        if (!checked) {
            checked = 1;
            if (cvar_int("avp2m_gamefixes", 1)) avp2m_start_game_fix_watcher();
            else rlog("game fixes disabled (avp2m_gamefixes 0)");
        }
        if (avp2m_game_fix_news(&fname, &fwhere)) rlog("game fix applied at %p: %s", fwhere, fname);
    }
    {   /* cursor call hooks (src/compat/cursor_hooks.c); cshell.dll loads late, so look again every second */
        static DWORD last;
        static int on = -1;
        DWORD now = GetTickCount();
        if (on < 0) {
            on = cvar_int("avp2m_cursorhooks", 1) != 0;
            if (on) avp2m_cursor_hooks_init(cursor_log, cursor_map, NULL);
        }
        if (on && now - last > 1000) { avp2m_hook_cursor_apis(); last = now; }
    }
    {   /* automated test runs: avp2m_quitafter N closes the game N seconds after the first frame */
        static DWORD t0;
        static int secs = -1, closing;
        DWORD now = GetTickCount();
        if (secs < 0) { secs = cvar_int("avp2m_quitafter", 0); t0 = now; }
        if (secs > 0 && !closing && now - t0 > (DWORD)secs * 1000u) {
            rlog("avp2m_quitafter %d: closing the game", secs);
            closing = 1;
            PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
        } else if (closing && now - t0 > (DWORD)secs * 1000u + 8000u) {
            rlog("avp2m_quitafter: still running, exiting");
            ExitProcess(0);
        }
    }
    if (g_ctx && g_swap) present_frame();
}

static int __cdecl r_get_info_flags(void) { return 0; }

static void __cdecl r_get_screen_format(void *pformat)
{
    typedef void (__attribute__((thiscall)) *PFN_PFInit)(void *self, int type,
                                                         uint32_t a, uint32_t r, uint32_t g, uint32_t b);
    void **vtbl;
    if (!pformat) return;
    vtbl = *(void ***)pformat;
    ((PFN_PFInit)vtbl[0])(pformat, PFORMAT_BPP_32, 0, 0x00FF0000u, 0x0000FF00u, 0x000000FFu);
}

static void *__cdecl r_create_surface(int w, int h)
{
    Surface *s;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return NULL;
    s = (Surface *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = (uint32_t)w;
    s->h = (uint32_t)h;
    s->pitch = (uint32_t)w * 4;
    s->pixels = (uint8_t *)calloc((size_t)s->pitch, (size_t)h);
    if (!s->pixels) { free(s); return NULL; }
    s->dirty = 1;
    s->opt_key = COLORKEY_NONE;
    if (!g_surfaces.next) { g_surfaces.next = g_surfaces.prev = &g_surfaces; }
    s->next = g_surfaces.next;
    s->prev = &g_surfaces;
    g_surfaces.next->prev = s;
    g_surfaces.next = s;
    return s;
}

static void __cdecl r_delete_surface(Surface *s)
{
    if (!s) return;
    flush_hud();
    if (g_batch_srv && g_batch_srv == s->srv) {
        flush_batch();
        g_batch_srv = NULL;
    }
    s->prev->next = s->next;
    s->next->prev = s->prev;
    SAFE_RELEASE(s->srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(s->tex, ID3D11Texture2D);
    free(s->pixels);
    free(s);
}

static void __cdecl r_get_surface_info(Surface *s, uint32_t *w, uint32_t *h, int32_t *pitch)
{
    if (!s) return;
    if (w) *w = s->w;
    if (h) *h = s->h;
    if (pitch) *pitch = (int32_t)s->pitch;
}

static void *__cdecl r_lock_surface(Surface *s)        { return s ? s->pixels : NULL; }
static void __cdecl r_unlock_surface(Surface *s)       { if (s) s->dirty = 1; }

static int __cdecl r_optimize_surface(Surface *s, uint32_t key)
{
    if (!s) return 0;
    s->opt_key = key;
    return 1;
}
static void __cdecl r_unoptimize_surface(Surface *s)   { (void)s; }

static void __cdecl r_blit_to_screen(BlitRequest *req)
{
    Surface *s;
    uint32_t key, rgba;
    int blend, optimized;
    const LTRect *sr, *dr;
    float u0, v0, u1, v1;

    if (!g_ctx || !req || !(s = (Surface *)req->surface) || !req->src || !req->dst) return;
    optimized = g_in3d && g_in_opt2d;

    if (optimized) {
        /* Optimized path: the request's key always applies (-1 = opaque);
         * modulated by the optimized-2D color and the request alpha. */
        float a = req->alpha;
        uint32_t ab;
        key = req->color_key;
        blend = g_opt_blend;
        if (a < 0) a = 0;
        if (a > 1) a = 1;
        ab = (uint32_t)(a * 255.0f + 0.5f);
        /* g_opt_color is A8R8G8B8; vertex color is R8G8B8A8 (bytes R,G,B,A) */
        rgba = ((g_opt_color >> 16) & 0xFFu) | (g_opt_color & 0xFF00u) |
               ((g_opt_color & 0xFFu) << 16) | (ab << 24);
        if (blend == OPT2D_SOLID && key != COLORKEY_NONE) blend = OPT2D_ALPHA;
    } else {
        key = (req->flags & BLIT_FLAG_COLORKEY) ? req->color_key : COLORKEY_NONE;
        blend = (key != COLORKEY_NONE) ? OPT2D_ALPHA : OPT2D_SOLID;
        rgba = 0xFFFFFFFFu;
    }

    if (s->dirty || !s->tex || s->tex_key != key) {
        flush_batch();
        if (!upload_surface(s, key)) return;
    }

    sr = req->src;
    dr = req->dst;
    u0 = (float)sr->left / (float)s->w;
    v0 = (float)sr->top / (float)s->h;
    u1 = (float)sr->right / (float)s->w;
    v1 = (float)sr->bottom / (float)s->h;
    if (g_hud_frame && g_nhud < MAX_HUD_QUADS) {
        HudQuad *q = &g_hud[g_nhud++];
        q->srv = s->srv; q->blend = blend;
        q->x0 = (float)dr->left; q->y0 = (float)dr->top; q->x1 = (float)dr->right; q->y1 = (float)dr->bottom;
        q->u0 = u0; q->v0 = v0; q->u1 = u1; q->v1 = v1; q->rgba = rgba;
        return;
    }
    push_quad(s->srv, blend, (float)dr->left, (float)dr->top, (float)dr->right, (float)dr->bottom,
              u0, v0, u1, v1, rgba);
}

/* Not used in normal play yet: safe no-ops. */
static int  __cdecl r_bind_texture(void *tex, int changed)
{
    RTexture *rt;
    if (!g_dev || !tex) return 0;
    rt = get_texture(tex);
    if (rt && changed) {            /* texture contents changed: rebuild */
        release_texture(rt);
        rt = create_texture(tex);
    }
    return rt != NULL;
}
static void __cdecl r_unbind_texture(void *tex)
{
    RTexture *rt;
    if (!readable(tex, 0x10)) return;
    rt = (RTexture *)(uintptr_t)RD32(tex, 0x0C);
    for (RTexture *t = g_textures; t; t = t->next)
        if (t == rt) { release_texture(rt); return; }
}
static int  __cdecl r_load_world_data(void *p)               { (void)p; return 1; }
static void *__cdecl r_create_context(void *init)
{
    RenderContext *rc = (RenderContext *)calloc(1, sizeof *rc);
    if (!rc) return NULL;
    rc->world = readable(init, 4) ? (void *)(uintptr_t)RD32(init, 0) : NULL;
    g_cur_ctx = rc;
    rlog("CreateContext: world %p", rc->world);
    return rc;
}
static void __cdecl r_delete_context(void *ctx)
{
    RenderContext *rc = (RenderContext *)ctx;
    if (!rc) return;
    if (rc == g_cur_ctx) g_cur_ctx = NULL;
    if (rc->vb) ID3D11Buffer_Release(rc->vb);
    lm_release(rc);
    if (rc->cvb) ID3D11Buffer_Release(rc->cvb);
    free(rc->vcol);
    free(rc->dlm); free(rc->dgv); free(rc->dvert); free(rc->anim_state);
    free(rc->bsps);
    free(rc->bsp_xf);
    free(rc->sky);
    free(rc->batches);
    free(rc);
}
/* Screen lock (the console and other software drawing): the stock renderer
 * locks a rectangle of the back buffer and the engine reads and writes its
 * pixels directly (32-bit 0x00RRGGBB, the screen format we report).
 * LockScreen(left, top, right, bottom, void **pixels, long *pitch) returns 1
 * on success with pixels pointing at the rectangle's top-left. We copy the
 * rectangle of the game target into CPU memory and upload it on unlock. */
static D3D11_BOX g_scr_box;
static int g_scr_locked;
static int __cdecl r_lock_screen(int l, int t, int r, int b, void **pixels, long *pitch)
{
    D3D11_MAPPED_SUBRESOURCE map;
    uint32_t y;
    static int slog;
    if (g_scr_locked || !g_dev || !g_rt_tex) return 0;
    flush_hud();
    if (l < 0) l = 0;
    if (t < 0) t = 0;
    if (r > (int)g_w) r = (int)g_w;
    if (b > (int)g_h) b = (int)g_h;
    if (r <= l || b <= t) return 0;
    if (!g_scr_staging || g_scr_w != g_w || g_scr_h != g_h) {
        D3D11_TEXTURE2D_DESC td;
        if (g_scr_staging) { ID3D11Texture2D_Release(g_scr_staging); g_scr_staging = NULL; }
        free(g_scr_buf);
        g_scr_buf = (uint8_t *)malloc((size_t)g_w * g_h * 4);
        memset(&td, 0, sizeof td);
        td.Width = g_w; td.Height = g_h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (!g_scr_buf || FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_scr_staging))) {
            free(g_scr_buf); g_scr_buf = NULL; g_scr_staging = NULL;
            return 0;
        }
        g_scr_w = g_w; g_scr_h = g_h;
    }
    flush_batch();
    g_scr_box.left = (UINT)l; g_scr_box.top = (UINT)t; g_scr_box.right = (UINT)r; g_scr_box.bottom = (UINT)b;
    g_scr_box.front = 0; g_scr_box.back = 1;
    {   /* the game's 4:3 area sits g_xoff pixels into the (possibly wider) game target */
        D3D11_BOX src = g_scr_box;
        src.left += g_xoff; src.right += g_xoff;
        ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_scr_staging, 0, (UINT)l, (UINT)t, 0,
                                                  (ID3D11Resource *)g_rt_tex, 0, &src);
    }
    if (FAILED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_scr_staging, 0, D3D11_MAP_READ, 0, &map))) return 0;
    for (y = (UINT)t; y < (UINT)b; y++)
        memcpy(g_scr_buf + ((size_t)y * g_w + (UINT)l) * 4, (const uint8_t *)map.pData + (size_t)y * map.RowPitch + (size_t)l * 4,
               (size_t)(r - l) * 4);
    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_scr_staging, 0);
    if (pixels) *pixels = g_scr_buf + ((size_t)t * g_w + (UINT)l) * 4;
    if (pitch) *pitch = (long)(g_w * 4);
    g_scr_locked = 1;
    if (slog < 3) { rlog("LockScreen(%d,%d,%d,%d)", l, t, r, b); slog++; }
    return 1;
}
static void __cdecl r_unlock_screen(void)
{
    if (!g_scr_locked) return;
    g_scr_locked = 0;
    {
        D3D11_BOX dst = g_scr_box;
        dst.left += g_xoff; dst.right += g_xoff;
        ID3D11DeviceContext_UpdateSubresource(g_ctx, (ID3D11Resource *)g_rt_tex, 0, &dst,
                                          g_scr_buf + ((size_t)g_scr_box.top * g_w + g_scr_box.left) * 4, g_w * 4, 0);
    }
}
static int  __cdecl r_warp_to_screen(void *req)              { (void)req; return 0; }
static void __cdecl r_make_screenshot(const char *file)
{
    int ok = file && save_texture_bmp(g_rt_tex, file);
    rlog("MakeScreenShot(%s): %s", file ? file : "?", ok ? "written" : "failed");
}
static void __cdecl r_read_console_variables(void)           { }
static void __cdecl r_blit_from_screen(void *req)            { (void)req; }

/* ------------------------------------------------------------------ */
/* Exports                                                              */
/* ------------------------------------------------------------------ */
#define SLOT(fn) ((AVP2M_Fn)(void *)(fn))

__declspec(dllexport) void __cdecl RenderDLLSetup(RenderStruct *rs)
{
    static const AVP2M_Fn slots[RS_SLOT_COUNT] = {
        SLOT(r_init), SLOT(r_term), SLOT(r_bind_texture), SLOT(r_unbind_texture),
        SLOT(r_load_world_data), SLOT(r_create_context), SLOT(r_delete_context),
        SLOT(r_clear), SLOT(r_start3d), SLOT(r_end3d), SLOT(r_isin3d),
        SLOT(r_start_opt2d), SLOT(r_end_opt2d), SLOT(r_isin_opt2d),
        SLOT(r_set_opt2d_blend), SLOT(r_get_opt2d_blend), SLOT(r_set_opt2d_color),
        SLOT(r_get_opt2d_color), SLOT(r_render_scene), SLOT(r_render_command),
        SLOT(r_get_hook), SLOT(r_swap_buffers), SLOT(r_get_info_flags),
        SLOT(r_get_screen_format), SLOT(r_create_surface), SLOT(r_delete_surface),
        SLOT(r_get_surface_info), SLOT(r_lock_surface), SLOT(r_unlock_surface),
        SLOT(r_optimize_surface), SLOT(r_unoptimize_surface), SLOT(r_lock_screen),
        SLOT(r_unlock_screen), SLOT(r_blit_to_screen), SLOT(r_warp_to_screen),
        SLOT(r_make_screenshot), SLOT(r_read_console_variables), SLOT(r_blit_from_screen),
    };
    int i;
    g_rs = rs;
    for (i = 0; i < RS_SLOT_COUNT; i++) rs->slots[i] = slots[i];
    rlog("avp2m_d3d11 %s: RenderDLLSetup(%p)", RENDERER_VERSION, (void *)rs);
}

/* Display modes: everything the primary output supports at 32 bpp, plus the
 * classic 4:3 modes, deduplicated, smallest first. */
static void add_mode(RenderMode **head, uint32_t w, uint32_t h, const char *desc)
{
    RenderMode *m, **pp;
    for (m = *head; m; m = m->next)
        if (m->width == w && m->height == h) return;
    m = (RenderMode *)calloc(1, sizeof *m);
    if (!m) return;
    m->hardware = 1;
    lstrcpynA(m->render_dll, "avp2m_d3d11.ren", sizeof m->render_dll);
    lstrcpynA(m->internal_name, "D3D11", sizeof m->internal_name);
    lstrcpynA(m->description, desc, sizeof m->description);
    m->width = w;
    m->height = h;
    m->bit_depth = 32;
    for (pp = head; *pp && ((*pp)->width < w || ((*pp)->width == w && (*pp)->height < h)); pp = &(*pp)->next) {}
    m->next = *pp;
    *pp = m;
}

__declspec(dllexport) RenderMode *__cdecl GetSupportedModes(void)
{
    RenderMode *head = NULL;
    IDXGIFactory *f = NULL;
    IDXGIAdapter *a = NULL;
    IDXGIOutput *o = NULL;
    char desc[128] = "Direct3D 11";
    static const uint32_t classic[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 } };
    int i;

    if (SUCCEEDED(CreateDXGIFactory(&IID_IDXGIFactory, (void **)&f)) &&
        SUCCEEDED(IDXGIFactory_EnumAdapters(f, 0, &a))) {
        DXGI_ADAPTER_DESC ad;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(a, &ad))) {
            char name[100];
            WideCharToMultiByte(CP_ACP, 0, ad.Description, -1, name, sizeof name, NULL, NULL);
            wsprintfA(desc, "D3D11 - %.100s", name);
        }
        if (SUCCEEDED(IDXGIAdapter_EnumOutputs(a, 0, &o))) {
            UINT n = 0;
            IDXGIOutput_GetDisplayModeList(o, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &n, NULL);
            if (n) {
                DXGI_MODE_DESC *md = (DXGI_MODE_DESC *)calloc(n, sizeof *md);
                if (md && SUCCEEDED(IDXGIOutput_GetDisplayModeList(o, DXGI_FORMAT_B8G8R8A8_UNORM, 0, &n, md))) {
                    UINT k;
                    for (k = 0; k < n; k++)
                        if (md[k].Width >= 640 && md[k].Height >= 480)
                            add_mode(&head, md[k].Width, md[k].Height, desc);
                }
                free(md);
            }
            IDXGIOutput_Release(o);
        }
        IDXGIAdapter_Release(a);
    }
    if (f) IDXGIFactory_Release(f);
    for (i = 0; i < 3; i++) add_mode(&head, classic[i][0], classic[i][1], desc);
    return head;
}

__declspec(dllexport) void __cdecl FreeModeList(RenderMode *list)
{
    while (list) {
        RenderMode *n = list->next;
        free(list);
        list = n;
    }
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        g_t0 = GetTickCount();
        DisableThreadLibraryCalls(inst);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log != INVALID_HANDLE_VALUE) CloseHandle(g_log);
    }
    return TRUE;
}
