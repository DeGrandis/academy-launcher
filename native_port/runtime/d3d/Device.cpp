#include "d3d/XboxD3D.h"
#include "d3d/PixelShader.h"
#include "d3d/VertexShader.h"

#include "Hle.h"
#include "Log.h"

#include <timeapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace cw::d3d {

namespace {

constexpr std::uint32_t kRenderStateAddress = 0x002E4250;
constexpr std::uint32_t kTextureStateAddress = 0x002E4050;
constexpr std::uint32_t kDevicePointerAddress = 0x002E44E8;
DWORD* const g_rs = reinterpret_cast<DWORD*>(static_cast<std::uintptr_t>(kRenderStateAddress));
DWORD* const g_tss = reinterpret_cast<DWORD*>(static_cast<std::uintptr_t>(kTextureStateAddress));

// Indices into D3D_g_RenderState for XDK 5233 (MULTISAMPLETYPE removed, later states shift down by one).
enum RenderState : int {
    RS_ZFUNC = 57, RS_ALPHAFUNC = 58, RS_ALPHABLENDENABLE = 59, RS_ALPHATESTENABLE = 60, RS_ALPHAREF = 61,
    RS_SRCBLEND = 62, RS_DESTBLEND = 63, RS_ZWRITEENABLE = 64, RS_DITHERENABLE = 65, RS_SHADEMODE = 66,
    RS_COLORWRITEENABLE = 67, RS_STENCILZFAIL = 68, RS_STENCILPASS = 69, RS_STENCILFUNC = 70, RS_STENCILREF = 71,
    RS_STENCILMASK = 72, RS_STENCILWRITEMASK = 73, RS_BLENDOP = 74, RS_BLENDCOLOR = 75,
    RS_FOGENABLE = 92, RS_FOGTABLEMODE = 93, RS_FOGSTART = 94, RS_FOGEND = 95, RS_FOGDENSITY = 96, RS_RANGEFOGENABLE = 97,
    RS_WRAP0 = 98, RS_LIGHTING = 102, RS_SPECULARENABLE = 103, RS_LOCALVIEWER = 104, RS_COLORVERTEX = 105,
    RS_SPECULARMATERIALSOURCE = 110, RS_DIFFUSEMATERIALSOURCE = 111, RS_AMBIENTMATERIALSOURCE = 112,
    RS_EMISSIVEMATERIALSOURCE = 113, RS_AMBIENT = 115, RS_POINTSIZE = 116,
    RS_PSTEXTUREMODES = 136, RS_VERTEXBLEND = 137, RS_FOGCOLOR = 138, RS_FILLMODE = 139, RS_BACKFILLMODE = 140,
    RS_TWOSIDEDLIGHTING = 141, RS_NORMALIZENORMALS = 142, RS_ZENABLE = 143, RS_STENCILENABLE = 144, RS_STENCILFAIL = 145,
    RS_FRONTFACE = 146, RS_CULLMODE = 147, RS_TEXTUREFACTOR = 148, RS_ZBIAS = 149, RS_LOGICOP = 150,
    RS_EDGEANTIALIAS = 151, RS_MULTISAMPLEANTIALIAS = 152, RS_MULTISAMPLEMASK = 153, RS_MULTISAMPLEMODE = 154,
    RS_MULTISAMPLERENDERTARGETMODE = 155, RS_SHADOWFUNC = 156, RS_LINEWIDTH = 157, RS_SAMPLEALPHA = 158,
    RS_DXT1NOISEENABLE = 159, RS_YUVENABLE = 160, RS_OCCLUSIONCULLENABLE = 161, RS_STENCILCULLENABLE = 162,
    RS_ROPZCMPALWAYSREAD = 163, RS_ROPZREAD = 164, RS_DONOTCULLUNCOMPRESSED = 165,
};

enum TextureState : int {
    TSS_ADDRESSU = 0, TSS_ADDRESSV = 1, TSS_ADDRESSW = 2, TSS_MAGFILTER = 3, TSS_MINFILTER = 4, TSS_MIPFILTER = 5,
    TSS_MIPMAPLODBIAS = 6, TSS_MAXMIPLEVEL = 7, TSS_MAXANISOTROPY = 8, TSS_COLOROP = 12, TSS_COLORARG0 = 13,
    TSS_COLORARG1 = 14, TSS_COLORARG2 = 15, TSS_ALPHAOP = 16, TSS_ALPHAARG0 = 17, TSS_ALPHAARG1 = 18,
    TSS_ALPHAARG2 = 19, TSS_RESULTARG = 20, TSS_TEXTURETRANSFORMFLAGS = 21,
};
constexpr int kStageSize = 32;
constexpr DWORD kStateUnknown = 0x7FFFFFFF;

struct XPresentParameters {
    UINT BackBufferWidth;
    UINT BackBufferHeight;
    DWORD BackBufferFormat;
    UINT BackBufferCount;
    DWORD MultiSampleType;
    DWORD SwapEffect;
    HWND hDeviceWindow;
    BOOL Windowed;
    BOOL EnableAutoDepthStencil;
    DWORD AutoDepthStencilFormat;
    DWORD Flags;
    UINT RefreshRate;
    UINT PresentationInterval;
    XSurface* BufferSurfaces[3];
    XSurface* DepthStencilSurface;
};

struct XDisplayMode {
    UINT Width;
    UINT Height;
    UINT RefreshRate;
    DWORD Flags;
    DWORD Format;
};

struct XSurfaceDesc {
    DWORD Format;
    DWORD Type;
    DWORD Usage;
    UINT Size;
    DWORD MultiSampleType;
    UINT Width;
    UINT Height;
};

struct XPixelShader {
    DWORD RefCount;
    DWORD D3DOwned;
    DWORD* Definition;
};

struct XVertexShader {
    std::vector<DWORD> declaration;
    std::vector<DWORD> function;
    HostVertexShader host;
};

struct Stream {
    XResource* buffer = nullptr;
    UINT stride = 0;
};

HWND g_window = nullptr;
IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_device = nullptr;
IDirect3DSurface9* g_hostBackBuffer = nullptr;
IDirect3DSurface9* g_hostDepth = nullptr;
UINT g_width = 640;
UINT g_height = 480;
std::uint8_t g_fakeDevice[0x2000] = {};
XSurface g_backBuffer{};
XSurface g_depthBuffer{};
XPixelContainer* g_renderTarget = nullptr;
std::recursive_mutex g_lock;

Stream g_streams[16];
XResource* g_indexBuffer = nullptr;
UINT g_baseVertexIndex = 0;
XPixelContainer* g_textures[4] = {};
DWORD g_texCoordIndex[4] = {0, 1, 2, 3};
DWORD g_borderColor[4] = {};
DWORD g_vertexShader = 0;
DWORD g_pixelShader = 0;
float g_vertexConstants[192][4] = {};
D3DMATRIX g_transforms[10] = {};
D3DVIEWPORT9 g_viewport{0, 0, 640, 480, 0.0f, 1.0f};
std::map<std::pair<const XPixelContainer*, UINT>, XSurface*> g_surfaceLevels;
std::map<const XPixelContainer*, IDirect3DTexture9*> g_hostRenderTargets;
DWORD g_frame = 0;
DWORD g_drawsThisFrame = 0;
DWORD g_skippedShaderDraws = 0;
bool g_reportedPixelShader = false;

std::uint8_t* xboxPointer(DWORD address) {
    return reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(address));
}

void* allocate(std::size_t size) {
    return VirtualAlloc(nullptr, std::max<std::size_t>(size, 1), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}

UINT log2Of(UINT value) {
    UINT result = 0;
    while ((1u << result) < value) {
        ++result;
    }
    return result;
}

float asFloat(DWORD value) {
    float result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

DWORD asDword(float value) {
    DWORD result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

// ---------------------------------------------------------------- Xbox -> PC value conversion

DWORD convertCompare(DWORD value) {
    return (value & 0xF) + 1;
}

DWORD convertBlend(DWORD value) {
    switch (value) {
    case 0x000: return D3DBLEND_ZERO;
    case 0x001: return D3DBLEND_ONE;
    case 0x300: return D3DBLEND_SRCCOLOR;
    case 0x301: return D3DBLEND_INVSRCCOLOR;
    case 0x302: return D3DBLEND_SRCALPHA;
    case 0x303: return D3DBLEND_INVSRCALPHA;
    case 0x304: return D3DBLEND_DESTALPHA;
    case 0x305: return D3DBLEND_INVDESTALPHA;
    case 0x306: return D3DBLEND_DESTCOLOR;
    case 0x307: return D3DBLEND_INVDESTCOLOR;
    case 0x308: return D3DBLEND_SRCALPHASAT;
    case 0x8001:
    case 0x8003: return D3DBLEND_BLENDFACTOR;
    case 0x8002:
    case 0x8004: return D3DBLEND_INVBLENDFACTOR;
    default: return D3DBLEND_ONE;
    }
}

DWORD convertBlendOp(DWORD value) {
    switch (value) {
    case 0x800A: return D3DBLENDOP_SUBTRACT;
    case 0x800B: return D3DBLENDOP_REVSUBTRACT;
    case 0x8007: return D3DBLENDOP_MIN;
    case 0x8008: return D3DBLENDOP_MAX;
    default: return D3DBLENDOP_ADD;
    }
}

DWORD convertStencilOp(DWORD value) {
    switch (value) {
    case 0x0000: return D3DSTENCILOP_ZERO;
    case 0x1E01: return D3DSTENCILOP_REPLACE;
    case 0x1E02: return D3DSTENCILOP_INCRSAT;
    case 0x1E03: return D3DSTENCILOP_DECRSAT;
    case 0x150A: return D3DSTENCILOP_INVERT;
    case 0x8507: return D3DSTENCILOP_INCR;
    case 0x8508: return D3DSTENCILOP_DECR;
    default: return D3DSTENCILOP_KEEP;
    }
}

DWORD convertCull(DWORD value) {
    switch (value) {
    case 0x900: return D3DCULL_CW;
    case 0x901: return D3DCULL_CCW;
    default: return D3DCULL_NONE;
    }
}

DWORD convertFill(DWORD value) {
    switch (value) {
    case 0x1B00: return D3DFILL_POINT;
    case 0x1B01: return D3DFILL_WIREFRAME;
    default: return D3DFILL_SOLID;
    }
}

DWORD convertColorWrite(DWORD value) {
    return ((value & (1u << 16)) ? D3DCOLORWRITEENABLE_RED : 0) | ((value & (1u << 8)) ? D3DCOLORWRITEENABLE_GREEN : 0) |
        ((value & 1u) ? D3DCOLORWRITEENABLE_BLUE : 0) | ((value & (1u << 24)) ? D3DCOLORWRITEENABLE_ALPHA : 0);
}

DWORD convertWrap(DWORD value) {
    return ((value & 0x10) ? D3DWRAPCOORD_0 : 0) | ((value & 0x1000) ? D3DWRAPCOORD_1 : 0) | ((value & 0x100000) ? D3DWRAPCOORD_2 : 0);
}

DWORD convertTextureOp(DWORD value) {
    static constexpr DWORD kMap[] = {
        0, D3DTOP_DISABLE, D3DTOP_SELECTARG1, D3DTOP_SELECTARG2, D3DTOP_MODULATE, D3DTOP_MODULATE2X, D3DTOP_MODULATE4X,
        D3DTOP_ADD, D3DTOP_ADDSIGNED, D3DTOP_ADDSIGNED2X, D3DTOP_SUBTRACT, D3DTOP_ADDSMOOTH, D3DTOP_BLENDDIFFUSEALPHA,
        D3DTOP_BLENDCURRENTALPHA, D3DTOP_BLENDTEXTUREALPHA, D3DTOP_BLENDFACTORALPHA, D3DTOP_BLENDTEXTUREALPHAPM,
        D3DTOP_PREMODULATE, D3DTOP_MODULATEALPHA_ADDCOLOR, D3DTOP_MODULATECOLOR_ADDALPHA, D3DTOP_MODULATEINVALPHA_ADDCOLOR,
        D3DTOP_MODULATEINVCOLOR_ADDALPHA, D3DTOP_DOTPRODUCT3, D3DTOP_MULTIPLYADD, D3DTOP_LERP, D3DTOP_BUMPENVMAP,
        D3DTOP_BUMPENVMAPLUMINANCE,
    };
    return value < std::size(kMap) ? kMap[value] : D3DTOP_DISABLE;
}

DWORD convertAddress(DWORD value) {
    return value == 5 ? D3DTADDRESS_CLAMP : (value >= 1 && value <= 4 ? value : D3DTADDRESS_WRAP);
}

DWORD convertFilter(DWORD value) {
    return value >= 4 ? D3DTEXF_LINEAR : value;
}

DWORD convertTexCoordIndex(DWORD value) {
    const DWORD mode = value & 0xFFFF0000;
    const DWORD index = value & 0xFFFF;
    switch (mode) {
    case 0x00010000: return D3DTSS_TCI_CAMERASPACENORMAL | index;
    case 0x00020000: return D3DTSS_TCI_CAMERASPACEPOSITION | index;
    case 0x00030000: return D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR | index;
    case 0x00050000: return D3DTSS_TCI_SPHEREMAP | index;
    default: return index;
    }
}

D3DTRANSFORMSTATETYPE convertTransform(DWORD state) {
    if (state == 0) return D3DTS_VIEW;
    if (state == 1) return D3DTS_PROJECTION;
    if (state >= 2 && state <= 5) return static_cast<D3DTRANSFORMSTATETYPE>(D3DTS_TEXTURE0 + (state - 2));
    return D3DTS_WORLDMATRIX(state - 6);
}

// ---------------------------------------------------------------- window and device

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CLOSE) {
        logf("window closed; exiting");
        ExitProcess(0);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void pumpMessages() {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void initializeSurface(XSurface& surface, DWORD format, UINT bytesPerPixel) {
    const UINT pitch = (g_width * bytesPerPixel + 63) & ~63u;
    surface.Common = 0x7FFF | kCommonTypeSurface | kCommonD3DCreated;
    surface.Data = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(allocate(static_cast<std::size_t>(pitch) * g_height)));
    surface.Lock = 0;
    surface.Format = kFormatDmaA | (2u << 4) | (format << 8) | (1u << 16);
    surface.Size = (g_width - 1) | ((g_height - 1) << 12) | (((pitch / 64) - 1) << 24);
    surface.Parent = nullptr;
}

void setDefaultStates() {
    std::fill(g_rs, g_rs + 166, 0u);
    g_rs[RS_ZFUNC] = 0x203;
    g_rs[RS_ALPHAFUNC] = 0x207;
    g_rs[RS_SRCBLEND] = 1;
    g_rs[RS_DESTBLEND] = 0;
    g_rs[RS_ZWRITEENABLE] = TRUE;
    g_rs[RS_SHADEMODE] = 0x1D01;
    g_rs[RS_COLORWRITEENABLE] = 0x01010101;
    g_rs[RS_STENCILZFAIL] = 0x1E00;
    g_rs[RS_STENCILPASS] = 0x1E00;
    g_rs[RS_STENCILFUNC] = 0x207;
    g_rs[RS_STENCILMASK] = 0xFF;
    g_rs[RS_STENCILWRITEMASK] = 0xFF;
    g_rs[RS_BLENDOP] = 0x8006;
    g_rs[RS_FOGEND] = asDword(1.0f);
    g_rs[RS_FOGDENSITY] = asDword(1.0f);
    g_rs[RS_LIGHTING] = TRUE;
    g_rs[RS_COLORVERTEX] = TRUE;
    g_rs[RS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    g_rs[RS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    g_rs[RS_POINTSIZE] = asDword(1.0f);
    g_rs[RS_FILLMODE] = 0x1B02;
    g_rs[RS_BACKFILLMODE] = 0x1B02;
    g_rs[RS_ZENABLE] = D3DZB_TRUE;
    g_rs[RS_STENCILFAIL] = 0x1E00;
    g_rs[RS_FRONTFACE] = 0x900;
    g_rs[RS_CULLMODE] = 0x901;
    g_rs[RS_TEXTUREFACTOR] = 0xFFFFFFFF;
    g_rs[RS_MULTISAMPLEANTIALIAS] = TRUE;
    g_rs[RS_MULTISAMPLEMASK] = 0xFFFFFFFF;
    g_rs[RS_LINEWIDTH] = asDword(1.0f);

    for (int stage = 0; stage < 4; ++stage) {
        DWORD* ts = g_tss + stage * kStageSize;
        std::fill(ts, ts + kStageSize, 0u);
        ts[TSS_ADDRESSU] = ts[TSS_ADDRESSV] = ts[TSS_ADDRESSW] = 1;
        ts[TSS_MAGFILTER] = ts[TSS_MINFILTER] = D3DTEXF_POINT;
        ts[TSS_MAXANISOTROPY] = 1;
        ts[TSS_COLOROP] = stage == 0 ? 4 : 1;
        ts[TSS_ALPHAOP] = stage == 0 ? 2 : 1;
        ts[TSS_COLORARG1] = ts[TSS_ALPHAARG1] = D3DTA_TEXTURE;
        ts[TSS_COLORARG2] = ts[TSS_ALPHAARG2] = D3DTA_CURRENT;
        ts[TSS_RESULTARG] = D3DTA_CURRENT;
        g_texCoordIndex[stage] = stage;
    }
}

void applyRenderStates() {
    auto set = [](D3DRENDERSTATETYPE state, DWORD value) { g_device->SetRenderState(state, value); };
    set(D3DRS_ZENABLE, g_rs[RS_ZENABLE]);
    set(D3DRS_ZWRITEENABLE, g_rs[RS_ZWRITEENABLE]);
    set(D3DRS_ZFUNC, convertCompare(g_rs[RS_ZFUNC]));
    set(D3DRS_ALPHATESTENABLE, g_rs[RS_ALPHATESTENABLE]);
    set(D3DRS_ALPHAFUNC, convertCompare(g_rs[RS_ALPHAFUNC]));
    set(D3DRS_ALPHAREF, g_rs[RS_ALPHAREF] & 0xFF);
    set(D3DRS_ALPHABLENDENABLE, g_rs[RS_ALPHABLENDENABLE]);
    set(D3DRS_SRCBLEND, convertBlend(g_rs[RS_SRCBLEND]));
    set(D3DRS_DESTBLEND, convertBlend(g_rs[RS_DESTBLEND]));
    set(D3DRS_BLENDOP, convertBlendOp(g_rs[RS_BLENDOP]));
    set(D3DRS_BLENDFACTOR, g_rs[RS_BLENDCOLOR]);
    set(D3DRS_DITHERENABLE, g_rs[RS_DITHERENABLE]);
    set(D3DRS_SHADEMODE, g_rs[RS_SHADEMODE] == 0x1D00 ? D3DSHADE_FLAT : D3DSHADE_GOURAUD);
    set(D3DRS_COLORWRITEENABLE, convertColorWrite(g_rs[RS_COLORWRITEENABLE]));
    set(D3DRS_STENCILENABLE, g_rs[RS_STENCILENABLE]);
    set(D3DRS_STENCILFAIL, convertStencilOp(g_rs[RS_STENCILFAIL]));
    set(D3DRS_STENCILZFAIL, convertStencilOp(g_rs[RS_STENCILZFAIL]));
    set(D3DRS_STENCILPASS, convertStencilOp(g_rs[RS_STENCILPASS]));
    set(D3DRS_STENCILFUNC, convertCompare(g_rs[RS_STENCILFUNC]));
    set(D3DRS_STENCILREF, g_rs[RS_STENCILREF]);
    set(D3DRS_STENCILMASK, g_rs[RS_STENCILMASK]);
    set(D3DRS_STENCILWRITEMASK, g_rs[RS_STENCILWRITEMASK]);
    set(D3DRS_FOGENABLE, g_rs[RS_FOGENABLE]);
    set(D3DRS_FOGTABLEMODE, g_rs[RS_FOGTABLEMODE]);
    set(D3DRS_FOGSTART, g_rs[RS_FOGSTART]);
    set(D3DRS_FOGEND, g_rs[RS_FOGEND]);
    set(D3DRS_FOGDENSITY, g_rs[RS_FOGDENSITY]);
    set(D3DRS_RANGEFOGENABLE, g_rs[RS_RANGEFOGENABLE]);
    set(D3DRS_FOGCOLOR, g_rs[RS_FOGCOLOR]);
    for (int index = 0; index < 4; ++index) {
        set(static_cast<D3DRENDERSTATETYPE>(D3DRS_WRAP0 + index), convertWrap(g_rs[RS_WRAP0 + index]));
    }
    set(D3DRS_LIGHTING, g_rs[RS_LIGHTING]);
    set(D3DRS_SPECULARENABLE, g_rs[RS_SPECULARENABLE]);
    set(D3DRS_LOCALVIEWER, g_rs[RS_LOCALVIEWER]);
    set(D3DRS_COLORVERTEX, g_rs[RS_COLORVERTEX]);
    set(D3DRS_SPECULARMATERIALSOURCE, g_rs[RS_SPECULARMATERIALSOURCE]);
    set(D3DRS_DIFFUSEMATERIALSOURCE, g_rs[RS_DIFFUSEMATERIALSOURCE]);
    set(D3DRS_AMBIENTMATERIALSOURCE, g_rs[RS_AMBIENTMATERIALSOURCE]);
    set(D3DRS_EMISSIVEMATERIALSOURCE, g_rs[RS_EMISSIVEMATERIALSOURCE]);
    set(D3DRS_AMBIENT, g_rs[RS_AMBIENT]);
    set(D3DRS_NORMALIZENORMALS, g_rs[RS_NORMALIZENORMALS]);
    set(D3DRS_FILLMODE, convertFill(g_rs[RS_FILLMODE]));
    static const bool wireframe = std::getenv("CW_WIREFRAME") != nullptr;
    if (wireframe) {
        set(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
    }
    set(D3DRS_CULLMODE, convertCull(g_rs[RS_CULLMODE]));
    set(D3DRS_TEXTUREFACTOR, g_rs[RS_TEXTUREFACTOR]);
    set(D3DRS_DEPTHBIAS, asDword(static_cast<float>(g_rs[RS_ZBIAS]) * -0.000005f));
}

void applyTextureStages(bool linearTextures[4], UINT textureSizes[4][2]) {
    for (DWORD stage = 0; stage < 4; ++stage) {
        const DWORD* ts = g_tss + stage * kStageSize;
        auto setStage = [&](D3DTEXTURESTAGESTATETYPE type, DWORD value) {
            if (value != kStateUnknown) {
                g_device->SetTextureStageState(stage, type, value);
            }
        };
        auto setSampler = [&](D3DSAMPLERSTATETYPE type, DWORD value) {
            if (value != kStateUnknown) {
                g_device->SetSamplerState(stage, type, value);
            }
        };
        setSampler(D3DSAMP_ADDRESSU, convertAddress(ts[TSS_ADDRESSU]));
        setSampler(D3DSAMP_ADDRESSV, convertAddress(ts[TSS_ADDRESSV]));
        setSampler(D3DSAMP_ADDRESSW, convertAddress(ts[TSS_ADDRESSW]));
        setSampler(D3DSAMP_MAGFILTER, convertFilter(ts[TSS_MAGFILTER]));
        setSampler(D3DSAMP_MINFILTER, convertFilter(ts[TSS_MINFILTER]));
        setSampler(D3DSAMP_MIPFILTER, convertFilter(ts[TSS_MIPFILTER]));
        setSampler(D3DSAMP_MIPMAPLODBIAS, ts[TSS_MIPMAPLODBIAS]);
        setSampler(D3DSAMP_MAXMIPLEVEL, ts[TSS_MAXMIPLEVEL]);
        setSampler(D3DSAMP_MAXANISOTROPY, std::max<DWORD>(1, ts[TSS_MAXANISOTROPY]));
        setSampler(D3DSAMP_BORDERCOLOR, g_borderColor[stage]);
        setStage(D3DTSS_COLOROP, convertTextureOp(ts[TSS_COLOROP]));
        setStage(D3DTSS_COLORARG0, ts[TSS_COLORARG0]);
        setStage(D3DTSS_COLORARG1, ts[TSS_COLORARG1]);
        setStage(D3DTSS_COLORARG2, ts[TSS_COLORARG2]);
        setStage(D3DTSS_ALPHAOP, convertTextureOp(ts[TSS_ALPHAOP]));
        setStage(D3DTSS_ALPHAARG0, ts[TSS_ALPHAARG0]);
        setStage(D3DTSS_ALPHAARG1, ts[TSS_ALPHAARG1]);
        setStage(D3DTSS_ALPHAARG2, ts[TSS_ALPHAARG2]);
        setStage(D3DTSS_RESULTARG, ts[TSS_RESULTARG]);
        setStage(D3DTSS_TEXTURETRANSFORMFLAGS, ts[TSS_TEXTURETRANSFORMFLAGS]);
        setStage(D3DTSS_TEXCOORDINDEX, convertTexCoordIndex(g_texCoordIndex[stage]));

        linearTextures[stage] = false;
        IDirect3DBaseTexture9* texture = nullptr;
        if (g_textures[stage] != nullptr) {
            texture = hostTexture(g_textures[stage]);
            const TextureLayout layout = describe(g_textures[stage]);
            linearTextures[stage] = layout.linear && texture != nullptr;
            textureSizes[stage][0] = layout.width;
            textureSizes[stage][1] = layout.height;
        }
        g_device->SetTexture(stage, texture);
    }
}

// ---------------------------------------------------------------- vertex processing

struct FvfLayout {
    UINT size = 0;
    bool pretransformed = false;
    UINT texCount = 0;
    UINT texOffset[8] = {};
    UINT texComponents[8] = {};
};

FvfLayout parseFvf(DWORD fvf) {
    FvfLayout layout;
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: layout.size = 12; break;
    case D3DFVF_XYZRHW: layout.size = 16; layout.pretransformed = true; break;
    case D3DFVF_XYZB1: layout.size = 16; break;
    case D3DFVF_XYZB2: layout.size = 20; break;
    case D3DFVF_XYZB3: layout.size = 24; break;
    case D3DFVF_XYZB4: layout.size = 28; break;
    default: break;
    }
    if (fvf & D3DFVF_NORMAL) layout.size += 12;
    if (fvf & D3DFVF_PSIZE) layout.size += 4;
    if (fvf & D3DFVF_DIFFUSE) layout.size += 4;
    if (fvf & D3DFVF_SPECULAR) layout.size += 4;
    layout.texCount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    for (UINT index = 0; index < layout.texCount && index < 8; ++index) {
        static constexpr UINT kComponents[] = {2, 3, 4, 1};
        layout.texComponents[index] = kComponents[(fvf >> (16 + index * 2)) & 3];
        layout.texOffset[index] = layout.size;
        layout.size += layout.texComponents[index] * 4;
    }
    return layout;
}

std::vector<std::uint8_t> g_vertexScratch;
std::vector<WORD> g_indexScratch;

// Copies vertices when Xbox-only conventions must be rewritten: screen-space Z and texel-space coordinates.
const std::uint8_t* adjustVertices(const std::uint8_t* source, UINT count, UINT stride, const FvfLayout& layout,
    const bool linearTextures[4], const UINT textureSizes[4][2]) {
    bool scaleTexture[8] = {};
    float scale[8][2] = {};
    bool any = layout.pretransformed;
    for (int stage = 0; stage < 4; ++stage) {
        const DWORD set = g_texCoordIndex[stage] & 0xFFFF;
        if (linearTextures[stage] && (g_texCoordIndex[stage] & 0xFFFF0000) == 0 && set < layout.texCount && layout.texComponents[set] >= 2) {
            scaleTexture[set] = true;
            scale[set][0] = 1.0f / static_cast<float>(textureSizes[stage][0]);
            scale[set][1] = 1.0f / static_cast<float>(textureSizes[stage][1]);
            any = true;
        }
    }
    if (!any) {
        return source;
    }

    g_vertexScratch.assign(source, source + static_cast<std::size_t>(count) * stride);
    for (UINT vertex = 0; vertex < count; ++vertex) {
        std::uint8_t* base = g_vertexScratch.data() + static_cast<std::size_t>(vertex) * stride;
        if (layout.pretransformed) {
            // Xbox screen-space Z is expressed in depth-buffer units (24-bit).
            reinterpret_cast<float*>(base)[2] /= 16777215.0f;
        }
        for (UINT set = 0; set < layout.texCount && set < 8; ++set) {
            if (scaleTexture[set]) {
                auto* uv = reinterpret_cast<float*>(base + layout.texOffset[set]);
                uv[0] *= scale[set][0];
                uv[1] *= scale[set][1];
            }
        }
    }
    return g_vertexScratch.data();
}

bool hostPrimitive(DWORD primitive, UINT vertexCount, D3DPRIMITIVETYPE& type, UINT& primitiveCount, bool& quads) {
    quads = false;
    switch (primitive) {
    case 1: type = D3DPT_POINTLIST; primitiveCount = vertexCount; break;
    case 2: type = D3DPT_LINELIST; primitiveCount = vertexCount / 2; break;
    case 3:
    case 4: type = D3DPT_LINESTRIP; primitiveCount = vertexCount > 1 ? vertexCount - 1 : 0; break;
    case 5: type = D3DPT_TRIANGLELIST; primitiveCount = vertexCount / 3; break;
    case 6:
    case 9: type = D3DPT_TRIANGLESTRIP; primitiveCount = vertexCount > 2 ? vertexCount - 2 : 0; break;
    case 7:
    case 10: type = D3DPT_TRIANGLEFAN; primitiveCount = vertexCount > 2 ? vertexCount - 2 : 0; break;
    case 8: type = D3DPT_TRIANGLELIST; primitiveCount = (vertexCount / 4) * 2; quads = true; break;
    default: return false;
    }
    return primitiveCount > 0;
}

void bindShaderState(const bool linearTextures[4], const UINT textureSizes[4][2]) {
    constexpr float kDepthScale = 16777215.0f;
    const float scale[4] = {g_viewport.Width * 0.5f, g_viewport.Height * -0.5f, (g_viewport.MaxZ - g_viewport.MinZ) * kDepthScale, 1.0f};
    const float offset[4] = {g_viewport.X + g_viewport.Width * 0.5f, g_viewport.Y + g_viewport.Height * 0.5f, g_viewport.MinZ * kDepthScale, 0.0f};
    // c-38/c-37 hold the viewport transform that XDK-compiled shaders append to oPos.
    std::memcpy(g_vertexConstants[58], scale, sizeof(scale));
    std::memcpy(g_vertexConstants[59], offset, sizeof(offset));
    g_device->SetVertexShaderConstantF(0, &g_vertexConstants[0][0], 192);
    g_device->SetVertexShaderConstantF(kViewportScaleConstant, scale, 1);
    g_device->SetVertexShaderConstantF(kViewportOffsetConstant, offset, 1);
    for (UINT stage = 0; stage < 4; ++stage) {
        const float texScale[4] = {linearTextures[stage] ? 1.0f / textureSizes[stage][0] : 1.0f,
            linearTextures[stage] ? 1.0f / textureSizes[stage][1] : 1.0f, 1.0f, 1.0f};
        g_device->SetVertexShaderConstantF(kTexScaleConstant + stage, texScale, 1);
        g_device->SetTextureStageState(stage, D3DTSS_TEXCOORDINDEX, stage);
        const DWORD transform = g_tss[stage * kStageSize + TSS_TEXTURETRANSFORMFLAGS];
        g_device->SetTextureStageState(stage, D3DTSS_TEXTURETRANSFORMFLAGS,
            transform != kStateUnknown && (transform & D3DTTFF_PROJECTED) ? D3DTTFF_PROJECTED | D3DTTFF_COUNT4 : D3DTTFF_DISABLE);
    }
    const float fog[4] = {asFloat(g_rs[RS_FOGSTART]), asFloat(g_rs[RS_FOGEND]), asFloat(g_rs[RS_FOGDENSITY]),
        static_cast<float>(g_rs[RS_FOGTABLEMODE])};
    g_device->SetVertexShaderConstantF(kFogConstant, fog, 1);
    g_device->SetRenderState(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
    g_device->SetRenderState(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
}

// Expands every declared attribute to float4 in a single interleaved stream.
const std::uint8_t* gatherShaderVertices(const HostVertexShader& shader, UINT first, UINT count) {
    const std::size_t stride = shader.inputs.size() * 16;
    g_vertexScratch.resize(stride * count);
    for (std::size_t slot = 0; slot < shader.inputs.size(); ++slot) {
        const ShaderInput& input = shader.inputs[slot];
        const Stream& stream = g_streams[input.stream];
        for (UINT vertex = 0; vertex < count; ++vertex) {
            auto* out = reinterpret_cast<float*>(g_vertexScratch.data() + vertex * stride + slot * 16);
            if (stream.buffer == nullptr) {
                out[0] = out[1] = out[2] = 0.0f;
                out[3] = 1.0f;
                continue;
            }
            convertAttribute(xboxPointer(stream.buffer->Data) + static_cast<std::size_t>(first + vertex) * stream.stride + input.offset, input.type, out);
        }
    }
    return g_vertexScratch.data();
}

// Stage state is stale while an Xbox pixel shader is bound; texture 0 x diffuse is the closest generic match.
void approximatePixelShader() {
    if (g_pixelShader == 0) {
        return;
    }
    const DWORD op = g_textures[0] != nullptr ? D3DTOP_MODULATE : D3DTOP_SELECTARG2;
    g_device->SetTextureStageState(0, D3DTSS_COLOROP, op);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAOP, op);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_device->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_device->SetTextureStageState(0, D3DTSS_RESULTARG, D3DTA_CURRENT);
    g_device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_device->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

void bindPixelShader() {
    if (g_pixelShader == 0) {
        g_device->SetPixelShader(nullptr);
        return;
    }
    const auto* definition = reinterpret_cast<const XPixelShader*>(static_cast<std::uintptr_t>(g_pixelShader))->Definition;
    // The live program is the D3DRS_PS* render states, which the game may edit after SetPixelShader.
    DWORD program[60];
    std::memcpy(program, g_rs, 57 * sizeof(DWORD));
    program[54] = g_rs[RS_PSTEXTUREMODES];
    std::memcpy(program + 57, definition + 57, 3 * sizeof(DWORD));
    IDirect3DPixelShader9* shader = hostPixelShader(g_device, program);
    g_device->SetPixelShader(shader);
    if (shader == nullptr) {
        approximatePixelShader();
        return;
    }
    float constants[kPixelShaderConstantCount][4];
    pixelShaderConstants(program, g_tss, kStageSize, constants);
    const DWORD fog = g_rs[RS_FOGCOLOR];
    constants[26][0] = ((fog >> 16) & 0xFF) / 255.0f;
    constants[26][1] = ((fog >> 8) & 0xFF) / 255.0f;
    constants[26][2] = (fog & 0xFF) / 255.0f;
    constants[26][3] = 1.0f;
    g_device->SetPixelShaderConstantF(0, &constants[0][0], kPixelShaderConstantCount);
}

// Binds state for a draw of vertices [first, first + count) and returns host-ready vertex data.
bool prepareDraw(UINT first, UINT count, const std::uint8_t*& vertices, UINT& stride) {
    static const DWORD logFrame = [] {
        const char* value = std::getenv("CW_DRAWLOG_FRAME");
        return value == nullptr ? 0ul : std::strtoul(value, nullptr, 10);
    }();
    if (logFrame != 0 && g_frame == logFrame) {
        const auto* shader = (g_vertexShader & 1) ? reinterpret_cast<XVertexShader*>(static_cast<std::uintptr_t>(g_vertexShader & ~1u)) : nullptr;
        logf("draw: vs=%08lX ps=%08lX first=%u count=%u stream0=%p/%u inputs=%u ins=%u tex0=%p", g_vertexShader, g_pixelShader, first, count,
            static_cast<void*>(g_streams[0].buffer), g_streams[0].stride, shader ? static_cast<UINT>(shader->host.inputs.size()) : 0,
            shader ? static_cast<UINT>(shader->function.size() / 4) : 0, static_cast<void*>(g_textures[0]));
        for (int stage = 0; stage < 4; ++stage) {
            if (g_textures[stage] != nullptr) {
                const TextureLayout layout = describe(reinterpret_cast<const XPixelContainer*>(g_textures[stage]));
                logf("draw:   tex%d %ux%u fmt=0x%02lX levels=%u", stage, layout.width, layout.height, layout.format, layout.levels);
            }
        }
        logf("draw:   fog=%lu mode=%lu start=%g end=%g c109=%g,%g,%g,%g ablend=%lu atest=%lu aref=%lu src=%lu dst=%lu zw=%lu cw=%lX",
            g_rs[RS_FOGENABLE], g_rs[RS_FOGTABLEMODE], asFloat(g_rs[RS_FOGSTART]), asFloat(g_rs[RS_FOGEND]), g_vertexConstants[109][0],
            g_vertexConstants[109][1], g_vertexConstants[109][2], g_vertexConstants[109][3], g_rs[RS_ALPHABLENDENABLE],
            g_rs[RS_ALPHATESTENABLE], g_rs[RS_ALPHAREF], g_rs[RS_SRCBLEND], g_rs[RS_DESTBLEND], g_rs[RS_ZWRITEENABLE], g_rs[RS_COLORWRITEENABLE]);
        logf("draw:   c96=%g,%g,%g,%g c99=%g,%g,%g,%g c100=%g,%g,%g,%g stream1=%p/%u", g_vertexConstants[96][0], g_vertexConstants[96][1],
            g_vertexConstants[96][2], g_vertexConstants[96][3], g_vertexConstants[99][0], g_vertexConstants[99][1], g_vertexConstants[99][2],
            g_vertexConstants[99][3], g_vertexConstants[100][0], g_vertexConstants[100][1], g_vertexConstants[100][2], g_vertexConstants[100][3],
            static_cast<void*>(g_streams[1].buffer), g_streams[1].stride);
    }
    bool linear[4] = {};
    UINT sizes[4][2] = {};
    if (g_pixelShader != 0 && !g_reportedPixelShader) {
        g_reportedPixelShader = true;
        logf("d3d: pixel shaders are approximated with the fixed-function texture stages");
    }
    if (g_vertexShader & 1) {
        auto* shader = reinterpret_cast<XVertexShader*>(static_cast<std::uintptr_t>(g_vertexShader & ~1u));
        if (!shader->host.translated) {
            translateVertexShader(g_device, shader->declaration, shader->function, shader->host);
        }
        if (shader->host.shader == nullptr || shader->host.inputs.empty()) {
            if (g_skippedShaderDraws++ % 500 == 0) {
                logf("d3d: skipping draw with untranslated vertex shader (%lu skipped so far)", g_skippedShaderDraws);
            }
            return false;
        }
        applyRenderStates();
        applyTextureStages(linear, sizes);
        bindPixelShader();
        g_device->SetVertexDeclaration(shader->host.declaration);
        g_device->SetVertexShader(shader->host.shader);
        bindShaderState(linear, sizes);
        static const bool debugSolid = std::getenv("CW_DEBUG_SOLID") != nullptr;
        if (debugSolid) {
            g_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            g_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            g_device->SetRenderState(D3DRS_ZFUNC, D3DCMP_ALWAYS);
            g_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            g_device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            g_device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            g_device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        }
        stride = static_cast<UINT>(shader->host.inputs.size() * 16);
        vertices = gatherShaderVertices(shader->host, first, count);
        ++g_drawsThisFrame;
        return true;
    }
    if (g_streams[0].buffer == nullptr) {
        return false;
    }
    const FvfLayout layout = parseFvf(g_vertexShader);
    g_device->SetVertexShader(nullptr);
    g_device->SetFVF(g_vertexShader);
    applyRenderStates();
    applyTextureStages(linear, sizes);
    bindPixelShader();
    stride = g_streams[0].stride;
    vertices = adjustVertices(xboxPointer(g_streams[0].buffer->Data) + static_cast<std::size_t>(first) * stride, count, stride, layout, linear, sizes);
    ++g_drawsThisFrame;
    return true;
}

// ---------------------------------------------------------------- D3D entry points

// Writes the back buffer to screenshot_<frame>.bmp for frames listed in CW_SCREENSHOT_FRAMES, and every
// CW_SCREENSHOT_EVERY frames.
void maybeCaptureScreenshot() {
    static const std::string frames = [] {
        const char* value = std::getenv("CW_SCREENSHOT_FRAMES");
        return value == nullptr ? std::string() : "," + std::string(value) + ",";
    }();
    static const unsigned every = [] {
        const char* value = std::getenv("CW_SCREENSHOT_EVERY");
        return value == nullptr ? 0u : static_cast<unsigned>(std::strtoul(value, nullptr, 10));
    }();
    const bool listed = !frames.empty() && frames.find("," + std::to_string(g_frame) + ",") != std::string::npos;
    if (!listed && (every == 0 || g_frame % every != 0)) {
        return;
    }
    IDirect3DSurface9* copy = nullptr;
    if (FAILED(g_device->CreateOffscreenPlainSurface(g_width, g_height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &copy, nullptr))) {
        return;
    }
    D3DLOCKED_RECT locked;
    if (SUCCEEDED(g_device->GetRenderTargetData(g_hostBackBuffer, copy)) && SUCCEEDED(copy->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        BITMAPFILEHEADER file{};
        BITMAPINFOHEADER info{};
        info.biSize = sizeof(info);
        info.biWidth = static_cast<LONG>(g_width);
        info.biHeight = -static_cast<LONG>(g_height);
        info.biPlanes = 1;
        info.biBitCount = 32;
        file.bfType = 0x4D42;
        file.bfOffBits = sizeof(file) + sizeof(info);
        file.bfSize = file.bfOffBits + g_width * g_height * 4;
        const std::string name = "screenshot_" + std::to_string(g_frame) + ".bmp";
        if (FILE* output = std::fopen(name.c_str(), "wb")) {
            std::fwrite(&file, sizeof(file), 1, output);
            std::fwrite(&info, sizeof(info), 1, output);
            for (UINT y = 0; y < g_height; ++y) {
                std::fwrite(static_cast<std::uint8_t*>(locked.pBits) + y * locked.Pitch, 4, g_width, output);
            }
            std::fclose(output);
            logf("d3d: wrote %s", name.c_str());
        }
        copy->UnlockRect();
    }
    copy->Release();
}

// CW_FPS unset: vsync at the monitor's refresh rate; CW_FPS=n: software cap without vsync; CW_FPS=0: unlimited.
int fpsSetting() {
    static const int fps = [] {
        const char* value = std::getenv("CW_FPS");
        return value == nullptr ? -1 : std::atoi(value);
    }();
    return fps;
}

HRESULT __stdcall xDirect3D_CreateDevice(UINT adapter, DWORD deviceType, HWND focusWindow, DWORD behaviorFlags,
    XPresentParameters* parameters, void** returnedDevice) {
    std::lock_guard lock(g_lock);
    g_width = parameters != nullptr && parameters->BackBufferWidth != 0 ? parameters->BackBufferWidth : 640;
    g_height = parameters != nullptr && parameters->BackBufferHeight != 0 ? parameters->BackBufferHeight : 480;
    logf("d3d: Direct3D_CreateDevice %ux%u flags=0x%lX interval=0x%X refresh=%u", g_width, g_height, parameters != nullptr ? parameters->Flags : 0,
        parameters != nullptr ? parameters->PresentationInterval : 0, parameters != nullptr ? parameters->RefreshRate : 0);

    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"CloneWarsXboxWindow";
    RegisterClassW(&windowClass);
    // In widescreen mode the game renders anamorphic 16:9 into 640x480, so the window supplies the horizontal stretch.
    const char* widescreenSetting = std::getenv("CW_WIDESCREEN");
    const bool widescreen = widescreenSetting == nullptr || std::strcmp(widescreenSetting, "0") != 0;
    const LONG windowHeight = static_cast<LONG>(g_height * 2);
    const LONG windowWidth = widescreen ? windowHeight * 16 / 9 : static_cast<LONG>(g_width * 2);
    RECT rect{0, 0, windowWidth, windowHeight};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_window = CreateWindowW(windowClass.lpszClassName, L"Star Wars: The Clone Wars", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, windowClass.hInstance, nullptr);

    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS present{};
    present.BackBufferWidth = g_width;
    present.BackBufferHeight = g_height;
    present.BackBufferFormat = D3DFMT_X8R8G8B8;
    present.BackBufferCount = 1;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = g_window;
    present.Windowed = TRUE;
    present.EnableAutoDepthStencil = TRUE;
    present.AutoDepthStencilFormat = D3DFMT_D24S8;
    present.PresentationInterval = fpsSetting() < 0 ? D3DPRESENT_INTERVAL_ONE : D3DPRESENT_INTERVAL_IMMEDIATE;
    const DWORD flags = D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
    HRESULT result = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window, flags | D3DCREATE_HARDWARE_VERTEXPROCESSING, &present, &g_device);
    if (FAILED(result)) {
        result = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_window, flags | D3DCREATE_SOFTWARE_VERTEXPROCESSING, &present, &g_device);
    }
    if (FAILED(result)) {
        fatal("d3d: could not create a Direct3D 9 device (0x%08lX)", result);
    }

    g_device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &g_hostBackBuffer);
    g_device->GetDepthStencilSurface(&g_hostDepth);
    initializeTextures(g_device);
    initializeSurface(g_backBuffer, kFmtLinX8R8G8B8, 4);
    initializeSurface(g_depthBuffer, kFmtLinD24S8, 4);
    g_renderTarget = &g_backBuffer;
    g_viewport = {0, 0, g_width, g_height, 0.0f, 1.0f};
    setDefaultStates();
    *reinterpret_cast<void**>(static_cast<std::uintptr_t>(kDevicePointerAddress)) = g_fakeDevice;
    if (parameters != nullptr) {
        parameters->BufferSurfaces[0] = &g_backBuffer;
        parameters->DepthStencilSurface = &g_depthBuffer;
    }
    if (returnedDevice != nullptr) {
        *returnedDevice = g_fakeDevice;
    }
    g_device->BeginScene();
    logf("d3d: device ready; window %p", g_window);
    return D3D_OK;
}

void limitFrameRate() {
    static LARGE_INTEGER frequency{};
    static LONGLONG next = 0;
    if (fpsSetting() <= 0) {
        return;
    }
    if (frequency.QuadPart == 0) {
        QueryPerformanceFrequency(&frequency);
        timeBeginPeriod(1);
    }
    const LONGLONG period = frequency.QuadPart / fpsSetting();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (next == 0 || now.QuadPart - next > period * 4) {
        next = now.QuadPart;
    }
    next += period;
    while (now.QuadPart < next) {
        const LONGLONG remainingMs = (next - now.QuadPart) * 1000 / frequency.QuadPart;
        Sleep(remainingMs > 1 ? static_cast<DWORD>(remainingMs - 1) : 0);
        QueryPerformanceCounter(&now);
    }
}

DWORD __stdcall xSwap(DWORD flags) {
    std::lock_guard lock(g_lock);
    g_device->EndScene();
    maybeCaptureScreenshot();
    g_device->Present(nullptr, nullptr, nullptr, nullptr);
    limitFrameRate();
    {
        static ULONGLONG windowStart = GetTickCount64();
        static DWORD windowFrames = 0;
        ++windowFrames;
        const ULONGLONG now = GetTickCount64();
        if (now - windowStart >= 1000) {
            wchar_t title[96];
            swprintf_s(title, L"Star Wars: The Clone Wars - %.1f FPS (frame %lu)", windowFrames * 1000.0 / (now - windowStart), g_frame);
            SetWindowTextW(g_window, title);
            windowStart = now;
            windowFrames = 0;
        }
    }
    pumpMessages();
    g_device->BeginScene();
    if (g_frame % 300 == 0) {
        logf("d3d: frame %lu (%lu draws)", g_frame, g_drawsThisFrame);
    }
    g_drawsThisFrame = 0;
    return ++g_frame;
}

void __stdcall xClear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    std::lock_guard lock(g_lock);
    DWORD hostFlags = 0;
    if (flags & 0xF0) hostFlags |= D3DCLEAR_TARGET;
    if (flags & 0x01) hostFlags |= D3DCLEAR_ZBUFFER;
    if (flags & 0x02) hostFlags |= D3DCLEAR_STENCIL;
    g_device->Clear(count, rects, hostFlags, color, std::clamp(z, 0.0f, 1.0f), stencil);
}

void __stdcall xDrawVertices(DWORD primitive, UINT startVertex, UINT vertexCount) {
    std::lock_guard lock(g_lock);
    D3DPRIMITIVETYPE type;
    UINT primitiveCount;
    bool quads;
    const std::uint8_t* vertices = nullptr;
    UINT stride = 0;
    if (!hostPrimitive(primitive, vertexCount, type, primitiveCount, quads) || !prepareDraw(startVertex, vertexCount, vertices, stride)) {
        return;
    }
    if (quads) {
        g_indexScratch.clear();
        for (UINT quad = 0; quad + 3 < vertexCount; quad += 4) {
            const WORD q = static_cast<WORD>(quad);
            g_indexScratch.insert(g_indexScratch.end(), {q, WORD(q + 1), WORD(q + 2), q, WORD(q + 2), WORD(q + 3)});
        }
        g_device->DrawIndexedPrimitiveUP(type, 0, vertexCount, primitiveCount, g_indexScratch.data(), D3DFMT_INDEX16, vertices, stride);
    } else {
        g_device->DrawPrimitiveUP(type, primitiveCount, vertices, stride);
    }
}

void __stdcall xDrawIndexedVertices(DWORD primitive, UINT indexCount, const WORD* indices) {
    std::lock_guard lock(g_lock);
    D3DPRIMITIVETYPE type;
    UINT primitiveCount;
    bool quads;
    if (indices == nullptr || !hostPrimitive(primitive, indexCount, type, primitiveCount, quads)) {
        return;
    }
    WORD minIndex = 0xFFFF;
    WORD maxIndex = 0;
    for (UINT index = 0; index < indexCount; ++index) {
        minIndex = std::min(minIndex, indices[index]);
        maxIndex = std::max(maxIndex, indices[index]);
    }
    g_indexScratch.clear();
    if (quads) {
        for (UINT quad = 0; quad + 3 < indexCount; quad += 4) {
            const WORD* q = indices + quad;
            g_indexScratch.insert(g_indexScratch.end(), {q[0], q[1], q[2], q[0], q[2], q[3]});
        }
    } else {
        g_indexScratch.assign(indices, indices + indexCount);
    }
    for (WORD& index : g_indexScratch) {
        index = static_cast<WORD>(index - minIndex);
    }

    const UINT vertexCount = static_cast<UINT>(maxIndex - minIndex) + 1;
    const std::uint8_t* vertices = nullptr;
    UINT stride = 0;
    if (!prepareDraw(g_baseVertexIndex + minIndex, vertexCount, vertices, stride)) {
        return;
    }
    g_device->DrawIndexedPrimitiveUP(type, 0, vertexCount, primitiveCount, g_indexScratch.data(), D3DFMT_INDEX16, vertices, stride);
}

void __stdcall xSetStreamSource(UINT stream, XResource* buffer, UINT stride) {
    if (stream < 16) {
        g_streams[stream] = {buffer, stride};
    }
}

void __stdcall xSetIndices(XResource* indexBuffer, UINT baseVertexIndex) {
    g_indexBuffer = indexBuffer;
    g_baseVertexIndex = baseVertexIndex;
    // Inlined game code builds index pointers from D3D's cached index-buffer base.
    *reinterpret_cast<DWORD*>(static_cast<std::uintptr_t>(0x002E4044)) = indexBuffer != nullptr ? indexBuffer->Data : 0;
}

void __stdcall xSetTexture(DWORD stage, XPixelContainer* texture) {
    if (stage < 4) {
        g_textures[stage] = texture;
    }
}

void __stdcall xSetVertexShader(DWORD handle) {
    g_vertexShader = handle;
}

HRESULT __stdcall xCreateVertexShader(const DWORD* declaration, const DWORD* function, DWORD* handle, DWORD usage) {
    auto* shader = new XVertexShader;
    if (declaration != nullptr) {
        for (const DWORD* token = declaration; *token != 0xFFFFFFFF; ++token) {
            shader->declaration.push_back(*token);
        }
    }
    if (function != nullptr) {
        const DWORD instructions = function[0] >> 16;
        shader->function.assign(function, function + 1 + instructions * 4);
    }
    *handle = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(shader)) | 1;
    return D3D_OK;
}

HRESULT __stdcall xDeleteVertexShader(DWORD handle) {
    return D3D_OK;
}

void __stdcall xGetVertexShader(DWORD* handle) {
    *handle = g_vertexShader;
}

void storeVertexConstants(int reg, const void* data, DWORD count) {
    // Callers pass the hardware slot (D3D register + 96) already.
    const int first = reg;
    for (DWORD index = 0; index < count; ++index) {
        if (first + static_cast<int>(index) >= 0 && first + static_cast<int>(index) < 192) {
            std::memcpy(g_vertexConstants[first + index], static_cast<const float*>(data) + index * 4, 16);
        }
    }
}

void __fastcall xSetVertexShaderConstant1(int reg, const void* data) {
    storeVertexConstants(reg, data, 1);
}

void __fastcall xSetVertexShaderConstant4(int reg, const void* data) {
    storeVertexConstants(reg, data, 4);
}

void __fastcall xSetVertexShaderConstantNotInline(int reg, const void* data, DWORD count) {
    storeVertexConstants(reg, data, count);
}

HRESULT __stdcall xCreatePixelShader(const DWORD* definition, DWORD* handle) {
    auto* shader = new XPixelShader{1, 0, new DWORD[60]};
    std::memcpy(shader->Definition, definition, 60 * sizeof(DWORD));
    *handle = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(shader));
    return D3D_OK;
}

HRESULT __stdcall xDeletePixelShader(DWORD handle) {
    return D3D_OK;
}

void __stdcall xSetPixelShader(DWORD handle) {
    g_pixelShader = handle;
    if (handle != 0) {
        // The real library mirrors the definition into the pixel-shader render-state slots.
        std::memcpy(g_rs, reinterpret_cast<XPixelShader*>(static_cast<std::uintptr_t>(handle))->Definition, 57 * sizeof(DWORD));
        g_rs[RS_PSTEXTUREMODES] = g_rs[54];
    }
}

void __stdcall xGetPixelShader(DWORD* handle) {
    *handle = g_pixelShader;
}

void __stdcall xSetPixelShaderConstant(DWORD reg, const void* data, DWORD count) {
    if (g_pixelShader == 0) {
        return;
    }
    // Constant registers reach the combiners through the definition's C0/C1/final mappings, as packed colors.
    const DWORD* definition = reinterpret_cast<const XPixelShader*>(static_cast<std::uintptr_t>(g_pixelShader))->Definition;
    for (DWORD index = 0; index < count; ++index) {
        const float* value = static_cast<const float*>(data) + index * 4;
        auto channel = [&](int component) { return static_cast<DWORD>(std::clamp(value[component], 0.0f, 1.0f) * 255.0f + 0.5f); };
        const DWORD color = (channel(3) << 24) | (channel(0) << 16) | (channel(1) << 8) | channel(2);
        const DWORD target = reg + index;
        for (int stage = 0; stage < 8; ++stage) {
            if (((definition[57] >> (stage * 4)) & 0xF) == target) g_rs[10 + stage] = color;
            if (((definition[58] >> (stage * 4)) & 0xF) == target) g_rs[18 + stage] = color;
        }
        if ((definition[59] & 0xF) == target) g_rs[43] = color;
        if (((definition[59] >> 4) & 0xF) == target) g_rs[44] = color;
    }
}

void __stdcall xSetTransform(DWORD state, const D3DMATRIX* matrix) {
    std::lock_guard lock(g_lock);
    if (state < 10) {
        g_transforms[state] = *matrix;
        g_device->SetTransform(convertTransform(state), matrix);
    }
}

void __stdcall xGetTransform(DWORD state, D3DMATRIX* matrix) {
    if (state < 10) {
        *matrix = g_transforms[state];
    }
}

void __stdcall xSetViewport(const D3DVIEWPORT9* viewport) {
    std::lock_guard lock(g_lock);
    g_viewport = *viewport;
    D3DVIEWPORT9 host = *viewport;
    D3DSURFACE_DESC description;
    IDirect3DSurface9* target = nullptr;
    if (SUCCEEDED(g_device->GetRenderTarget(0, &target))) {
        target->GetDesc(&description);
        target->Release();
        host.Width = std::min<DWORD>(host.Width, description.Width - std::min<DWORD>(host.X, description.Width));
        host.Height = std::min<DWORD>(host.Height, description.Height - std::min<DWORD>(host.Y, description.Height));
    }
    host.MinZ = std::clamp(host.MinZ, 0.0f, 1.0f);
    host.MaxZ = std::clamp(host.MaxZ, 0.0f, 1.0f);
    g_device->SetViewport(&host);
}

void __stdcall xGetViewport(D3DVIEWPORT9* viewport) {
    *viewport = g_viewport;
}

HRESULT __stdcall xLightEnable(DWORD index, BOOL enable) {
    std::lock_guard lock(g_lock);
    return g_device->LightEnable(index, enable);
}

// Xbox D3DLIGHT8/D3DMATERIAL8 share the Direct3D 9 layouts.
HRESULT __stdcall xSetLight(DWORD index, const D3DLIGHT9* light) {
    std::lock_guard lock(g_lock);
    return g_device->SetLight(index, light);
}

HRESULT __stdcall xSetMaterial(const D3DMATERIAL9* material) {
    std::lock_guard lock(g_lock);
    return g_device->SetMaterial(material);
}

HRESULT __stdcall xReset(XPresentParameters* parameters) {
    if (parameters != nullptr) {
        parameters->BufferSurfaces[0] = &g_backBuffer;
        parameters->DepthStencilSurface = &g_depthBuffer;
    }
    return D3D_OK;
}

void __stdcall xPersistDisplay() {
}

void __stdcall xSetBumpEnv(DWORD stage, DWORD type, DWORD value) {
    std::lock_guard lock(g_lock);
    static constexpr D3DTEXTURESTAGESTATETYPE kTypes[] = {D3DTSS_BUMPENVMAT00, D3DTSS_BUMPENVMAT01, D3DTSS_BUMPENVMAT11,
        D3DTSS_BUMPENVMAT10, D3DTSS_BUMPENVLSCALE, D3DTSS_BUMPENVLOFFSET};
    if (stage < 4 && type >= 22 && type <= 27) {
        g_tss[stage * kStageSize + type] = value;
        g_device->SetTextureStageState(stage, kTypes[type - 22], value);
    }
}

void __stdcall xSetColorKeyColor(DWORD stage, DWORD value) {
}

template <int Index>
void __stdcall setComplexRenderState(DWORD value) {
    g_rs[Index] = value;
}

void __fastcall xSetRenderStateSimple(DWORD method, DWORD value) {
}

void __stdcall xSetTexCoordIndex(DWORD stage, DWORD value) {
    if (stage < 4) {
        g_texCoordIndex[stage] = value;
    }
}

void __stdcall xSetBorderColor(DWORD stage, DWORD value) {
    if (stage < 4) {
        g_borderColor[stage] = value;
    }
}

// ---------------------------------------------------------------- resources

XResource* createBuffer(DWORD type, UINT length) {
    auto* buffer = new XResource{1 | type | kCommonD3DCreated, static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(allocate(length))), 0};
    return buffer;
}

XResource* __stdcall xCreateVertexBuffer2(UINT length) {
    return createBuffer(kCommonTypeVertexBuffer, length);
}

XResource* __stdcall xCreateIndexBuffer2(UINT length) {
    return createBuffer(kCommonTypeIndexBuffer, length);
}

std::uint8_t* __stdcall xVertexBufferLock2(XResource* buffer, DWORD flags) {
    return xboxPointer(buffer->Data);
}

ULONG __stdcall xResourceAddRef(XResource* resource) {
    if ((resource->Common & kCommonRefCountMask) < kCommonRefCountMask) {
        ++resource->Common;
    }
    return resource->Common & kCommonRefCountMask;
}

ULONG __stdcall xResourceRelease(XResource* resource) {
    if ((resource->Common & kCommonRefCountMask) > 0) {
        --resource->Common;
    }
    return resource->Common & kCommonRefCountMask;
}

BOOL __stdcall xResourceIsBusy(XResource* resource) {
    return FALSE;
}

void __stdcall xResourceBlockUntilNotBusy(XResource* resource) {
}

void __stdcall xResourceRegister(XResource* resource, void* base) {
    resource->Data += static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(base));
}

void* __stdcall xAllocContiguousMemory(SIZE_T size, DWORD alignment) {
    return allocate(size);
}

void __stdcall xSetPushBufferSize(DWORD size, DWORD kickOff) {
}

XSurface* newContainer(UINT width, UINT height, UINT depth, UINT levels, DWORD format, bool cube, bool volume, DWORD commonType) {
    auto* container = new XSurface{};
    const bool linear = isLinearFormat(format);
    if (levels == 0) {
        levels = linear ? 1 : log2Of(std::max({width, height, depth})) + 1;
    }
    container->Format = kFormatDmaA | ((volume ? 3u : 2u) << 4) | (format << 8) | (levels << 16) | (cube ? kFormatCubeMap : 0);
    if (linear) {
        const UINT pitch = (width * bytesPerPixelOf(format) + 63) & ~63u;
        container->Size = (width - 1) | ((height - 1) << 12) | (((pitch / 64) - 1) << 24);
    } else {
        container->Format |= (log2Of(width) << 20) | (log2Of(height) << 24) | (volume ? log2Of(depth) << 28 : 0);
    }
    const TextureLayout layout = describe(container);
    const std::size_t size = cube ? static_cast<std::size_t>(faceSize(layout)) * 6 : levelOffset(layout, layout.levels);
    container->Data = static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(allocate(size)));
    container->Common = 1 | commonType | kCommonD3DCreated;
    return container;
}

IDirect3DTexture9* hostRenderTargetFor(const XPixelContainer* container) {
    auto existing = g_hostRenderTargets.find(container);
    if (existing != g_hostRenderTargets.end()) {
        return existing->second;
    }
    const TextureLayout layout = describe(container);
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(g_device->CreateTexture(layout.width, layout.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, nullptr))) {
        logf("d3d: could not create %ux%u render target", layout.width, layout.height);
        return nullptr;
    }
    g_hostRenderTargets[container] = texture;
    setRenderTargetOverride(container, texture);
    return texture;
}

XPixelContainer* __stdcall xCreateTexture2(UINT width, UINT height, UINT depth, UINT levels, DWORD usage, DWORD format, DWORD resourceType) {
    std::lock_guard lock(g_lock);
    const bool cube = resourceType == 5;
    const bool volume = resourceType == 4;
    const DWORD commonType = resourceType == 1 ? kCommonTypeSurface : kCommonTypeTexture;
    XSurface* container = newContainer(width, height, std::max(1u, depth), levels, format, cube, volume, commonType);
    if (usage & D3DUSAGE_RENDERTARGET) {
        hostRenderTargetFor(container);
    }
    return container;
}

HRESULT __stdcall xCreateStandAloneSurface(UINT width, UINT height, DWORD format, XSurface** surface) {
    std::lock_guard lock(g_lock);
    *surface = newContainer(width, height, 1, 1, format, false, false, kCommonTypeSurface);
    return D3D_OK;
}

XSurface* surfaceForLevel(XPixelContainer* texture, UINT face, UINT level) {
    std::lock_guard lock(g_lock);
    const std::pair<const XPixelContainer*, UINT> key{texture, face * 16 + level};
    auto existing = g_surfaceLevels.find(key);
    if (existing != g_surfaceLevels.end()) {
        return existing->second;
    }
    const TextureLayout layout = describe(texture);
    auto* surface = new XSurface{};
    surface->Common = 1 | kCommonTypeSurface;
    surface->Data = texture->Data + (layout.cube ? face * faceSize(layout) : 0) + levelOffset(layout, level);
    surface->Format = (texture->Format & 0x0000FFFF & ~kFormatCubeMap) | (1u << 16);
    if (!layout.linear) {
        surface->Format |= (log2Of(levelWidth(layout, level)) << 20) | (log2Of(levelHeight(layout, level)) << 24);
    }
    surface->Size = texture->Size;
    surface->Parent = texture;
    g_surfaceLevels[key] = surface;
    return surface;
}

XSurface* __stdcall xTextureGetSurfaceLevel2(XPixelContainer* texture, UINT level) {
    return surfaceForLevel(texture, 0, level);
}

XSurface* __stdcall xCubeTextureGetCubeMapSurface2(XPixelContainer* texture, DWORD face, UINT level) {
    return surfaceForLevel(texture, face, level);
}

void fillLockedRect(const XPixelContainer* container, std::uint8_t* base, UINT level, D3DLOCKED_RECT* locked, const RECT* rect) {
    const TextureLayout layout = describe(container);
    const UINT pitch = levelPitch(layout, level);
    if (rect != nullptr && layout.linear) {
        base += static_cast<std::size_t>(rect->top) * pitch + static_cast<std::size_t>(rect->left) * layout.bytesPerPixel;
    }
    locked->Pitch = static_cast<INT>(pitch);
    locked->pBits = base;
}

HRESULT __stdcall xTextureLockRect(XPixelContainer* texture, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    const TextureLayout layout = describe(texture);
    fillLockedRect(texture, xboxPointer(texture->Data) + levelOffset(layout, level), level, locked, rect);
    return D3D_OK;
}

HRESULT __stdcall xCubeTextureLockRect(XPixelContainer* texture, DWORD face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    const TextureLayout layout = describe(texture);
    fillLockedRect(texture, xboxPointer(texture->Data) + face * faceSize(layout) + levelOffset(layout, level), level, locked, rect);
    return D3D_OK;
}

HRESULT __stdcall xSurfaceLockRect(XSurface* surface, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) {
    fillLockedRect(surface, xboxPointer(surface->Data), 0, locked, rect);
    return D3D_OK;
}

HRESULT __stdcall xVolumeTextureLockBox(XPixelContainer* texture, UINT level, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
    const TextureLayout layout = describe(texture);
    locked->RowPitch = static_cast<INT>(levelPitch(layout, level));
    locked->SlicePitch = static_cast<INT>(levelPitch(layout, level) * levelHeight(layout, level));
    locked->pBits = xboxPointer(texture->Data) + levelOffset(layout, level);
    return D3D_OK;
}

HRESULT __stdcall xVolumeLockBox(XPixelContainer* volume, D3DLOCKED_BOX* locked, const D3DBOX* box, DWORD flags) {
    return xVolumeTextureLockBox(volume, 0, locked, box, flags);
}

void fillSurfaceDesc(const XPixelContainer* container, UINT level, XSurfaceDesc* description) {
    const TextureLayout layout = describe(container);
    description->Format = layout.format;
    description->Type = (container->Common & 0x00070000) == kCommonTypeSurface ? 1 : 3;
    description->Usage = 0;
    description->Size = levelSize(layout, level);
    description->MultiSampleType = 0x0011;
    description->Width = levelWidth(layout, level);
    description->Height = levelHeight(layout, level);
}

void __stdcall xGet2DSurfaceDesc(XPixelContainer* container, UINT level, XSurfaceDesc* description) {
    fillSurfaceDesc(container, level, description);
}

HRESULT __stdcall xSurfaceGetDesc(XSurface* surface, XSurfaceDesc* description) {
    fillSurfaceDesc(surface, 0, description);
    return D3D_OK;
}

XSurface* __stdcall xGetBackBuffer2(INT backBuffer) {
    return &g_backBuffer;
}

XSurface* __stdcall xGetDepthStencilSurface2() {
    return &g_depthBuffer;
}

void __stdcall xSetRenderTarget(XPixelContainer* renderTarget, XPixelContainer* depthStencil) {
    std::lock_guard lock(g_lock);
    if (renderTarget == nullptr) {
        return;
    }
    g_renderTarget = renderTarget;
    if (renderTarget == &g_backBuffer) {
        g_device->SetRenderTarget(0, g_hostBackBuffer);
    } else {
        const XPixelContainer* owner = static_cast<XSurface*>(renderTarget)->Parent != nullptr ? static_cast<XSurface*>(renderTarget)->Parent : renderTarget;
        IDirect3DTexture9* texture = hostRenderTargetFor(owner);
        IDirect3DSurface9* surface = nullptr;
        if (texture != nullptr && SUCCEEDED(texture->GetSurfaceLevel(0, &surface))) {
            g_device->SetRenderTarget(0, surface);
            surface->Release();
        }
    }
    g_device->SetDepthStencilSurface(depthStencil != nullptr ? g_hostDepth : nullptr);
}

ULONG __stdcall xDeviceRelease() {
    return 1;
}

void __stdcall xGetDeviceCaps(void* caps) {
    D3DCAPS9 hostCaps{};
    g_d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &hostCaps);
    hostCaps.VertexShaderVersion = D3DVS_VERSION(1, 1);
    hostCaps.PixelShaderVersion = D3DPS_VERSION(1, 1);
    hostCaps.MaxTextureWidth = std::min<DWORD>(hostCaps.MaxTextureWidth, 4096);
    hostCaps.MaxTextureHeight = std::min<DWORD>(hostCaps.MaxTextureHeight, 4096);
    hostCaps.MaxSimultaneousTextures = 4;
    hostCaps.MaxTextureBlendStages = 8;
    std::memcpy(caps, &hostCaps, offsetof(D3DCAPS9, DevCaps2));
}

void __stdcall xGetDisplayMode(XDisplayMode* mode) {
    *mode = {g_width, g_height, 60, 0, kFmtLinX8R8G8B8};
}

void __stdcall xGetDisplayFieldStatus(DWORD* status) {
    status[0] = 3;
    status[1] = g_frame;
}

void __stdcall xBeginVisibilityTest() {
}

HRESULT __stdcall xEndVisibilityTest(DWORD index) {
    return D3D_OK;
}

HRESULT __stdcall xGetVisibilityTestResult(DWORD index, UINT* result, ULONGLONG* timeStamp) {
    if (result != nullptr) {
        *result = 640 * 480;
    }
    if (timeStamp != nullptr) {
        *timeStamp = g_frame;
    }
    return D3D_OK;
}

void __stdcall xCopyRects(XSurface* source, const RECT* sourceRects, UINT count, XSurface* destination, const POINT* points) {
    static bool reported = false;
    if (!reported) {
        reported = true;
        logf("d3d: CopyRects is not implemented yet");
    }
}

// The NV2A video overlay only displays XMV movies, which are skipped.
void __stdcall xEnableOverlay(BOOL enable) {
}

void __stdcall xUpdateOverlay(XSurface* surface, const RECT* sourceRect, const RECT* destinationRect, BOOL enableColorKey, D3DCOLOR colorKey) {
}

} // namespace

HWND gameWindow() {
    return g_window;
}

} // namespace cw::d3d

namespace cw::hle {

void installD3DHooks() {
    using namespace cw::d3d;
    auto hook = [](std::uint32_t address, const void* function, const char* name) { hookFunction(address, function, name); };
#define CW_HOOK(address, function) hook(address, reinterpret_cast<const void*>(&function), #function)
#define CW_RS(address, index) hook(address, reinterpret_cast<const void*>(&setComplexRenderState<index>), "SetRenderState[" #index "]")

    CW_HOOK(0x002DFD50, xDirect3D_CreateDevice);
    CW_HOOK(0x002E0870, xSwap);
    CW_HOOK(0x002DC250, xClear);
    CW_HOOK(0x002E0A20, xDrawVertices);
    CW_HOOK(0x002E0AC0, xDrawIndexedVertices);
    CW_HOOK(0x002D82B0, xSetStreamSource);
    CW_HOOK(0x002D7190, xSetIndices);
    CW_HOOK(0x002D6FE0, xSetTexture);
    CW_HOOK(0x002D8690, xSetVertexShader);
    CW_HOOK(0x002D7F20, xCreateVertexShader);
    CW_HOOK(0x002D8580, xDeleteVertexShader);
    CW_HOOK(0x002D8060, xGetVertexShader);
    CW_HOOK(0x002D8080, xSetVertexShaderConstant1);
    CW_HOOK(0x002D80E0, xSetVertexShaderConstant4);
    CW_HOOK(0x002D8270, xSetVertexShaderConstantNotInline);
    CW_HOOK(0x002E0DE0, xCreatePixelShader);
    CW_HOOK(0x002E0E30, xDeletePixelShader);
    CW_HOOK(0x002E0E50, xGetPixelShader);
    CW_HOOK(0x002E0E70, xSetPixelShader);
    CW_HOOK(0x002E1070, xSetPixelShaderConstant);
    CW_HOOK(0x002D62E0, xSetTransform);
    CW_HOOK(0x002D63F0, xGetTransform);
    CW_HOOK(0x002D6BB0, xSetViewport);
    CW_HOOK(0x002D6420, xGetViewport);
    CW_HOOK(0x002D6EF0, xLightEnable);
    CW_HOOK(0x002D6D10, xSetLight);
    CW_HOOK(0x002D6440, xSetMaterial);
    CW_HOOK(0x002D6760, xReset);
    CW_HOOK(0x002D75F0, xPersistDisplay);
    CW_HOOK(0x002D4060, xSetBumpEnv);
    CW_HOOK(0x002D4110, xSetColorKeyColor);
    CW_HOOK(0x002D3690, xSetRenderStateSimple);
    CW_HOOK(0x002D3F50, xSetTexCoordIndex);
    CW_HOOK(0x002D40D0, xSetBorderColor);
    CW_HOOK(0x002DBE40, xCreateVertexBuffer2);
    CW_HOOK(0x002DBE00, xCreateIndexBuffer2);
    CW_HOOK(0x002DBE90, xVertexBufferLock2);
    CW_HOOK(0x002D9450, xResourceAddRef);
    CW_HOOK(0x002D9490, xResourceRelease);
    CW_HOOK(0x002D9570, xResourceIsBusy);
    CW_HOOK(0x002D9630, xResourceBlockUntilNotBusy);
    CW_HOOK(0x002D9600, xResourceRegister);
    CW_HOOK(0x002E1340, xAllocContiguousMemory);
    CW_HOOK(0x002DFD30, xSetPushBufferSize);
    CW_HOOK(0x002E1370, xCreateTexture2);
    CW_HOOK(0x002DDDB0, xCreateStandAloneSurface);
    CW_HOOK(0x002E1440, xTextureGetSurfaceLevel2);
    CW_HOOK(0x002E14D0, xCubeTextureGetCubeMapSurface2);
    CW_HOOK(0x002E1490, xTextureLockRect);
    CW_HOOK(0x002E1520, xCubeTextureLockRect);
    CW_HOOK(0x002DDEF0, xSurfaceLockRect);
    CW_HOOK(0x002E1590, xVolumeTextureLockBox);
    CW_HOOK(0x002DDF80, xVolumeLockBox);
    CW_HOOK(0x002E14C0, xGet2DSurfaceDesc);
    CW_HOOK(0x002DDED0, xSurfaceGetDesc);
    CW_HOOK(0x002D6810, xGetBackBuffer2);
    CW_HOOK(0x002D6B90, xGetDepthStencilSurface2);
    CW_HOOK(0x002D62C0, xSetRenderTarget);
    CW_HOOK(0x002D6470, xDeviceRelease);
    CW_HOOK(0x002D62A0, xGetDeviceCaps);
    CW_HOOK(0x002D6690, xGetDisplayMode);
    CW_HOOK(0x002D7350, xGetDisplayFieldStatus);
    CW_HOOK(0x002D7250, xBeginVisibilityTest);
    CW_HOOK(0x002D72F0, xEndVisibilityTest);
    CW_HOOK(0x002D64C0, xGetVisibilityTestResult);
    CW_HOOK(0x002D6860, xCopyRects);
    CW_HOOK(0x002E1720, xEnableOverlay);
    CW_HOOK(0x002E15A0, xUpdateOverlay);

    CW_RS(0x002D3660, RS_PSTEXTUREMODES);
    CW_RS(0x002D39F0, RS_EDGEANTIALIAS);
    CW_RS(0x002D3A30, RS_SHADOWFUNC);
    CW_RS(0x002D3A70, RS_FOGCOLOR);
    CW_RS(0x002D3AC0, RS_CULLMODE);
    CW_RS(0x002D3B30, RS_FRONTFACE);
    CW_RS(0x002D3B70, RS_NORMALIZENORMALS);
    CW_RS(0x002D3BB0, RS_TEXTUREFACTOR);
    CW_RS(0x002D3C10, RS_LINEWIDTH);
    CW_RS(0x002D3C80, RS_DXT1NOISEENABLE);
    CW_RS(0x002D3D00, RS_ZBIAS);
    CW_RS(0x002D3D80, RS_LOGICOP);
    CW_RS(0x002D3DE0, RS_FILLMODE);
    CW_RS(0x002D3E30, RS_BACKFILLMODE);
    CW_RS(0x002D3E90, RS_TWOSIDEDLIGHTING);
    CW_RS(0x002D3F00, RS_VERTEXBLEND);
    CW_RS(0x002D4B50, RS_ZENABLE);
    CW_RS(0x002D4BE0, RS_STENCILENABLE);
    CW_RS(0x002D4C70, RS_STENCILFAIL);
    CW_RS(0x002D4CE0, RS_YUVENABLE);
    CW_RS(0x002D4D10, RS_OCCLUSIONCULLENABLE);
    CW_RS(0x002D4D80, RS_STENCILCULLENABLE);
    CW_RS(0x002D4DF0, RS_ROPZCMPALWAYSREAD);
    CW_RS(0x002D4E10, RS_ROPZREAD);
    CW_RS(0x002D4E30, RS_DONOTCULLUNCOMPRESSED);
    CW_RS(0x002D4E50, RS_MULTISAMPLEMODE);
    CW_RS(0x002D4E90, RS_MULTISAMPLERENDERTARGETMODE);
    CW_RS(0x002D4ED0, RS_MULTISAMPLEANTIALIAS);
    CW_RS(0x002D4F50, RS_MULTISAMPLEMASK);
    CW_RS(0x002D4FA0, RS_SAMPLEALPHA);
#undef CW_HOOK
#undef CW_RS
}

} // namespace cw::hle
