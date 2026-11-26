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

#include "../render_struct.h"

#define RENDERER_VERSION "0.1.0"
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
static ID3D11SamplerState *g_samp_point, *g_samp_linear;
static ID3D11BlendState   *g_blend[OPT2D_BLEND_COUNT];
static ID3D11RasterizerState *g_rast;
static ID3D11Texture2D *g_white_tex;
static ID3D11ShaderResourceView *g_white_srv;

static UINT g_vb_pos;                          /* in vertices */

static Surface g_surfaces = { 0 };             /* list head   */

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

    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_point);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ID3D11Device_CreateSamplerState(g_dev, &sd, &g_samp_linear);

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
    return g_rt_rtv && g_rt_srv;
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
    SAFE_RELEASE(g_cb, ID3D11Buffer);
    SAFE_RELEASE(g_vb, ID3D11Buffer);
    SAFE_RELEASE(g_layout, ID3D11InputLayout);
    SAFE_RELEASE(g_ps, ID3D11PixelShader);
    SAFE_RELEASE(g_vs, ID3D11VertexShader);
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

static int __cdecl r_render_scene(void *scene)         { (void)scene; return 1; /* milestone 2 */ }
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
static int  __cdecl r_bind_texture(void *tex, int changed)   { (void)tex; (void)changed; return 1; }
static void __cdecl r_unbind_texture(void *tex)              { (void)tex; }
static int  __cdecl r_load_world_data(void *p)               { (void)p; return 1; }
static void *__cdecl r_create_context(void *init)            { (void)init; return calloc(1, 16); }
static void __cdecl r_delete_context(void *ctx)              { free(ctx); }
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
