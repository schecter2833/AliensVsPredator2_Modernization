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
#include <string.h>
#include <math.h>

#include "../render_struct.h"

#define RENDERER_VERSION "0.2.0-dev"
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

static ID3D11Device        *g_dev;
static ID3D11DeviceContext *g_ctx;
static IDXGISwapChain      *g_swap;
static ID3D11RenderTargetView *g_bb_rtv;
static uint32_t g_bb_w, g_bb_h;

static ID3D11Texture2D          *g_rt_tex;     /* game-resolution render target */
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
typedef struct WVertex { float x, y, z, u, v; uint32_t rgba; } WVertex;

/* Renderer-side texture, created from engine TextureData. Its pointer is
 * stored in SharedTexture+0x0C, like the stock renderer does. */
typedef struct RTexture {
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    uint32_t w, h;
    void *shared;                    /* engine SharedTexture */
    struct RTexture *next;
} RTexture;
static RTexture *g_textures;

typedef struct DrawBatch { RTexture *tex; UINT first, count; } DrawBatch;
typedef struct RenderContext {
    void *world;                     /* engine world (first field of the init block) */
    ID3D11Buffer *vb;
    UINT vcount;
    DrawBatch *batches;
    UINT nbatches;
    int built;
} RenderContext;
static RenderContext *g_cur_ctx;
static ID3D11VertexShader *g_vs3d;
static ID3D11PixelShader  *g_ps3d;
static ID3D11InputLayout  *g_layout3d;
static ID3D11Buffer       *g_cb3d;
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
    return s ? atoi(s) : def;
}

/* ------------------------------------------------------------------ */
/* Shaders                                                              */
/* ------------------------------------------------------------------ */
static const char k_hlsl3d[] =
    "cbuffer C : register(b0) { float4x4 vp; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct VI { float3 p : POSITION; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "struct VO { float4 p : SV_Position; float2 uv : TEXCOORD; float4 c : COLOR; };\n"
    "VO vs(VI i) { VO o; o.p = mul(float4(i.p, 1), vp); o.uv = i.uv; o.c = i.c; return o; }\n"
    "float4 ps(VO i) : SV_Target { float4 c = t.Sample(s, i.uv); clip(c.a - 0.5); return float4(c.rgb * i.c.rgb, 1); }\n";

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
        D3D11_INPUT_ELEMENT_DESC il3[3] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM,  0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        vsb = psb = err = NULL;
        hr = compile(k_hlsl3d, sizeof(k_hlsl3d) - 1, "avp2m3d", NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err);
        if (SUCCEEDED(hr)) hr = compile(k_hlsl3d, sizeof(k_hlsl3d) - 1, "avp2m3d", NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err);
        if (FAILED(hr)) { rlog("3D shader compile failed: %s", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 0; }
        hr = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &g_vs3d);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &g_ps3d);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateInputLayout(g_dev, il3, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &g_layout3d);
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
    bd.ByteWidth = 64;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(g_dev, &bd, NULL, &g_cb3d))) return 0;
    {
        D3D11_DEPTH_STENCIL_DESC dd;
        memset(&dd, 0, sizeof dd);
        dd.DepthEnable = TRUE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ID3D11Device_CreateDepthStencilState(g_dev, &dd, &g_ds_on);
        dd.DepthEnable = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
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
    memset(&td, 0, sizeof td);
    td.Width = g_w;
    td.Height = g_h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &td, NULL, &g_rt_tex))) return 0;
    ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)g_rt_tex, NULL, &g_rt_rtv);
    ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_rt_tex, NULL, &g_rt_srv);
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
    SAFE_RELEASE(g_layout3d, ID3D11InputLayout);
    SAFE_RELEASE(g_ps3d, ID3D11PixelShader);
    SAFE_RELEASE(g_vs3d, ID3D11VertexShader);
    SAFE_RELEASE(g_rt_srv, ID3D11ShaderResourceView);
    SAFE_RELEASE(g_rt_rtv, ID3D11RenderTargetView);
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

static void bind_pipeline(ID3D11RenderTargetView *rtv, float w, float h, ID3D11SamplerState *samp)
{
    D3D11_VIEWPORT vp = { 0, 0, w, h, 0, 1 };
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
    bind_pipeline(g_rt_rtv, (float)g_w, (float)g_h, g_samp_point);
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

typedef struct BuildTri { RTexture *tex; WVertex v[3]; } BuildTri;

static int cmp_tri(const void *a, const void *b)
{
    uintptr_t x = (uintptr_t)((const BuildTri *)a)->tex, y = (uintptr_t)((const BuildTri *)b)->tex;
    return x < y ? -1 : x > y;
}

/* Surface flags (LithTech; tentative names). */
#define SURF_INVISIBLE 0x04
#define SURF_SKY       0x10

static void build_world(RenderContext *rc)
{
    const uint8_t *world = (const uint8_t *)rc->world;
    const uint8_t *list;
    uint32_t nbsp, b, total_polys = 0, skipped = 0, hidden = 0, untextured = 0;
    BuildTri *tris = NULL;
    size_t cap = 0, n = 0, i;
    WVertex *verts;

    rc->built = 1;
    if (!readable(world, 0x194)) { rlog("world: bad world pointer %p", (void *)world); return; }
    list = RDPTR(world, 0x18C);
    nbsp = RD32(world, 0x190);
    if (nbsp > 4096 || !readable(list, nbsp * 4)) { rlog("world: bad bsp list %p n=%lu", (void *)list, (unsigned long)nbsp); return; }

    for (b = 0; b < nbsp; b++) {
        const uint8_t *entry = RDPTR(list, b * 4), *bsp, *polies;
        uint32_t np, pi;
        if (!readable(entry, 8)) continue;
        bsp = RDPTR(entry, 4);
        if (!readable(bsp, 0xA8)) continue;
        polies = RDPTR(bsp, 0xA0);
        np = RD32(bsp, 0xA4);
        if (np > 2000000 || !readable(polies, np * 4)) { skipped++; continue; }
        for (pi = 0; pi < np; pi++) {
            const uint8_t *poly = RDPTR(polies, pi * 4), *vlist, *surface, *v0;
            const float *plane, *p0;
            uint32_t nv, k, rgba, sflags;
            RTexture *tex = NULL;
            float iw = 1.0f / 64, ih = 1.0f / 64;
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
            {   /* gentle directional term so geometry reads well before lightmaps */
                float l = 0.55f + 0.45f * (0.5f + 0.5f * (plane[1] * 0.8f + plane[0] * 0.36f + plane[2] * 0.48f));
                uint32_t c = (uint32_t)(l * 255);
                rgba = 0xFF000000u | (c << 16) | (c << 8) | c;
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
                for (c = 0; c < 3; c++) {
                    const float *p = (const float *)RDPTR(vv[c], 0);
                    const float *uv = (const float *)(vv[c] + 4);
                    if (!readable(p, 12)) break;
                    tris[n].v[c].x = p[0]; tris[n].v[c].y = p[1]; tris[n].v[c].z = p[2];
                    tris[n].v[c].u = uv[0] * iw; tris[n].v[c].v = uv[1] * ih;
                    tris[n].v[c].rgba = rgba;
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
            for (i = 0; i < n; i++) {
                memcpy(&verts[i * 3], tris[i].v, sizeof tris[i].v);
                if (!rc->nbatches || rc->batches[rc->nbatches - 1].tex != tris[i].tex) {
                    rc->batches[rc->nbatches].tex = tris[i].tex;
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
        }
        free(verts);
    }
    free(tris);
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

static void render_world(const uint8_t *sd)
{
    RenderContext *rc;
    const float *pos, *rot;
    float xfov, yfov, farz, nearz = 1.0f, sx, sy, a, bq;
    float r[3], u[3], f[3], m[16];
    const int32_t *rect;
    D3D11_VIEWPORT vp;
    D3D11_MAPPED_SUBRESOURCE map;
    UINT stride = sizeof(WVertex), offset = 0;
    float bf[4] = { 0, 0, 0, 0 };
    static int logged;

    if (!readable(sd, 0xF4) || RD32(sd, SD_DRAWMODE) != 1) return;
    rc = (RenderContext *)RDPTR(sd, SD_CONTEXT);
    if (!rc || rc != g_cur_ctx) return;
    if (!rc->built) build_world(rc);
    if (!rc->vb) return;

    pos = (const float *)(sd + SD_POS);
    rot = (const float *)(sd + SD_ROT);
    rect = (const int32_t *)(sd + SD_RECT);
    xfov = *(const float *)(sd + SD_XFOV);
    yfov = *(const float *)(sd + SD_YFOV);
    farz = *(const float *)(sd + SD_FARZ);
    if (!(farz > 10.0f && farz < 1e7f)) farz = 10000.0f;
    if (!(xfov > 0.05f && xfov < 3.1f)) xfov = 1.5708f;
    if (!(yfov > 0.05f && yfov < 3.1f)) yfov = 1.1781f;
    if (logged < 3) {
        rlog("scene: pos %d,%d,%d rot %d,%d,%d,%d (x1000) fov %d,%d (mrad) far %d rect %ld,%ld,%ld,%ld",
             (int)pos[0], (int)pos[1], (int)pos[2], (int)(rot[0] * 1000), (int)(rot[1] * 1000),
             (int)(rot[2] * 1000), (int)(rot[3] * 1000), (int)(xfov * 1000), (int)(yfov * 1000), (int)farz,
             (long)rect[0], (long)rect[1], (long)rect[2], (long)rect[3]);
        logged++;
    }

    flush_batch();
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
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cb3d, 0);
    }

    vp.TopLeftX = (float)rect[0];
    vp.TopLeftY = (float)rect[1];
    vp.Width = (float)(rect[2] - rect[0]);
    vp.Height = (float)(rect[3] - rect[1]);
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    if (vp.Width <= 0 || vp.Height <= 0) { vp.TopLeftX = vp.TopLeftY = 0; vp.Width = (float)g_w; vp.Height = (float)g_h; }

    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, &g_rt_rtv, g_dsv);
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_ds_on, 0);
    ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend[OPT2D_SOLID], bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);
    ID3D11DeviceContext_RSSetState(g_ctx, g_rast);
    ID3D11DeviceContext_IASetInputLayout(g_ctx, g_layout3d);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_ctx, 0, 1, &rc->vb, &stride, &offset);
    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs3d, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(g_ctx, 0, 1, &g_cb3d);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps3d, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, &g_samp_wrap);
    {
        UINT bi;
        for (bi = 0; bi < rc->nbatches; bi++) {
            ID3D11ShaderResourceView *srv = rc->batches[bi].tex ? rc->batches[bi].tex->srv : g_white_srv;
            ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &srv);
            ID3D11DeviceContext_Draw(g_ctx, rc->batches[bi].count, rc->batches[bi].first);
        }
    }

    /* back to the 2D pipeline */
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

static int __cdecl r_render_scene(void *scene)         { if (g_ctx) render_world((const uint8_t *)scene); return 1; }
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

    /* Scale the game image to the window, preserving aspect ratio. */
    sx = (float)g_bb_w / (float)g_w;
    sy = (float)g_bb_h / (float)g_h;
    s = sx < sy ? sx : sy;
    dw = g_w * s;
    dh = g_h * s;
    x0 = ((float)g_bb_w - dw) * 0.5f;
    y0 = ((float)g_bb_h - dh) * 0.5f;

    bind_pipeline(g_bb_rtv, (float)g_bb_w, (float)g_bb_h,
                  (s == 1.0f) ? g_samp_point : g_samp_linear);
    ID3D11DeviceContext_ClearRenderTargetView(g_ctx, g_bb_rtv, black);
    push_quad(g_rt_srv, OPT2D_SOLID, x0, y0, x0 + dw, y0 + dh, 0, 0, 1, 1, 0xFFFFFFFFu);
    flush_batch();
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, &none);

    IDXGISwapChain_Present(g_swap, g_vsync ? 1 : 0, 0);
    g_batch_srv = NULL;
    begin_game_pass();
}

uint32_t g_frame_no;
static void __cdecl r_swap_buffers(uint32_t flags)
{
    (void)flags;
    g_frame_no++;
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
    free(rc->batches);
    free(rc);
}
static void *__cdecl r_lock_screen(void)                     { return NULL; }
static void __cdecl r_unlock_screen(void)                    { }
static int  __cdecl r_warp_to_screen(void *req)              { (void)req; return 0; }
static void __cdecl r_make_screenshot(const char *file)      { rlog("MakeScreenShot(%s): not implemented yet", file ? file : "?"); }
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
