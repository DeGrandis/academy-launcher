#include "d3d/Device11.h"

#include "d3d/D3D9Stubs.h"
#include "d3d/FixedFunction11.h"
#include "d3d/FxTuner.h"
#include "d3d/PostShaders11.h"
#include "d3d/XboxD3D.h"

#include "Log.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace cw::options {
extern char g_missionName[32];  // Device.cpp: the mission being played (shell.wld in the menus)
}

namespace cw::d3d {

HRESULT stubCalled(const char* method) {
    static std::mutex lock;
    static std::set<std::string> reported;
    std::lock_guard guard(lock);
    if (reported.insert(method).second) {
        logf("d3d11: %s is not implemented", method);
    }
    return E_NOTIMPL;
}

namespace {

template <typename T>
void release(T*& pointer) {
    if (pointer != nullptr) {
        pointer->Release();
        pointer = nullptr;
    }
}

ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11DeviceContext1* g_ctx1 = nullptr;

// ---------------------------------------------------------------- formats

DXGI_FORMAT dxgiFormat(D3DFORMAT format) {
    switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case D3DFMT_DXT1: return DXGI_FORMAT_BC1_UNORM;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3: return DXGI_FORMAT_BC2_UNORM;
    case D3DFMT_DXT4:
    case D3DFMT_DXT5: return DXGI_FORMAT_BC3_UNORM;
    case D3DFMT_D24S8:
    case D3DFMT_D24X8: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

bool isBlockFormat(D3DFORMAT format) {
    return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 || format == D3DFMT_DXT3 || format == D3DFMT_DXT4 || format == D3DFMT_DXT5;
}

UINT levelPitchOf(D3DFORMAT format, UINT width) {
    if (isBlockFormat(format)) {
        return std::max(1u, (width + 3) / 4) * (format == D3DFMT_DXT1 ? 8 : 16);
    }
    return width * 4;
}

UINT levelRowsOf(D3DFORMAT format, UINT height) {
    return isBlockFormat(format) ? std::max(1u, (height + 3) / 4) : height;
}

// ---------------------------------------------------------------- resources

struct ShaderResource11 {
    ID3D11ShaderResourceView* srv = nullptr;
    bool cube = false;
    float average[4] = {1, 1, 1, 1};  // mean color of the top level (effect lights take their color from it)
    virtual ~ShaderResource11() { release(srv); }
};

struct Surface11 : StubDirect3DSurface9 {
    enum class Kind { Target, Depth, System } kind = Kind::Target;
    UINT width = 0;
    UINT height = 0;
    D3DFORMAT format = D3DFMT_A8R8G8B8;
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    std::vector<std::uint8_t> memory;  // System surfaces
    UINT pitch = 0;
    bool borrowed = false;  // texture and views belong to the device (the scene surface)

    ~Surface11() override {
        if (borrowed) {
            return;
        }
        release(rtv);
        release(dsv);
        release(texture);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC* description) override {
        *description = {};
        description->Format = format;
        description->Type = D3DRTYPE_SURFACE;
        description->Usage = kind == Kind::Target ? D3DUSAGE_RENDERTARGET : kind == Kind::Depth ? D3DUSAGE_DEPTHSTENCIL : 0;
        description->Pool = kind == Kind::System ? D3DPOOL_SYSTEMMEM : D3DPOOL_DEFAULT;
        description->Width = width;
        description->Height = height;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override {
        if (kind != Kind::System) {
            return D3DERR_INVALIDCALL;
        }
        locked->Pitch = static_cast<INT>(pitch);
        locked->pBits = memory.data() + (rect != nullptr ? static_cast<std::size_t>(rect->top) * pitch + rect->left * 4 : 0);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect() override {
        return D3D_OK;
    }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override {
        return D3DRTYPE_SURFACE;
    }
};

Surface11* createTargetSurface(ID3D11Texture2D* texture, UINT width, UINT height) {
    auto* surface = new Surface11;
    surface->kind = Surface11::Kind::Target;
    surface->width = width;
    surface->height = height;
    surface->texture = texture;
    texture->AddRef();
    g_dev->CreateRenderTargetView(texture, nullptr, &surface->rtv);
    return surface;
}

Surface11* createDepthSurface(UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    auto* surface = new Surface11;
    surface->kind = Surface11::Kind::Depth;
    surface->width = width;
    surface->height = height;
    surface->format = D3DFMT_D24S8;
    if (FAILED(g_dev->CreateTexture2D(&desc, nullptr, &surface->texture)) ||
        FAILED(g_dev->CreateDepthStencilView(surface->texture, nullptr, &surface->dsv))) {
        logf("d3d11: could not create a %ux%u depth buffer", width, height);
    }
    return surface;
}

// Mean color of a top level as uploaded: B8G8R8A8 texels, or the endpoint colors of DXT blocks.
void measureAverage(const std::vector<std::uint8_t>& data, D3DFORMAT format, float out[4]) {
    double sum[4] = {};
    std::size_t count = 0;
    if (isBlockFormat(format)) {
        const std::size_t block = format == D3DFMT_DXT1 ? 8 : 16;
        const std::size_t colorAt = format == D3DFMT_DXT1 ? 0 : 8;
        for (std::size_t at = 0; at + block <= data.size(); at += block) {
            for (int endpoint = 0; endpoint < 2; ++endpoint) {
                const std::uint16_t c = static_cast<std::uint16_t>(data[at + colorAt + endpoint * 2] | (data[at + colorAt + endpoint * 2 + 1] << 8));
                sum[0] += ((c >> 11) & 31) / 31.0;
                sum[1] += ((c >> 5) & 63) / 63.0;
                sum[2] += (c & 31) / 31.0;
            }
            double alpha = 1;
            if (format == D3DFMT_DXT2 || format == D3DFMT_DXT3) {
                alpha = 0;
                for (int k = 0; k < 8; ++k) alpha += ((data[at + k] & 15) + (data[at + k] >> 4)) / 30.0;
                alpha /= 8;
            } else if (format == D3DFMT_DXT4 || format == D3DFMT_DXT5) {
                alpha = (data[at] + data[at + 1]) / 510.0;
            }
            sum[3] += alpha * 2;
            count += 2;
        }
    } else {
        for (std::size_t at = 0; at + 4 <= data.size(); at += 4 * 7) {  // every 7th texel is plenty
            sum[2] += data[at] / 255.0;
            sum[1] += data[at + 1] / 255.0;
            sum[0] += data[at + 2] / 255.0;
            sum[3] += data[at + 3] / 255.0;
            ++count;
        }
    }
    for (int k = 0; k < 4; ++k) {
        out[k] = count > 0 ? static_cast<float>(sum[k] / count) : 1.0f;
    }
}

struct Texture11 : StubDirect3DTexture9, ShaderResource11 {
    UINT width = 0;
    UINT height = 0;
    UINT levels = 1;
    D3DFORMAT format = D3DFMT_A8R8G8B8;
    bool renderTarget = false;
    ID3D11Texture2D* texture = nullptr;
    std::vector<std::vector<std::uint8_t>> staging;
    Surface11* surface = nullptr;  // render targets: level 0

    ~Texture11() override {
        if (surface != nullptr) surface->Release();
        release(texture);
    }
    UINT levelWidth(UINT level) const { return std::max(1u, width >> level); }
    UINT levelHeight(UINT level) const { return std::max(1u, height >> level); }

    DWORD STDMETHODCALLTYPE GetLevelCount() override { return levels; }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_TEXTURE; }
    HRESULT STDMETHODCALLTYPE GetLevelDesc(UINT level, D3DSURFACE_DESC* description) override {
        *description = {};
        description->Format = format;
        description->Type = D3DRTYPE_SURFACE;
        description->Usage = renderTarget ? D3DUSAGE_RENDERTARGET : 0;
        description->Width = levelWidth(level);
        description->Height = levelHeight(level);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSurfaceLevel(UINT level, IDirect3DSurface9** result) override {
        if (!renderTarget || level != 0 || surface == nullptr) {
            return stubCalled("GetSurfaceLevel (not a render target)");
        }
        surface->AddRef();
        *result = surface;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LockRect(UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override {
        if (level >= levels) {
            return D3DERR_INVALIDCALL;
        }
        staging.resize(levels);
        const UINT pitch = levelPitchOf(format, levelWidth(level));
        staging[level].resize(static_cast<std::size_t>(pitch) * levelRowsOf(format, levelHeight(level)));
        locked->Pitch = static_cast<INT>(pitch);
        locked->pBits = staging[level].data();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(UINT level) override {
        if (level == 0 && !staging.empty() && !staging[0].empty()) {
            measureAverage(staging[0], format, average);
        }
        if (level < staging.size() && !staging[level].empty()) {
            g_ctx->UpdateSubresource(texture, level, nullptr, staging[level].data(), levelPitchOf(format, levelWidth(level)), 0);
            std::vector<std::uint8_t>().swap(staging[level]);
        }
        return D3D_OK;
    }
};

struct CubeTexture11 : StubDirect3DCubeTexture9, ShaderResource11 {
    UINT edge = 0;
    UINT levels = 1;
    D3DFORMAT format = D3DFMT_A8R8G8B8;
    ID3D11Texture2D* texture = nullptr;
    std::vector<std::uint8_t> staging;

    ~CubeTexture11() override { release(texture); }
    DWORD STDMETHODCALLTYPE GetLevelCount() override { return levels; }
    D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_CUBETEXTURE; }
    HRESULT STDMETHODCALLTYPE LockRect(D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT* locked, const RECT* rect, DWORD flags) override {
        const UINT size = std::max(1u, edge >> level);
        const UINT pitch = levelPitchOf(format, size);
        staging.assign(static_cast<std::size_t>(pitch) * levelRowsOf(format, size), 0);
        locked->Pitch = static_cast<INT>(pitch);
        locked->pBits = staging.data();
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE UnlockRect(D3DCUBEMAP_FACES face, UINT level) override {
        const UINT size = std::max(1u, edge >> level);
        g_ctx->UpdateSubresource(texture, D3D11CalcSubresource(level, static_cast<UINT>(face), levels), nullptr, staging.data(),
            levelPitchOf(format, size), 0);
        return D3D_OK;
    }
};

struct VertexShader11 : StubDirect3DVertexShader9 {
    ID3D11VertexShader* shader = nullptr;
    std::vector<std::uint8_t> bytecode;
    std::string source;
    ID3D11VertexShader* shadowShader = nullptr;  // the sun's shadow map variant (compiled on first use)
    bool shadowTried = false;
    ~VertexShader11() override {
        release(shader);
        release(shadowShader);
    }
};

struct PixelShader11 : StubDirect3DPixelShader9 {
    ID3D11PixelShader* shader = nullptr;
    ~PixelShader11() override { release(shader); }
};

struct VertexDeclaration11 : StubDirect3DVertexDeclaration9 {
    std::vector<D3DVERTEXELEMENT9> elements;
    std::map<const VertexShader11*, ID3D11InputLayout*> layouts;
    ~VertexDeclaration11() override {
        for (auto& [shader, layout] : layouts) {
            layout->Release();
        }
    }
};

// ---------------------------------------------------------------- shader compilation

ID3DBlob* compile(const std::string& source, const char* name, const char* profile) {
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(source.data(), source.size(), name, nullptr, nullptr, "main", profile,
        D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY | D3DCOMPILE_OPTIMIZATION_LEVEL2, 0, &code, &errors);
    if (FAILED(result)) {
        logf("d3d11: %s compile failed: %s", name, errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        logf("%s", source.c_str());
        release(code);
    }
    release(errors);
    return code;
}

VertexShader11* makeVertexShader(const std::string& source, const char* name) {
    ID3DBlob* code = compile(source, name, "vs_5_0");
    if (code == nullptr) {
        return nullptr;
    }
    auto* shader = new VertexShader11;
    shader->source = source;
    const auto* bytes = static_cast<const std::uint8_t*>(code->GetBufferPointer());
    shader->bytecode.assign(bytes, bytes + code->GetBufferSize());
    g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader->shader);
    code->Release();
    return shader;
}

PixelShader11* makePixelShader(const std::string& source, const char* name) {
    ID3DBlob* code = compile(source, name, "ps_5_0");
    if (code == nullptr) {
        return nullptr;
    }
    auto* shader = new PixelShader11;
    g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader->shader);
    code->Release();
    return shader;
}

bool replaceOnce(std::string& text, const std::string& from, const std::string& to) {
    const std::size_t at = text.find(from);
    if (at == std::string::npos) {
        return false;
    }
    text.replace(at, from.size(), to);
    return true;
}

// ---------------------------------------------------------------- the device

constexpr UINT kVertexBufferSize = 64 << 20;  // a whole frame, so the shadow pass can draw it again
constexpr UINT kIndexBufferSize = 8 << 20;
constexpr UINT kXboxVertexConstants = 199;  // c0-c191, viewport scale/offset, texture scales, fog (VertexShader.cpp)
constexpr UINT kXboxPixelConstants = 27;

std::string keyBytes(const void* data, std::size_t size) {
    return std::string(static_cast<const char*>(data), size);
}

void copyMatrix(float out[16], const D3DMATRIX& matrix) {
    std::memcpy(out, &matrix, 64);
}

D3DMATRIX multiply(const D3DMATRIX& a, const D3DMATRIX& b) {
    D3DMATRIX result{};
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            float sum = 0;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[row][k] * b.m[k][column];
            }
            result.m[row][column] = sum;
        }
    }
    return result;
}

void unpackColor(DWORD color, float out[4]) {
    out[0] = ((color >> 16) & 0xFF) / 255.0f;
    out[1] = ((color >> 8) & 0xFF) / 255.0f;
    out[2] = (color & 0xFF) / 255.0f;
    out[3] = (color >> 24) / 255.0f;
}

float asFloat(DWORD value) {
    float result;
    std::memcpy(&result, &value, 4);
    return result;
}

D3D11_BLEND convertBlend(DWORD blend, bool alpha) {
    switch (blend) {
    case D3DBLEND_ZERO: return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE: return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT: return D3D11_BLEND_SRC_ALPHA_SAT;
    case D3DBLEND_BLENDFACTOR: return D3D11_BLEND_BLEND_FACTOR;
    case D3DBLEND_INVBLENDFACTOR: return D3D11_BLEND_INV_BLEND_FACTOR;
    default: return D3D11_BLEND_ONE;
    }
}

class Device11 : public StubDirect3DDevice9 {
public:
    HWND window = nullptr;
    bool vsync = true;
    IDXGISwapChain1* swapChain = nullptr;
    bool tearing = false;
    UINT width = 0;
    UINT height = 0;
    Surface11* scene = nullptr;        // what the game calls the back buffer
    ID3D11Texture2D* sceneTexture = nullptr;
    Surface11* depth = nullptr;
    std::map<std::pair<UINT, UINT>, Surface11*> sizedDepth;  // depth buffers for render targets of other sizes

    // Direct3D 9 state.
    DWORD rs[256] = {};
    DWORD tss[4][33] = {};
    DWORD sampler[4][14] = {};
    D3DMATRIX world{}, view{}, projection{}, textureMatrix[4]{};
    D3DLIGHT9 lights[8] = {};
    BOOL lightEnabled[8] = {};
    D3DMATERIAL9 material{};
    DWORD fvf = 0;
    VertexDeclaration11* declaration = nullptr;
    VertexShader11* vertexShader = nullptr;
    PixelShader11* pixelShader = nullptr;
    float vsConstants[256][4] = {};
    float psConstants[32][4] = {};
    IDirect3DBaseTexture9* textures[4] = {};
    D3DVIEWPORT9 viewport{0, 0, 640, 480, 0, 1};
    Surface11* renderTarget = nullptr;
    Surface11* depthStencil = nullptr;

    // Direct3D 11 objects.
    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* indexBuffer = nullptr;
    UINT vertexOffset = 0;
    UINT indexOffset = 0;
    ID3D11Buffer* vsConstantBuffer = nullptr;
    ID3D11Buffer* xboxPsConstantBuffer = nullptr;
    ID3D11Buffer* pixelCommonBuffer = nullptr;
    std::unordered_map<std::string, VertexShader11*> ffVertexShaders;
    std::unordered_map<std::string, PixelShader11*> ffPixelShaders;
    std::unordered_map<std::string, ID3D11InputLayout*> fvfLayouts;
    std::unordered_map<std::string, ID3D11BlendState*> blendStates;
    std::unordered_map<std::string, ID3D11DepthStencilState*> depthStates;
    std::unordered_map<std::string, ID3D11RasterizerState*> rasterStates;
    std::unordered_map<std::string, ID3D11SamplerState*> samplerStates;
    ID3D11VertexShader* clearVertexShader = nullptr;
    ID3D11PixelShader* clearPixelShader = nullptr;
    ID3D11Buffer* clearConstants = nullptr;
    ID3D11Texture2D* readback = nullptr;
    UINT drawCount = 0;
    std::vector<std::uint8_t> lastVsConstants;  // the vertex constants of the draw being made

    // ---- Frame effects (compositeScene): the 3D scene renders in high precision with its view depth beside it; when
    // the game starts on its 2D (HUD, menus) or presents, the scene gets sun shadows, ambient occlusion, effect lights,
    // bloom, tone mapping and anti-aliasing, and the 2D is drawn on the finished image.
    struct Target {
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11ShaderResourceView* srv = nullptr;
        UINT width = 0;
        UINT height = 0;
    };
    Target hdr, ldr, ldrTemp, lit, linear, glow, ao, aoBlur, bounce, bounceBlur;
    std::vector<Target> bloom;
    ID3D11Texture2D* shadowTexture = nullptr;
    ID3D11DepthStencilView* shadowDsv = nullptr;
    ID3D11ShaderResourceView* shadowSrv = nullptr;
    ID3D11VertexShader* fullscreenVS = nullptr;
    ID3D11PixelShader* aoPS = nullptr;
    ID3D11PixelShader* aoBlurPS = nullptr;
    ID3D11PixelShader* bouncePS = nullptr;
    ID3D11PixelShader* bounceBlurPS = nullptr;
    ID3D11PixelShader* lightingPS = nullptr;
    ID3D11PixelShader* bloomPrefilterPS = nullptr;
    ID3D11PixelShader* bloomDownPS = nullptr;
    ID3D11PixelShader* bloomUpPS = nullptr;
    ID3D11PixelShader* tonemapPS = nullptr;
    ID3D11PixelShader* copyPS = nullptr;
    ID3D11PixelShader* fxaaPS = nullptr;
    ID3D11PixelShader* shadowAlphaPS = nullptr;
    ID3D11Buffer* postConstants = nullptr;
    ID3D11Buffer* shadowDrawConstants = nullptr;
    ID3D11Buffer* shadowAlphaConstants = nullptr;
    ID3D11SamplerState* pointClamp = nullptr;
    ID3D11SamplerState* linearClamp = nullptr;
    ID3D11SamplerState* shadowCompare = nullptr;
    ID3D11RasterizerState* shadowRaster = nullptr;
    ID3D11RasterizerState* postRaster = nullptr;
    ID3D11DepthStencilState* shadowDepthState = nullptr;
    ID3D11DepthStencilState* noDepthState = nullptr;
    ID3D11BlendState* opaqueBlend = nullptr;
    ID3D11BlendState* additiveBlend = nullptr;
    bool effectsReady = false;
    bool composited = false;
    UINT sceneDraws3D = 0;

    struct Camera {
        D3DMATRIX view;
        D3DMATRIX projection;
        D3DVIEWPORT9 viewport;
    };
    std::vector<Camera> cameras;
    std::vector<UINT> cameraUses;
    struct ShadowRecord {
        VertexShader11* vs;
        ID3D11InputLayout* layout;
        D3D11_PRIMITIVE_TOPOLOGY topology;
        UINT stride, vertexAt, indexAt, indexCount;
        std::vector<std::uint8_t> constants;
        UINT camera;
        ID3D11ShaderResourceView* alphaView;  // alpha-tested casters (foliage)
        ID3D11SamplerState* alphaSampler;
        float alphaRef;
    };
    std::vector<ShadowRecord> records;
    struct EffectLight {
        float position[3];  // world space
        float radius;
        float color[3];
    };
    std::vector<EffectLight> effectLights;
    bool haveSun = false;
    float sunTravel[3] = {0, -1, 0};  // direction the game's main directional light travels (world space)
    float sunLuminance = 0;

    void createEffects();
    void createEffectTargets();
    void releaseEffectTargets();
    void recordForEffects(VertexShader11* vs, ID3D11InputLayout* layout, D3D11_PRIMITIVE_TOPOLOGY topology, UINT stride, UINT vertexAt,
        UINT indexAt, UINT indexCount);
    void captureEffectLight(const void* vertices, UINT vertexCount, UINT stride);
    void noteSun();
    void clearRecords();
    void clearCasters();
    // Effect lights persist between frames: the game only draws what is on screen, so a halo's light would vanish
    // whenever it left the view. Lights that keep their place are kept while off screen and fade when they are in
    // view but no longer drawn; brief ones (shots, explosions) fade out quickly. Brightness eases, so nothing pops.
    struct TrackedLight {
        float position[3];
        float radius;
        float color[3];
        float intensity;
        UINT firstSeen, lastSeen;
    };
    std::vector<TrackedLight> trackedLights;
    UINT effectFrame = 0;
    void updateTrackedLights(const Camera& camera);
    void compositeScene(bool full);
    void renderShadows(const Camera& camera, const D3DMATRIX sunProjection[2]);
    ID3D11VertexShader* shadowVariant(VertexShader11* vs);
    void fullscreenPass(ID3D11PixelShader* ps, const Target& target, ID3D11ShaderResourceView* const* views, UINT viewCount,
        ID3D11BlendState* blend = nullptr);

    bool initialize(HWND targetWindow, UINT targetWidth, UINT targetHeight, bool useVsync);
    void createTargets();
    void releaseTargets();
    void setDefaults();
    Surface11* depthFor(Surface11* target);
    void bindTargets();
    bool upload(const void* data, UINT size, ID3D11Buffer* buffer, UINT capacity, UINT& offset, UINT& at);
    void draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* vertices, UINT vertexCount, UINT stride, const WORD* indices);
    void clearRect(const D3D11_RECT& rect, DWORD flags, D3DCOLOR color, float z, DWORD stencil);
    void applyStates();
    void applyTextures();
    ID3D11InputLayout* fvfLayout(DWORD vertexFormat, const VertexShader11* shader);
    VertexShader11* ffVertexShader();
    PixelShader11* ffPixelShader();
    void fillFfConstants();
    template <typename Map, typename Desc, typename Create>
    auto cachedState(Map& map, const Desc& desc, Create create) -> typename Map::mapped_type;

    // IDirect3DDevice9 (the part the translation layer uses).
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9* caps) override;
    HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS* parameters) override;
    HRESULT STDMETHODCALLTYPE Present(const RECT*, const RECT*, HWND, const RGNDATA*) override;
    HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** surface) override;
    HRESULT STDMETHODCALLTYPE CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL, IDirect3DTexture9** result, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL, IDirect3DCubeTexture9** result, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT format, D3DPOOL, IDirect3DSurface9** result, HANDLE*) override;
    HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9* source, IDirect3DSurface9* destination) override;
    HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD index, IDirect3DSurface9* target) override;
    HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD index, IDirect3DSurface9** target) override;
    HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9* surface) override;
    HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9** surface) override;
    HRESULT STDMETHODCALLTYPE BeginScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE EndScene() override { return D3D_OK; }
    HRESULT STDMETHODCALLTYPE Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) override;
    HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) override;
    HRESULT STDMETHODCALLTYPE SetViewport(const D3DVIEWPORT9* value) override { viewport = *value; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9* value) override { *value = viewport; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetMaterial(const D3DMATERIAL9* value) override { material = *value; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE SetLight(DWORD index, const D3DLIGHT9* light) override {
        if (index < 8) lights[index] = *light;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE LightEnable(DWORD index, BOOL enable) override {
        if (index < 8) lightEnabled[index] = enable;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) override;
    HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) override {
        if (stage < 4 && type < 33) tss[stage][type] = value;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD stage, D3DSAMPLERSTATETYPE type, DWORD value) override {
        if (stage < 4 && type < 14) sampler[stage][type] = value;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE state, DWORD value) override {
        if (state < 256) rs[state] = value;
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFVF(DWORD value) override { fvf = value; declaration = nullptr; return D3D_OK; }
    HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** result) override;
    HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9* value) override {
        declaration = static_cast<VertexDeclaration11*>(value);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9* shader) override {
        vertexShader = static_cast<VertexShader11*>(shader);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9* shader) override {
        pixelShader = static_cast<PixelShader11*>(shader);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT reg, const float* data, UINT count) override {
        if (reg < 256) std::memcpy(vsConstants[reg], data, std::min(count, 256 - reg) * 16);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT reg, const float* data, UINT count) override {
        if (reg < 32) std::memcpy(psConstants[reg], data, std::min(count, 32 - reg) * 16);
        return D3D_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) override;
    HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertexCount, UINT primitiveCount,
        const void* indices, D3DFORMAT indexFormat, const void* data, UINT stride) override;
};

Device11* g_instance = nullptr;

bool Device11::initialize(HWND targetWindow, UINT targetWidth, UINT targetHeight, bool useVsync) {
    window = targetWindow;
    width = targetWidth;
    height = targetHeight;
    vsync = useVsync;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (std::getenv("CW_D3D11_DEBUG") != nullptr) {
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL level{};
    HRESULT result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 2, D3D11_SDK_VERSION, &g_dev, &level, &g_ctx);
    if (FAILED(result)) {
        logf("d3d11: D3D11CreateDevice failed (0x%08lX)", result);
        return false;
    }
    g_ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), reinterpret_cast<void**>(&g_ctx1));

    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    IDXGIFactory2* factory = nullptr;
    g_dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    dxgiDevice->GetAdapter(&adapter);
    adapter->GetParent(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory));
    DXGI_ADAPTER_DESC adapterDesc{};
    adapter->GetDesc(&adapterDesc);
    if (IDXGIFactory5* factory5 = nullptr; SUCCEEDED(factory->QueryInterface(__uuidof(IDXGIFactory5), reinterpret_cast<void**>(&factory5)))) {
        BOOL allow = FALSE;
        tearing = SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow))) && allow;
        factory5->Release();
    }
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    result = factory->CreateSwapChainForHwnd(g_dev, window, &desc, nullptr, nullptr, &swapChain);
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    factory->Release();
    adapter->Release();
    dxgiDevice->Release();
    if (FAILED(result)) {
        logf("d3d11: CreateSwapChainForHwnd failed (0x%08lX)", result);
        return false;
    }

    auto makeBuffer = [](UINT size, UINT bind, ID3D11Buffer** buffer) {
        D3D11_BUFFER_DESC bufferDesc{};
        bufferDesc.ByteWidth = size;
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = bind;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&bufferDesc, nullptr, buffer);
    };
    makeBuffer(kVertexBufferSize, D3D11_BIND_VERTEX_BUFFER, &vertexBuffer);
    makeBuffer(kIndexBufferSize, D3D11_BIND_INDEX_BUFFER, &indexBuffer);
    makeBuffer(static_cast<UINT>(std::max<std::size_t>(sizeof(FfVertexConstants), kXboxVertexConstants * 16) + 15) & ~15u, D3D11_BIND_CONSTANT_BUFFER,
        &vsConstantBuffer);
    makeBuffer(((kXboxPixelConstants * 16) + 15) & ~15u, D3D11_BIND_CONSTANT_BUFFER, &xboxPsConstantBuffer);
    makeBuffer(sizeof(PixelCommonConstants), D3D11_BIND_CONSTANT_BUFFER, &pixelCommonBuffer);
    makeBuffer(32, D3D11_BIND_CONSTANT_BUFFER, &clearConstants);

    // Rectangle clears: a full-viewport triangle at depth z with a constant color.
    const char* clearSource = R"(
cbuffer Clear : register(b0) { float4 color; float4 depth; };
struct Out { float4 pos : SV_Position; };
Out vsMain(uint id : SV_VertexID) { Out o; float2 uv = float2((id << 1) & 2, id & 2); o.pos = float4(uv * float2(2, -2) + float2(-1, 1), depth.x, 1); return o; }
float4 psMain(Out i) : SV_Target { return color; }
)";
    ID3DBlob* code = nullptr;
    if (SUCCEEDED(D3DCompile(clearSource, std::strlen(clearSource), "clear", nullptr, nullptr, "vsMain", "vs_5_0", 0, 0, &code, nullptr))) {
        g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &clearVertexShader);
        code->Release();
    }
    if (SUCCEEDED(D3DCompile(clearSource, std::strlen(clearSource), "clear", nullptr, nullptr, "psMain", "ps_5_0", 0, 0, &code, nullptr))) {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &clearPixelShader);
        code->Release();
    }

    createEffects();
    createTargets();
    setDefaults();
    char adapterName[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), nullptr, nullptr);
    logf("d3d11: device ready on %s (feature level %X), %ux%u, %s", adapterName, static_cast<unsigned>(level), width, height,
        vsync ? "vsync" : tearing ? "no vsync (tearing)" : "no vsync");
    return true;
}

void Device11::createTargets() {
    createEffectTargets();
    // The game's back buffer: the high-precision scene until the frame effects have run, then the finished image.
    scene = new Surface11;
    scene->kind = Surface11::Kind::Target;
    scene->width = width;
    scene->height = height;
    scene->borrowed = true;
    scene->texture = hdr.texture;
    scene->rtv = hdr.rtv;
    sceneTexture = ldr.texture;
    composited = false;
    depth = createDepthSurface(width, height);
    renderTarget = scene;
    renderTarget->AddRef();
    depthStencil = depth;
    depthStencil->AddRef();
}

void Device11::releaseTargets() {
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    release(renderTarget);
    release(depthStencil);
    release(scene);
    release(depth);
    sceneTexture = nullptr;
    release(readback);
    releaseEffectTargets();
    for (auto& [size, surface] : sizedDepth) {
        surface->Release();
    }
    sizedDepth.clear();
}

void Device11::setDefaults() {
    rs[D3DRS_ZENABLE] = D3DZB_TRUE;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_BLENDOP] = D3DBLENDOP_ADD;
    rs[D3DRS_COLORWRITEENABLE] = 0xF;
    rs[D3DRS_LIGHTING] = TRUE;
    rs[D3DRS_COLORVERTEX] = TRUE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xFFFFFFFF;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_TEXTUREFACTOR] = 0xFFFFFFFF;
    rs[D3DRS_BLENDFACTOR] = 0xFFFFFFFF;
    float one = 1.0f;
    std::memcpy(&rs[D3DRS_FOGEND], &one, 4);
    std::memcpy(&rs[D3DRS_FOGDENSITY], &one, 4);
    for (int stage = 0; stage < 4; ++stage) {
        tss[stage][D3DTSS_COLOROP] = stage == 0 ? D3DTOP_MODULATE : D3DTOP_DISABLE;
        tss[stage][D3DTSS_ALPHAOP] = stage == 0 ? D3DTOP_SELECTARG1 : D3DTOP_DISABLE;
        tss[stage][D3DTSS_COLORARG1] = tss[stage][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        tss[stage][D3DTSS_COLORARG2] = tss[stage][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        tss[stage][D3DTSS_RESULTARG] = D3DTA_CURRENT;
        tss[stage][D3DTSS_TEXCOORDINDEX] = stage;
        sampler[stage][D3DSAMP_ADDRESSU] = sampler[stage][D3DSAMP_ADDRESSV] = sampler[stage][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        sampler[stage][D3DSAMP_MAGFILTER] = sampler[stage][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        sampler[stage][D3DSAMP_MAXANISOTROPY] = 1;
    }
    D3DMATRIX identity{};
    identity._11 = identity._22 = identity._33 = identity._44 = 1;
    world = view = projection = identity;
    for (D3DMATRIX& matrix : textureMatrix) {
        matrix = identity;
    }
}

// ---------------------------------------------------------------- resources

HRESULT Device11::GetDeviceCaps(D3DCAPS9* caps) {
    return stubCalled("GetDeviceCaps");
}

HRESULT Device11::CreateTexture(UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL, IDirect3DTexture9** result, HANDLE*) {
    auto* texture = new Texture11;
    texture->width = w;
    texture->height = h;
    texture->levels = std::max(1u, levels);
    texture->format = format;
    texture->renderTarget = (usage & D3DUSAGE_RENDERTARGET) != 0;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = texture->levels;
    desc.ArraySize = 1;
    desc.Format = dxgiFormat(format);
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (texture->renderTarget ? D3D11_BIND_RENDER_TARGET : 0);
    // Block-compressed levels must cover whole 4x4 blocks: small textures whose top level is not a multiple of 4
    // still upload their blocks (Direct3D 11 accepts BC textures of any size since feature level 10).
    HRESULT hr = g_dev->CreateTexture2D(&desc, nullptr, &texture->texture);
    if (FAILED(hr)) {
        logf("d3d11: could not create a %ux%u texture (format %u, %u levels): 0x%08lX", w, h, static_cast<unsigned>(format), texture->levels, hr);
        delete texture;
        return hr;
    }
    g_dev->CreateShaderResourceView(texture->texture, nullptr, &texture->srv);
    if (texture->renderTarget) {
        texture->surface = createTargetSurface(texture->texture, w, h);
    }
    *result = texture;
    return D3D_OK;
}

HRESULT Device11::CreateCubeTexture(UINT edge, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL, IDirect3DCubeTexture9** result, HANDLE*) {
    auto* texture = new CubeTexture11;
    texture->edge = edge;
    texture->levels = std::max(1u, levels);
    texture->format = format;
    texture->cube = true;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = edge;
    desc.MipLevels = texture->levels;
    desc.ArraySize = 6;
    desc.Format = dxgiFormat(format);
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
    const HRESULT hr = g_dev->CreateTexture2D(&desc, nullptr, &texture->texture);
    if (FAILED(hr)) {
        logf("d3d11: could not create a cube texture (%u): 0x%08lX", edge, hr);
        delete texture;
        return hr;
    }
    g_dev->CreateShaderResourceView(texture->texture, nullptr, &texture->srv);
    *result = texture;
    return D3D_OK;
}

HRESULT Device11::CreateOffscreenPlainSurface(UINT w, UINT h, D3DFORMAT format, D3DPOOL, IDirect3DSurface9** result, HANDLE*) {
    auto* surface = new Surface11;
    surface->kind = Surface11::Kind::System;
    surface->width = w;
    surface->height = h;
    surface->format = format;
    surface->pitch = w * 4;
    surface->memory.resize(static_cast<std::size_t>(surface->pitch) * h);
    *result = surface;
    return D3D_OK;
}

HRESULT Device11::GetRenderTargetData(IDirect3DSurface9* sourceSurface, IDirect3DSurface9* destinationSurface) {
    auto* source = static_cast<Surface11*>(sourceSurface);
    auto* destination = static_cast<Surface11*>(destinationSurface);
    if (source == scene && !composited) {
        compositeScene(sceneDraws3D > 0);
    }
    if (source->texture == nullptr || destination->kind != Surface11::Kind::System) {
        return D3DERR_INVALIDCALL;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->texture->GetDesc(&desc);
    if (readback != nullptr) {
        D3D11_TEXTURE2D_DESC existing{};
        readback->GetDesc(&existing);
        if (existing.Width != desc.Width || existing.Height != desc.Height) {
            release(readback);
        }
    }
    if (readback == nullptr) {
        desc.BindFlags = 0;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        g_dev->CreateTexture2D(&desc, nullptr, &readback);
    }
    g_ctx->CopyResource(readback, source->texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_ctx->Map(readback, 0, D3D11_MAP_READ, 0, &mapped))) {
        return D3DERR_INVALIDCALL;
    }
    const UINT rows = std::min(destination->height, desc.Height);
    const UINT bytes = std::min(destination->pitch, desc.Width * 4);
    for (UINT y = 0; y < rows; ++y) {
        std::memcpy(destination->memory.data() + static_cast<std::size_t>(y) * destination->pitch, static_cast<const std::uint8_t*>(mapped.pData) + y * mapped.RowPitch, bytes);
    }
    g_ctx->Unmap(readback, 0);
    return D3D_OK;
}

HRESULT Device11::GetBackBuffer(UINT, UINT, D3DBACKBUFFER_TYPE, IDirect3DSurface9** surface) {
    scene->AddRef();
    *surface = scene;
    return D3D_OK;
}

HRESULT Device11::SetRenderTarget(DWORD index, IDirect3DSurface9* target) {
    if (index != 0 || target == nullptr) {
        return D3D_OK;
    }
    auto* surface = static_cast<Surface11*>(target);
    surface->AddRef();
    release(renderTarget);
    renderTarget = surface;
    // Direct3D 9 resets the viewport to the whole target.
    viewport = {0, 0, surface->width, surface->height, 0, 1};
    return D3D_OK;
}

HRESULT Device11::GetRenderTarget(DWORD index, IDirect3DSurface9** target) {
    if (renderTarget == nullptr) {
        return D3DERR_NOTFOUND;
    }
    renderTarget->AddRef();
    *target = renderTarget;
    return D3D_OK;
}

HRESULT Device11::SetDepthStencilSurface(IDirect3DSurface9* surface) {
    auto* value = static_cast<Surface11*>(surface);
    if (value != nullptr) value->AddRef();
    release(depthStencil);
    depthStencil = value;
    return D3D_OK;
}

HRESULT Device11::GetDepthStencilSurface(IDirect3DSurface9** surface) {
    depth->AddRef();
    *surface = depth;
    return D3D_OK;
}

HRESULT Device11::SetTexture(DWORD stage, IDirect3DBaseTexture9* texture) {
    if (stage >= 4) {
        return D3D_OK;
    }
    if (texture != nullptr) texture->AddRef();
    if (textures[stage] != nullptr) textures[stage]->Release();
    textures[stage] = texture;
    return D3D_OK;
}

HRESULT Device11::SetTransform(D3DTRANSFORMSTATETYPE state, const D3DMATRIX* matrix) {
    if (state == D3DTS_VIEW) view = *matrix;
    else if (state == D3DTS_PROJECTION) projection = *matrix;
    else if (state == D3DTS_WORLD) world = *matrix;
    else if (state >= D3DTS_TEXTURE0 && state <= D3DTS_TEXTURE3) textureMatrix[state - D3DTS_TEXTURE0] = *matrix;
    return D3D_OK;
}

HRESULT Device11::CreateVertexDeclaration(const D3DVERTEXELEMENT9* elements, IDirect3DVertexDeclaration9** result) {
    auto* declarationObject = new VertexDeclaration11;
    for (const D3DVERTEXELEMENT9* element = elements; element->Stream != 0xFF; ++element) {
        declarationObject->elements.push_back(*element);
    }
    *result = declarationObject;
    return D3D_OK;
}

// ---------------------------------------------------------------- presenting

HRESULT Device11::Reset(D3DPRESENT_PARAMETERS* parameters) {
    releaseTargets();
    width = parameters->BackBufferWidth;
    height = parameters->BackBufferHeight;
    const HRESULT result = swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(result)) {
        logf("d3d11: ResizeBuffers to %ux%u failed (0x%08lX)", width, height, result);
        return result;
    }
    createTargets();
    viewport = {0, 0, width, height, 0, 1};
    return D3D_OK;
}

HRESULT Device11::Present(const RECT*, const RECT*, HWND, const RGNDATA*) {
    if (!composited) {
        compositeScene(sceneDraws3D > 0);
    }
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&backBuffer)))) {
        g_ctx->CopyResource(backBuffer, sceneTexture);
        backBuffer->Release();
    }
    const HRESULT result = swapChain->Present(vsync ? 1 : 0, !vsync && tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    drawCount = 0;
    // The next frame starts on the high-precision scene again.
    scene->texture = hdr.texture;
    scene->rtv = hdr.rtv;
    composited = false;
    sceneDraws3D = 0;
    clearRecords();
    effectLights.clear();
    haveSun = false;
    sunLuminance = 0;
    return result;
}

// ---------------------------------------------------------------- drawing

Surface11* Device11::depthFor(Surface11* target) {
    if (depthStencil == nullptr || target == nullptr || (depthStencil->width == target->width && depthStencil->height == target->height)) {
        return depthStencil;
    }
    // Direct3D 11 needs the depth buffer to cover the render target; the game pairs every target with the one depth
    // buffer, so targets of other sizes get their own.
    auto& sized = sizedDepth[{target->width, target->height}];
    if (sized == nullptr) {
        sized = createDepthSurface(target->width, target->height);
    }
    return sized;
}

void Device11::bindTargets() {
    Surface11* depthSurface = depthFor(renderTarget);
    // The scene's 3D also writes its view depth (target 1) and what additive effects add (target 2) for the frame effects.
    const bool effects = renderTarget == scene && !composited;
    ID3D11RenderTargetView* rtvs[3] = {renderTarget != nullptr ? renderTarget->rtv : nullptr, effects ? linear.rtv : nullptr, effects ? glow.rtv : nullptr};
    g_ctx->OMSetRenderTargets(effects ? 3 : 1, rtvs, depthSurface != nullptr ? depthSurface->dsv : nullptr);
    D3D11_VIEWPORT vp{static_cast<float>(viewport.X), static_cast<float>(viewport.Y), static_cast<float>(viewport.Width),
        static_cast<float>(viewport.Height), viewport.MinZ, viewport.MaxZ};
    g_ctx->RSSetViewports(1, &vp);
}

template <typename Map, typename Desc, typename Create>
auto Device11::cachedState(Map& map, const Desc& desc, Create create) -> typename Map::mapped_type {
    const std::string key = keyBytes(&desc, sizeof(desc));
    auto found = map.find(key);
    if (found != map.end()) {
        return found->second;
    }
    typename Map::mapped_type state = nullptr;
    create(desc, &state);
    map.emplace(key, state);
    return state;
}

void Device11::applyStates() {
    D3D11_BLEND_DESC blend{};
    auto& target = blend.RenderTarget[0];
    target.BlendEnable = rs[D3DRS_ALPHABLENDENABLE] != 0;
    DWORD source = rs[D3DRS_SRCBLEND];
    DWORD destination = rs[D3DRS_DESTBLEND];
    if (source == D3DBLEND_BOTHSRCALPHA) { source = D3DBLEND_SRCALPHA; destination = D3DBLEND_INVSRCALPHA; }
    if (source == D3DBLEND_BOTHINVSRCALPHA) { source = D3DBLEND_INVSRCALPHA; destination = D3DBLEND_SRCALPHA; }
    target.SrcBlend = convertBlend(source, false);
    target.DestBlend = convertBlend(destination, false);
    target.SrcBlendAlpha = convertBlend(source, true);
    target.DestBlendAlpha = convertBlend(destination, true);
    const DWORD op = rs[D3DRS_BLENDOP] >= 1 && rs[D3DRS_BLENDOP] <= 5 ? rs[D3DRS_BLENDOP] : D3DBLENDOP_ADD;
    target.BlendOp = target.BlendOpAlpha = static_cast<D3D11_BLEND_OP>(op);
    target.RenderTargetWriteMask = static_cast<UINT8>(rs[D3DRS_COLORWRITEENABLE] & 0xF);
    // View depth: opaque, depth-writing 3D only.
    blend.IndependentBlendEnable = TRUE;
    const bool screenSpace = vertexShader == nullptr && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    // (Vehicles draw with alpha blending on; what writes depth and does not add light counts as a surface.)
    const bool additiveDraw = rs[D3DRS_ALPHABLENDENABLE] && destination == D3DBLEND_ONE;
    blend.RenderTarget[1].RenderTargetWriteMask = renderTarget == scene && !composited && !screenSpace && rs[D3DRS_ZENABLE] != D3DZB_FALSE &&
                                                          rs[D3DRS_ZWRITEENABLE] && !additiveDraw
                                                      ? D3D11_COLOR_WRITE_ENABLE_RED
                                                      : 0;
    blend.RenderTarget[1].SrcBlend = blend.RenderTarget[1].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[1].DestBlend = blend.RenderTarget[1].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[1].BlendOp = blend.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    // Glow: additive draws, blended the same way.
    blend.RenderTarget[2] = target;
    const bool additive = rs[D3DRS_ALPHABLENDENABLE] && destination == D3DBLEND_ONE;
    blend.RenderTarget[2].RenderTargetWriteMask = renderTarget == scene && !composited && !screenSpace && additive ? target.RenderTargetWriteMask : 0;
    if (blend.RenderTarget[1].RenderTargetWriteMask != 0) {
        // Opaque surfaces drawn over earlier glow hide it.
        blend.RenderTarget[2].BlendEnable = TRUE;
        blend.RenderTarget[2].SrcBlend = blend.RenderTarget[2].SrcBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[2].DestBlend = blend.RenderTarget[2].DestBlendAlpha = D3D11_BLEND_ZERO;
        blend.RenderTarget[2].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    ID3D11BlendState* blendState = cachedState(blendStates, blend, [](const D3D11_BLEND_DESC& d, ID3D11BlendState** s) { g_dev->CreateBlendState(&d, s); });
    float factor[4];
    unpackColor(rs[D3DRS_BLENDFACTOR], factor);
    g_ctx->OMSetBlendState(blendState, factor, 0xFFFFFFFF);

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = rs[D3DRS_ZENABLE] != D3DZB_FALSE;
    depthDesc.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    auto compare = [](DWORD value) { return static_cast<D3D11_COMPARISON_FUNC>(value >= 1 && value <= 8 ? value : D3DCMP_ALWAYS); };
    auto stencilOp = [](DWORD value) { return static_cast<D3D11_STENCIL_OP>(value >= 1 && value <= 8 ? value : D3DSTENCILOP_KEEP); };
    depthDesc.DepthFunc = compare(rs[D3DRS_ZFUNC]);
    depthDesc.StencilEnable = rs[D3DRS_STENCILENABLE] != 0;
    depthDesc.StencilReadMask = static_cast<UINT8>(rs[D3DRS_STENCILMASK]);
    depthDesc.StencilWriteMask = static_cast<UINT8>(rs[D3DRS_STENCILWRITEMASK]);
    depthDesc.FrontFace = {stencilOp(rs[D3DRS_STENCILFAIL]), stencilOp(rs[D3DRS_STENCILZFAIL]), stencilOp(rs[D3DRS_STENCILPASS]), compare(rs[D3DRS_STENCILFUNC])};
    depthDesc.BackFace = depthDesc.FrontFace;
    ID3D11DepthStencilState* depthState =
        cachedState(depthStates, depthDesc, [](const D3D11_DEPTH_STENCIL_DESC& d, ID3D11DepthStencilState** s) { g_dev->CreateDepthStencilState(&d, s); });
    g_ctx->OMSetDepthStencilState(depthState, rs[D3DRS_STENCILREF] & 0xFF);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = rs[D3DRS_FILLMODE] == D3DFILL_WIREFRAME ? D3D11_FILL_WIREFRAME : D3D11_FILL_SOLID;
    raster.CullMode = rs[D3DRS_CULLMODE] == D3DCULL_CW ? D3D11_CULL_FRONT : rs[D3DRS_CULLMODE] == D3DCULL_CCW ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    raster.DepthBias = static_cast<INT>(std::lround(asFloat(rs[D3DRS_DEPTHBIAS]) * 16777216.0f));
    raster.SlopeScaledDepthBias = asFloat(rs[D3DRS_SLOPESCALEDEPTHBIAS]);
    raster.DepthClipEnable = TRUE;
    ID3D11RasterizerState* rasterState =
        cachedState(rasterStates, raster, [](const D3D11_RASTERIZER_DESC& d, ID3D11RasterizerState** s) { g_dev->CreateRasterizerState(&d, s); });
    g_ctx->RSSetState(rasterState);
}

void Device11::applyTextures() {
    ID3D11ShaderResourceView* views[4] = {};
    ID3D11SamplerState* samplers[4] = {};
    for (int stage = 0; stage < 4; ++stage) {
        if (auto* resource = dynamic_cast<ShaderResource11*>(textures[stage])) {
            views[stage] = resource->srv;
        }
        const DWORD* s = sampler[stage];
        D3D11_SAMPLER_DESC desc{};
        const DWORD minFilter = s[D3DSAMP_MINFILTER];
        const DWORD magFilter = s[D3DSAMP_MAGFILTER];
        const DWORD mipFilter = s[D3DSAMP_MIPFILTER];
        if (minFilter == D3DTEXF_ANISOTROPIC || magFilter == D3DTEXF_ANISOTROPIC) {
            desc.Filter = D3D11_FILTER_ANISOTROPIC;
        } else {
            const bool minLinear = minFilter == D3DTEXF_LINEAR;
            const bool magLinear = magFilter == D3DTEXF_LINEAR;
            const bool mipLinear = mipFilter == D3DTEXF_LINEAR;
            desc.Filter = static_cast<D3D11_FILTER>((minLinear ? 0x10 : 0) | (magLinear ? 0x4 : 0) | (mipLinear ? 0x1 : 0));
        }
        auto address = [](DWORD value) { return static_cast<D3D11_TEXTURE_ADDRESS_MODE>(value >= 1 && value <= 5 ? value : D3DTADDRESS_WRAP); };
        desc.AddressU = address(s[D3DSAMP_ADDRESSU]);
        desc.AddressV = address(s[D3DSAMP_ADDRESSV]);
        desc.AddressW = address(s[D3DSAMP_ADDRESSW]);
        desc.MipLODBias = asFloat(s[D3DSAMP_MIPMAPLODBIAS]);
        desc.MaxAnisotropy = std::clamp<UINT>(s[D3DSAMP_MAXANISOTROPY], 1, 16);
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        unpackColor(s[D3DSAMP_BORDERCOLOR], desc.BorderColor);
        desc.MinLOD = static_cast<float>(s[D3DSAMP_MAXMIPLEVEL]);
        desc.MaxLOD = mipFilter == D3DTEXF_NONE ? desc.MinLOD : D3D11_FLOAT32_MAX;
        samplers[stage] = cachedState(samplerStates, desc, [](const D3D11_SAMPLER_DESC& d, ID3D11SamplerState** state) { g_dev->CreateSamplerState(&d, state); });
    }
    g_ctx->PSSetShaderResources(0, 4, views);
    g_ctx->PSSetSamplers(0, 4, samplers);
}

ID3D11InputLayout* Device11::fvfLayout(DWORD vertexFormat, const VertexShader11* shader) {
    const std::string key = keyBytes(&vertexFormat, 4) + keyBytes(&shader, sizeof(shader));
    auto found = fvfLayouts.find(key);
    if (found != fvfLayouts.end()) {
        return found->second;
    }
    std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
    UINT offset = 0;
    auto add = [&](const char* semantic, UINT index, DXGI_FORMAT format, UINT size) {
        elements.push_back({semantic, index, format, 0, offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
        offset += size;
    };
    static constexpr DXGI_FORMAT kFloats[] = {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT};
    switch (vertexFormat & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZRHW: add("POSITION", 0, kFloats[3], 16); break;
    case D3DFVF_XYZB1: add("POSITION", 0, kFloats[2], 12); add("BLENDWEIGHT", 0, kFloats[0], 4); break;
    case D3DFVF_XYZB2: add("POSITION", 0, kFloats[2], 12); add("BLENDWEIGHT", 0, kFloats[1], 8); break;
    case D3DFVF_XYZB3: add("POSITION", 0, kFloats[2], 12); add("BLENDWEIGHT", 0, kFloats[2], 12); break;
    case D3DFVF_XYZB4: add("POSITION", 0, kFloats[2], 12); add("BLENDWEIGHT", 0, kFloats[3], 16); break;
    default: add("POSITION", 0, kFloats[2], 12); break;
    }
    if (vertexFormat & D3DFVF_NORMAL) add("NORMAL", 0, kFloats[2], 12);
    if (vertexFormat & D3DFVF_PSIZE) add("PSIZE", 0, kFloats[0], 4);
    if (vertexFormat & D3DFVF_DIFFUSE) add("COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 4);
    if (vertexFormat & D3DFVF_SPECULAR) add("COLOR", 1, DXGI_FORMAT_B8G8R8A8_UNORM, 4);
    const UINT texCount = std::min<UINT>((vertexFormat & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, 8);
    for (UINT set = 0; set < texCount; ++set) {
        static constexpr UINT kComponents[] = {2, 3, 4, 1};
        const UINT components = kComponents[(vertexFormat >> (16 + set * 2)) & 3];
        add("TEXCOORD", set, kFloats[components - 1], components * 4);
    }
    ID3D11InputLayout* layout = nullptr;
    const HRESULT result = g_dev->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), shader->bytecode.data(), shader->bytecode.size(), &layout);
    if (FAILED(result)) {
        logf("d3d11: input layout for FVF 0x%lX failed (0x%08lX)", vertexFormat, result);
    }
    fvfLayouts.emplace(key, layout);
    return layout;
}

VertexShader11* Device11::ffVertexShader() {
    FfVertexKey key;
    key.fvf = fvf;
    const bool pretransformed = (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    key.lighting = rs[D3DRS_LIGHTING] && !pretransformed;
    if (key.lighting) {
        key.colorVertex = rs[D3DRS_COLORVERTEX] != 0;
        key.specularEnable = rs[D3DRS_SPECULARENABLE] != 0;
        key.localViewer = rs[D3DRS_LOCALVIEWER] != 0;
        key.materialSource[0] = static_cast<std::uint8_t>(rs[D3DRS_DIFFUSEMATERIALSOURCE]);
        key.materialSource[1] = static_cast<std::uint8_t>(rs[D3DRS_AMBIENTMATERIALSOURCE]);
        key.materialSource[2] = static_cast<std::uint8_t>(rs[D3DRS_SPECULARMATERIALSOURCE]);
        key.materialSource[3] = static_cast<std::uint8_t>(rs[D3DRS_EMISSIVEMATERIALSOURCE]);
        for (int index = 0; index < 8; ++index) {
            key.lightType[index] = lightEnabled[index] ? static_cast<std::uint8_t>(lights[index].Type) : 0;
        }
    }
    if (rs[D3DRS_FOGENABLE]) {
        key.fogTableMode = static_cast<std::uint8_t>(rs[D3DRS_FOGTABLEMODE]);
        key.rangeFog = rs[D3DRS_RANGEFOGENABLE] != 0;
    }
    for (int stage = 0; stage < 4; ++stage) {
        key.texCoordIndex[stage] = tss[stage][D3DTSS_TEXCOORDINDEX];
        key.textureTransform[stage] = tss[stage][D3DTSS_TEXTURETRANSFORMFLAGS];
    }
    const std::string bytes = keyBytes(&key, sizeof(key));
    auto found = ffVertexShaders.find(bytes);
    if (found != ffVertexShaders.end()) {
        return found->second;
    }
    VertexShader11* shader = makeVertexShader(generateFfVertexShader(key), "ff_vs");
    ffVertexShaders.emplace(bytes, shader);
    return shader;
}

PixelShader11* Device11::ffPixelShader() {
    FfPixelKey key;
    for (int stage = 0; stage < 4; ++stage) {
        FfPixelKey::Stage& s = key.stage[stage];
        const DWORD* t = tss[stage];
        s.colorOp = static_cast<std::uint8_t>(t[D3DTSS_COLOROP]);
        if (s.colorOp == D3DTOP_DISABLE || s.colorOp == 0) {
            s.colorOp = D3DTOP_DISABLE;
            break;
        }
        s.alphaOp = static_cast<std::uint8_t>(t[D3DTSS_ALPHAOP] == 0 ? D3DTOP_DISABLE : t[D3DTSS_ALPHAOP]);
        s.colorArg[0] = static_cast<std::uint8_t>(t[D3DTSS_COLORARG0]);
        s.colorArg[1] = static_cast<std::uint8_t>(t[D3DTSS_COLORARG1]);
        s.colorArg[2] = static_cast<std::uint8_t>(t[D3DTSS_COLORARG2]);
        s.alphaArg[0] = static_cast<std::uint8_t>(t[D3DTSS_ALPHAARG0]);
        s.alphaArg[1] = static_cast<std::uint8_t>(t[D3DTSS_ALPHAARG1]);
        s.alphaArg[2] = static_cast<std::uint8_t>(t[D3DTSS_ALPHAARG2]);
        s.resultArg = static_cast<std::uint8_t>(t[D3DTSS_RESULTARG]);
        if (auto* resource = dynamic_cast<ShaderResource11*>(textures[stage])) {
            s.textureType = resource->cube ? 2 : 1;
        }
        const DWORD flags = t[D3DTSS_TEXTURETRANSFORMFLAGS];
        if (flags & D3DTTFF_PROJECTED) {
            s.projected = static_cast<std::uint8_t>(std::clamp<DWORD>(flags & 0xFF, 2, 4));
        }
    }
    key.specularAdd = rs[D3DRS_SPECULARENABLE] != 0;
    key.flatShading = rs[D3DRS_SHADEMODE] == D3DSHADE_FLAT;
    const std::string bytes = keyBytes(&key, sizeof(key));
    auto found = ffPixelShaders.find(bytes);
    if (found != ffPixelShaders.end()) {
        return found->second;
    }
    PixelShader11* shader = makePixelShader(generateFfPixelShader(key), "ff_ps");
    ffPixelShaders.emplace(bytes, shader);
    return shader;
}

void Device11::fillFfConstants() {
    FfVertexConstants c{};
    copyMatrix(c.world, world);
    copyMatrix(c.view, view);
    copyMatrix(c.projection, projection);
    copyMatrix(c.worldView, multiply(world, view));
    for (int stage = 0; stage < 4; ++stage) {
        copyMatrix(c.textureMatrix[stage], textureMatrix[stage]);
    }
    std::memcpy(c.materialDiffuse, &material.Diffuse, 16);
    std::memcpy(c.materialAmbient, &material.Ambient, 16);
    std::memcpy(c.materialSpecular, &material.Specular, 16);
    std::memcpy(c.materialEmissive, &material.Emissive, 16);
    c.materialPower[0] = material.Power;
    unpackColor(rs[D3DRS_AMBIENT], c.globalAmbient);
    for (int index = 0; index < 8; ++index) {
        if (!lightEnabled[index]) {
            continue;
        }
        const D3DLIGHT9& light = lights[index];
        auto& out = c.lights[index];
        std::memcpy(out.diffuse, &light.Diffuse, 16);
        std::memcpy(out.specular, &light.Specular, 16);
        std::memcpy(out.ambient, &light.Ambient, 16);
        // Lights are given in world space; the generated shader lights in view space.
        const D3DVECTOR& p = light.Position;
        out.position[0] = p.x * view._11 + p.y * view._21 + p.z * view._31 + view._41;
        out.position[1] = p.x * view._12 + p.y * view._22 + p.z * view._32 + view._42;
        out.position[2] = p.x * view._13 + p.y * view._23 + p.z * view._33 + view._43;
        const D3DVECTOR& d = light.Direction;
        float direction[3] = {d.x * view._11 + d.y * view._21 + d.z * view._31, d.x * view._12 + d.y * view._22 + d.z * view._32,
            d.x * view._13 + d.y * view._23 + d.z * view._33};
        const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
        for (int axis = 0; axis < 3; ++axis) {
            out.direction[axis] = length > 0 ? direction[axis] / length : 0;
        }
        out.range[0] = light.Range;
        out.range[1] = light.Falloff;
        out.range[2] = light.Attenuation0;
        out.range[3] = light.Attenuation1;
        out.spot[0] = light.Attenuation2;
        out.spot[1] = std::cos(light.Theta * 0.5f);
        out.spot[2] = std::cos(light.Phi * 0.5f);
    }
    const UINT targetWidth = renderTarget != nullptr ? renderTarget->width : width;
    const UINT targetHeight = renderTarget != nullptr ? renderTarget->height : height;
    c.targetSize[0] = static_cast<float>(targetWidth);
    c.targetSize[1] = static_cast<float>(targetHeight);
    c.targetSize[2] = 1.0f / targetWidth;
    c.targetSize[3] = 1.0f / targetHeight;
    c.fog[0] = asFloat(rs[D3DRS_FOGSTART]);
    c.fog[1] = asFloat(rs[D3DRS_FOGEND]);
    c.fog[2] = asFloat(rs[D3DRS_FOGDENSITY]);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_ctx->Map(vsConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &c, sizeof(c));
        g_ctx->Unmap(vsConstantBuffer, 0);
    }
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&c);
    lastVsConstants.assign(bytes, bytes + sizeof(c));
}

bool Device11::upload(const void* data, UINT size, ID3D11Buffer* buffer, UINT capacity, UINT& offset, UINT& at) {
    if (size > capacity) {
        return false;
    }
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (offset + size > capacity) {
        offset = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
        clearCasters();  // their vertices are gone
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_ctx->Map(buffer, 0, mode, 0, &mapped))) {
        return false;
    }
    std::memcpy(static_cast<std::uint8_t*>(mapped.pData) + offset, data, size);
    g_ctx->Unmap(buffer, 0);
    at = offset;
    offset = (offset + size + 15) & ~15u;
    return true;
}

void Device11::draw(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* vertices, UINT vertexCount, UINT stride, const WORD* indices) {
    const bool screenSpace = vertexShader == nullptr && (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW;
    if (renderTarget == scene && !composited) {
        if (screenSpace && sceneDraws3D > 0) {
            compositeScene(true);  // the 2D begins: finish the 3D first
        } else if (!screenSpace) {
            ++sceneDraws3D;
        }
    }
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT indexCount = 0;
    std::vector<WORD> fanIndices;
    switch (type) {
    case D3DPT_POINTLIST: topology = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; indexCount = primitiveCount; break;
    case D3DPT_LINELIST: topology = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; indexCount = primitiveCount * 2; break;
    case D3DPT_LINESTRIP: topology = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; indexCount = primitiveCount + 1; break;
    case D3DPT_TRIANGLELIST: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST; indexCount = primitiveCount * 3; break;
    case D3DPT_TRIANGLESTRIP: topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; indexCount = primitiveCount + 2; break;
    case D3DPT_TRIANGLEFAN:
        // Direct3D 11 has no fans: list the triangles.
        topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        for (UINT triangle = 0; triangle < primitiveCount; ++triangle) {
            fanIndices.push_back(indices != nullptr ? indices[0] : 0);
            fanIndices.push_back(indices != nullptr ? indices[triangle + 1] : static_cast<WORD>(triangle + 1));
            fanIndices.push_back(indices != nullptr ? indices[triangle + 2] : static_cast<WORD>(triangle + 2));
        }
        indices = fanIndices.data();
        indexCount = static_cast<UINT>(fanIndices.size());
        break;
    default: return;
    }

    // Shaders: the Xbox vertex shader's translation, or the fixed-function one for this state; the Xbox pixel
    // shader's translation, or the texture stages' generated one.
    VertexShader11* vs = vertexShader;
    ID3D11InputLayout* layout = nullptr;
    if (vs != nullptr) {
        if (declaration == nullptr) {
            return;
        }
        auto found = declaration->layouts.find(vs);
        if (found == declaration->layouts.end()) {
            std::vector<D3D11_INPUT_ELEMENT_DESC> elements;
            for (const D3DVERTEXELEMENT9& element : declaration->elements) {
                elements.push_back({element.Usage == D3DDECLUSAGE_POSITION ? "POSITION" : "TEXCOORD", element.UsageIndex,
                    DXGI_FORMAT_R32G32B32A32_FLOAT, 0, element.Offset, D3D11_INPUT_PER_VERTEX_DATA, 0});
            }
            ID3D11InputLayout* created = nullptr;
            if (FAILED(g_dev->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), vs->bytecode.data(), vs->bytecode.size(), &created))) {
                logf("d3d11: input layout for an Xbox vertex shader failed");
            }
            found = declaration->layouts.emplace(vs, created).first;
        }
        layout = found->second;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_ctx->Map(vsConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, vsConstants, kXboxVertexConstants * 16);
            g_ctx->Unmap(vsConstantBuffer, 0);
        }
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(vsConstants);
        lastVsConstants.assign(bytes, bytes + kXboxVertexConstants * 16);
    } else {
        vs = ffVertexShader();
        if (vs == nullptr) {
            return;
        }
        layout = fvfLayout(fvf, vs);
        fillFfConstants();
        if (rs[D3DRS_LIGHTING]) {
            noteSun();
        }
    }
    PixelShader11* ps = pixelShader != nullptr ? pixelShader : ffPixelShader();
    if (layout == nullptr || ps == nullptr || vs->shader == nullptr || ps->shader == nullptr) {
        return;
    }
    if (pixelShader != nullptr) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_ctx->Map(xboxPsConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, psConstants, kXboxPixelConstants * 16);
            g_ctx->Unmap(xboxPsConstantBuffer, 0);
        }
    }
    PixelCommonConstants common{};
    unpackColor(rs[D3DRS_FOGCOLOR], common.fogColor);
    unpackColor(rs[D3DRS_TEXTUREFACTOR], common.textureFactor);
    common.alphaTest[0] = (rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f;
    common.alphaTest[1] = rs[D3DRS_ALPHATESTENABLE] ? static_cast<float>(rs[D3DRS_ALPHAFUNC]) : static_cast<float>(D3DCMP_ALWAYS);
    common.alphaTest[2] = rs[D3DRS_FOGENABLE] ? 1.0f : 0.0f;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_ctx->Map(pixelCommonBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, &common, sizeof(common));
        g_ctx->Unmap(pixelCommonBuffer, 0);
    }

    UINT vertexAt = 0;
    if (!upload(vertices, vertexCount * stride, vertexBuffer, kVertexBufferSize, vertexOffset, vertexAt)) {
        return;
    }
    UINT indexAt = 0;
    std::vector<WORD> sequential;
    if (indices == nullptr) {
        sequential.resize(indexCount);
        for (UINT index = 0; index < indexCount; ++index) {
            sequential[index] = static_cast<WORD>(index);
        }
        indices = sequential.data();
    }
    if (!upload(indices, indexCount * 2, indexBuffer, kIndexBufferSize, indexOffset, indexAt)) {
        return;
    }
    if (renderTarget == scene && !composited && !screenSpace) {
        recordForEffects(vs, layout, topology, stride, vertexAt, indexAt, indexCount);
        if (vertexShader == nullptr) {
            captureEffectLight(vertices, vertexCount, stride);
        }
    }

    bindTargets();
    applyStates();
    applyTextures();
    g_ctx->IASetInputLayout(layout);
    g_ctx->IASetPrimitiveTopology(topology);
    g_ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexAt);
    g_ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, indexAt);
    g_ctx->VSSetShader(vs->shader, nullptr, 0);
    g_ctx->PSSetShader(ps->shader, nullptr, 0);
    ID3D11Buffer* vsBuffers[] = {vsConstantBuffer};
    ID3D11Buffer* psBuffers[] = {nullptr, xboxPsConstantBuffer, pixelCommonBuffer};
    g_ctx->VSSetConstantBuffers(0, 1, vsBuffers);
    g_ctx->PSSetConstantBuffers(0, 3, psBuffers);
    g_ctx->DrawIndexed(indexCount, 0, 0);
    ++drawCount;
}

HRESULT Device11::DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT primitiveCount, const void* data, UINT stride) {
    UINT vertexCount = 0;
    switch (type) {
    case D3DPT_POINTLIST: vertexCount = primitiveCount; break;
    case D3DPT_LINELIST: vertexCount = primitiveCount * 2; break;
    case D3DPT_LINESTRIP: vertexCount = primitiveCount + 1; break;
    case D3DPT_TRIANGLELIST: vertexCount = primitiveCount * 3; break;
    default: vertexCount = primitiveCount + 2; break;
    }
    draw(type, primitiveCount, data, vertexCount, stride, nullptr);
    return D3D_OK;
}

HRESULT Device11::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT minIndex, UINT vertexCount, UINT primitiveCount, const void* indices,
    D3DFORMAT indexFormat, const void* data, UINT stride) {
    if (indexFormat != D3DFMT_INDEX16) {
        return stubCalled("DrawIndexedPrimitiveUP with 32-bit indices");
    }
    draw(type, primitiveCount, data, minIndex + vertexCount, stride, static_cast<const WORD*>(indices));
    return D3D_OK;
}

// Direct3D 9 clears the viewport (or the given rectangles inside it), not the whole target.
HRESULT Device11::Clear(DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    if (renderTarget == nullptr) {
        return D3D_OK;
    }
    const LONG left = static_cast<LONG>(viewport.X);
    const LONG top = static_cast<LONG>(viewport.Y);
    const LONG right = static_cast<LONG>(std::min<DWORD>(viewport.X + viewport.Width, renderTarget->width));
    const LONG bottom = static_cast<LONG>(std::min<DWORD>(viewport.Y + viewport.Height, renderTarget->height));
    std::vector<D3D11_RECT> areas;
    if (count == 0 || rects == nullptr) {
        areas.push_back({left, top, right, bottom});
    } else {
        for (DWORD index = 0; index < count; ++index) {
            D3D11_RECT area{std::max(left, rects[index].x1), std::max(top, rects[index].y1), std::min(right, rects[index].x2), std::min(bottom, rects[index].y2)};
            if (area.right > area.left && area.bottom > area.top) {
                areas.push_back(area);
            }
        }
    }
    for (const D3D11_RECT& area : areas) {
        clearRect(area, flags, color, z, stencil);
    }
    if (renderTarget == scene && !composited && (flags & D3DCLEAR_ZBUFFER) && linear.rtv != nullptr) {
        const float none[4] = {};
        g_ctx->ClearRenderTargetView(linear.rtv, none);
        g_ctx->ClearRenderTargetView(glow.rtv, none);
    }
    return D3D_OK;
}

void Device11::clearRect(const D3D11_RECT& rect, DWORD flags, D3DCOLOR color, float z, DWORD stencil) {
    Surface11* depthSurface = depthFor(renderTarget);
    const bool whole = rect.left == 0 && rect.top == 0 && static_cast<UINT>(rect.right) == renderTarget->width &&
                       static_cast<UINT>(rect.bottom) == renderTarget->height;
    float rgba[4];
    unpackColor(color, rgba);
    if (whole) {
        if (flags & D3DCLEAR_TARGET) g_ctx->ClearRenderTargetView(renderTarget->rtv, rgba);
        if ((flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && depthSurface != nullptr && depthSurface->dsv != nullptr) {
            g_ctx->ClearDepthStencilView(depthSurface->dsv, ((flags & D3DCLEAR_ZBUFFER) ? D3D11_CLEAR_DEPTH : 0) | ((flags & D3DCLEAR_STENCIL) ? D3D11_CLEAR_STENCIL : 0),
                z, static_cast<UINT8>(stencil));
        }
        return;
    }
    // Part of the target: draw the rectangle with the clear values.
    ID3D11RenderTargetView* rtv = renderTarget->rtv;
    g_ctx->OMSetRenderTargets(1, &rtv, depthSurface != nullptr ? depthSurface->dsv : nullptr);
    D3D11_VIEWPORT vp{static_cast<float>(rect.left), static_cast<float>(rect.top), static_cast<float>(rect.right - rect.left),
        static_cast<float>(rect.bottom - rect.top), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = (flags & D3DCLEAR_TARGET) ? D3D11_COLOR_WRITE_ENABLE_ALL : 0;
    g_ctx->OMSetBlendState(cachedState(blendStates, blend, [](const D3D11_BLEND_DESC& d, ID3D11BlendState** s) { g_dev->CreateBlendState(&d, s); }), nullptr, 0xFFFFFFFF);
    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = (flags & D3DCLEAR_ZBUFFER) != 0;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depthDesc.StencilEnable = (flags & D3DCLEAR_STENCIL) != 0;
    depthDesc.StencilReadMask = depthDesc.StencilWriteMask = 0xFF;
    depthDesc.FrontFace = {D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS};
    depthDesc.BackFace = depthDesc.FrontFace;
    g_ctx->OMSetDepthStencilState(
        cachedState(depthStates, depthDesc, [](const D3D11_DEPTH_STENCIL_DESC& d, ID3D11DepthStencilState** s) { g_dev->CreateDepthStencilState(&d, s); }),
        stencil & 0xFF);
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    g_ctx->RSSetState(cachedState(rasterStates, raster, [](const D3D11_RASTERIZER_DESC& d, ID3D11RasterizerState** s) { g_dev->CreateRasterizerState(&d, s); }));
    float constants[8] = {rgba[0], rgba[1], rgba[2], rgba[3], std::clamp(z, 0.0f, 1.0f), 0, 0, 0};
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_ctx->Map(clearConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        std::memcpy(mapped.pData, constants, sizeof(constants));
        g_ctx->Unmap(clearConstants, 0);
    }
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(clearVertexShader, nullptr, 0);
    g_ctx->PSSetShader(clearPixelShader, nullptr, 0);
    g_ctx->VSSetConstantBuffers(0, 1, &clearConstants);
    g_ctx->PSSetConstantBuffers(0, 1, &clearConstants);
    g_ctx->Draw(3, 0);
}

// ---------------------------------------------------------------- frame effects

// CW_FX=0 turns all of them off (the scene as the game drew it); CW_SHADOWS, CW_AO, CW_BLOOM, CW_FXAA and CW_LIGHTS =0
// turn off one. CW_SHADOW_RADII="near,far" sizes the two shadow cascades and CW_AO_RADIUS the occlusion (game units).
struct EffectSettings {
    bool enabled = true;
    bool shadows = true;
    bool ao = true;
    bool bloom = true;
    bool fxaa = true;
    bool lights = true;
    float radii[2] = {60.0f, 350.0f};
    float aoRadius = 6.0f;
};

bool envOn(const char* name) {
    const char* value = std::getenv(name);
    return value == nullptr || std::strcmp(value, "0") != 0;
}

const EffectSettings& effectSettings() {
    static const EffectSettings settings = [] {
        EffectSettings s;
        s.enabled = envOn("CW_FX");
        s.shadows = envOn("CW_SHADOWS");
        s.ao = envOn("CW_AO");
        s.bloom = envOn("CW_BLOOM");
        s.fxaa = envOn("CW_FXAA");
        s.lights = envOn("CW_LIGHTS");
        if (const char* value = std::getenv("CW_SHADOW_RADII")) {
            std::sscanf(value, "%f,%f", &s.radii[0], &s.radii[1]);
        }
        if (const char* value = std::getenv("CW_AO_RADIUS")) {
            s.aoRadius = static_cast<float>(std::atof(value));
        }
        return s;
    }();
    return settings;
}

constexpr UINT kShadowSize = 2048;  // per cascade; the atlas holds two side by side
constexpr int kMaxLights = 48;
constexpr int kBloomLevels = 5;

// Each planet's grade: color tint, saturation, contrast, bloom, the color fully shadowed ground takes, and how strong
// ambient occlusion is. Night copies of the maps (mods/night_maps: night<N>.wld) darken the same look to moonlight.
struct Look {
    const char* mission;
    float tint[3];
    float saturation, contrast, bloom, threshold, vignette;
    float shadow[3];
    float ao;
    float reflect;  // how mirror-like surfaces are (ice high, sand low)
};
const Look kLooks[] = {
    // Geonosis: warm, dusty red rock; shadows go brown.
    {"multi8", {1.04f, 0.99f, 0.93f}, 1.10f, 1.07f, 0.30f, 0.85f, 0.14f, {0.56f, 0.46f, 0.42f}, 0.6f, 0.05f},
    // Kashyyyk: humid green forest, deep shade under the trees.
    {"multi12", {0.97f, 1.03f, 0.96f}, 1.08f, 1.06f, 0.30f, 0.85f, 0.16f, {0.44f, 0.51f, 0.46f}, 0.7f, 0.07f},
    // Rhen Var: cold blue-white snow and ice; blue shadows.
    {"multi6", {0.96f, 0.99f, 1.05f}, 1.04f, 1.05f, 0.35f, 0.85f, 0.10f, {0.50f, 0.58f, 0.72f}, 0.5f, 0.35f},
    // Thule Moon (Academy and Conquest): pale grey moon rock.
    {"multi5", {0.98f, 0.99f, 1.03f}, 1.05f, 1.06f, 0.30f, 0.85f, 0.12f, {0.50f, 0.53f, 0.60f}, 0.6f, 0.12f},
    {"multi10", {0.98f, 0.99f, 1.03f}, 1.05f, 1.06f, 0.30f, 0.85f, 0.12f, {0.50f, 0.53f, 0.60f}, 0.6f, 0.10f},
};
const Look kDefaultLook = {"", {1.0f, 1.0f, 1.0f}, 1.05f, 1.04f, 0.35f, 0.80f, 0.12f, {0.55f, 0.57f, 0.62f}, 0.55f, 0.25f};  // also the menus' polished hangar

// Menu emitters: the hangar's console screens and indicator lights (textureContentHash of their textures) light the
// room around them.
struct MenuEmitter {
    std::uint64_t texture;
    float color[3];
};
const MenuEmitter kMenuEmitters[] = {
    {0xEF7AA3A356EFC5B0ull, {1.0f, 0.55f, 0.15f}},  // console with orange screens
    {0xB0028BF1CB2BED02ull, {1.0f, 0.55f, 0.15f}},  // orange gauges
    {0x4CBDD2AC100D81B7ull, {1.0f, 0.55f, 0.15f}},  // orange display
    {0xBA4ABD898B321EB0ull, {1.0f, 0.45f, 0.3f}},   // red/green indicator lights
};

struct alignas(16) PostConstantsData {
    float screen[4];
    float viewport[4];
    float projection[4];
    float viewToShadow[2][16];
    float shadowParams[4];
    float shadowColor[4];
    float sunView[4];
    float lighting[4];
    float grade[4];
    float tint[4];
    float aoParams[4];
    float debugParams[4];
    float surfaceParams[4];
    float lightPos[kMaxLights][4];
    float lightColor[kMaxLights][4];
};

struct alignas(16) ShadowDrawData {
    float projection[4];
    float inverseView[16];
    float sunProjection[16];
};

D3DMATRIX identityMatrix() {
    D3DMATRIX m{};
    m._11 = m._22 = m._33 = m._44 = 1;
    return m;
}

D3DMATRIX inverse(const D3DMATRIX& matrix) {
    const float* m = &matrix._11;
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float determinant = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    D3DMATRIX result = identityMatrix();
    if (std::fabs(determinant) > 1e-12f) {
        float* r = &result._11;
        for (int k = 0; k < 16; ++k) {
            r[k] = inv[k] / determinant;
        }
    }
    return result;
}

void transformPoint(const float in[3], const D3DMATRIX& m, float out[3]) {
    const float x = in[0], y = in[1], z = in[2];
    out[0] = x * m._11 + y * m._21 + z * m._31 + m._41;
    out[1] = x * m._12 + y * m._22 + z * m._32 + m._42;
    out[2] = x * m._13 + y * m._23 + z * m._33 + m._43;
}

void normalize3(float v[3]) {
    const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1e-6f) {
        v[0] /= length;
        v[1] /= length;
        v[2] /= length;
    }
}

void cross3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

float dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

bool isPerspective(const D3DMATRIX& projection) {
    return std::fabs(projection._34 - 1.0f) < 1e-3f && std::fabs(projection._44) < 1e-3f && projection._11 != 0 && projection._22 != 0;
}

void Device11::createEffects() {
    auto compileWith = [](const std::string& source, const char* entry, const char* profile) -> ID3DBlob* {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(D3DCompile(source.data(), source.size(), "frame_effects", nullptr, nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                &code, &errors))) {
            logf("d3d11: frame effect %s failed to compile: %s", entry, errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            release(code);
        }
        release(errors);
        return code;
    };
    bool ok = true;
    const std::string post = std::string(kPostHlsl) + kPostHlsl2 + kPostHlsl3;
    if (ID3DBlob* code = compileWith(post, "fullscreenVS", "vs_5_0")) {
        g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &fullscreenVS);
        code->Release();
    } else {
        ok = false;
    }
    const std::pair<const char*, ID3D11PixelShader**> pixelShaders[] = {
        {"aoPS", &aoPS}, {"aoBlurPS", &aoBlurPS}, {"bouncePS", &bouncePS}, {"bounceBlurPS", &bounceBlurPS}, {"lightingPS", &lightingPS}, {"bloomPrefilterPS", &bloomPrefilterPS}, {"bloomDownPS", &bloomDownPS},
        {"bloomUpPS", &bloomUpPS}, {"tonemapPS", &tonemapPS}, {"copyPS", &copyPS}, {"fxaaPS", &fxaaPS}};
    for (const auto& [entry, shader] : pixelShaders) {
        if (ID3DBlob* code = compileWith(post, entry, "ps_5_0")) {
            g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, shader);
            code->Release();
        } else {
            ok = false;
        }
    }
    if (ID3DBlob* code = compileWith(std::string(commonInterpolantsHlsl(false)) + kShadowPixelHlsl, "main", "ps_5_0")) {
        g_dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shadowAlphaPS);
        code->Release();
    } else {
        ok = false;
    }

    auto makeBuffer = [](UINT size, ID3D11Buffer** buffer) {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = (size + 15) & ~15u;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        g_dev->CreateBuffer(&desc, nullptr, buffer);
    };
    makeBuffer(sizeof(PostConstantsData), &postConstants);
    makeBuffer(sizeof(ShadowDrawData), &shadowDrawConstants);
    makeBuffer(16, &shadowAlphaConstants);

    D3D11_SAMPLER_DESC sampler{};
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    g_dev->CreateSamplerState(&sampler, &pointClamp);
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    g_dev->CreateSamplerState(&sampler, &linearClamp);
    sampler.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    sampler.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    sampler.BorderColor[0] = sampler.BorderColor[1] = sampler.BorderColor[2] = sampler.BorderColor[3] = 1;
    g_dev->CreateSamplerState(&sampler, &shadowCompare);

    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_NONE;
    raster.DepthClipEnable = TRUE;
    g_dev->CreateRasterizerState(&raster, &postRaster);
    // Casters behind the shadow map's near plane still cast (clamped to it); a slope bias keeps surfaces from
    // shadowing themselves.
    raster.DepthClipEnable = FALSE;
    raster.DepthBias = 400;
    raster.SlopeScaledDepthBias = 2.0f;
    raster.DepthBiasClamp = 0.01f;
    g_dev->CreateRasterizerState(&raster, &shadowRaster);

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = TRUE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthDesc.DepthFunc = D3D11_COMPARISON_LESS;
    g_dev->CreateDepthStencilState(&depthDesc, &shadowDepthState);
    depthDesc.DepthEnable = FALSE;
    depthDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    g_dev->CreateDepthStencilState(&depthDesc, &noDepthState);

    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    blend.RenderTarget[0].SrcBlend = blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOp = blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    g_dev->CreateBlendState(&blend, &opaqueBlend);
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].DestBlend = blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    g_dev->CreateBlendState(&blend, &additiveBlend);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kShadowSize * 2;
    desc.Height = kShadowSize;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};
    dsv.Format = DXGI_FORMAT_D32_FLOAT;
    dsv.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ok = ok && SUCCEEDED(g_dev->CreateTexture2D(&desc, nullptr, &shadowTexture)) && SUCCEEDED(g_dev->CreateDepthStencilView(shadowTexture, &dsv, &shadowDsv)) &&
         SUCCEEDED(g_dev->CreateShaderResourceView(shadowTexture, &srv, &shadowSrv));
    effectsReady = ok;
    const EffectSettings& settings = effectSettings();
    logf("d3d11: frame effects %s (shadows %s, ambient occlusion %s, bloom %s, anti-aliasing %s, effect lights %s)",
        !ok ? "unavailable" : settings.enabled ? "on" : "off", settings.shadows ? "on" : "off", settings.ao ? "on" : "off", settings.bloom ? "on" : "off",
        settings.fxaa ? "on" : "off", settings.lights ? "on" : "off");
}

void Device11::createEffectTargets() {
    auto make = [](Target& target, DXGI_FORMAT format, UINT w, UINT h) {
        target.width = std::max(1u, w);
        target.height = std::max(1u, h);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = target.width;
        desc.Height = target.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&desc, nullptr, &target.texture))) {
            logf("d3d11: could not create a %ux%u frame-effect target", w, h);
            return;
        }
        g_dev->CreateRenderTargetView(target.texture, nullptr, &target.rtv);
        g_dev->CreateShaderResourceView(target.texture, nullptr, &target.srv);
    };
    make(hdr, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
    make(ldr, DXGI_FORMAT_B8G8R8A8_UNORM, width, height);
    make(ldrTemp, DXGI_FORMAT_B8G8R8A8_UNORM, width, height);
    make(lit, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
    make(linear, DXGI_FORMAT_R32_FLOAT, width, height);
    make(glow, DXGI_FORMAT_R16G16B16A16_FLOAT, width, height);
    make(ao, DXGI_FORMAT_R8_UNORM, width / 2, height / 2);
    make(aoBlur, DXGI_FORMAT_R8_UNORM, width / 2, height / 2);
    make(bounce, DXGI_FORMAT_R11G11B10_FLOAT, width / 2, height / 2);
    make(bounceBlur, DXGI_FORMAT_R11G11B10_FLOAT, width / 2, height / 2);
    bloom.resize(kBloomLevels);
    for (int level = 0; level < kBloomLevels; ++level) {
        make(bloom[level], DXGI_FORMAT_R11G11B10_FLOAT, width >> (level + 1), height >> (level + 1));
    }
    const float none[4] = {};
    for (Target* target : {&hdr, &ldr, &linear, &glow}) {
        if (target->rtv != nullptr) g_ctx->ClearRenderTargetView(target->rtv, none);
    }
}

void Device11::releaseEffectTargets() {
    for (Target* target : {&hdr, &ldr, &ldrTemp, &lit, &linear, &glow, &ao, &aoBlur, &bounce, &bounceBlur}) {
        release(target->rtv);
        release(target->srv);
        release(target->texture);
    }
    for (Target& target : bloom) {
        release(target.rtv);
        release(target.srv);
        release(target.texture);
    }
    bloom.clear();
}

void Device11::clearCasters() {
    for (ShadowRecord& record : records) {
        release(record.alphaView);
    }
    records.clear();
}

void Device11::clearRecords() {
    clearCasters();
    cameras.clear();
    cameraUses.clear();
}

void Device11::noteSun() {
    for (int index = 0; index < 8; ++index) {
        const D3DLIGHT9& light = lights[index];
        if (!lightEnabled[index] || light.Type != D3DLIGHT_DIRECTIONAL) {
            continue;
        }
        const float luminance = light.Diffuse.r * 0.3f + light.Diffuse.g * 0.59f + light.Diffuse.b * 0.11f;
        float direction[3] = {light.Direction.x, light.Direction.y, light.Direction.z};
        normalize3(direction);
        if (luminance > sunLuminance && dot3(direction, direction) > 0.5f) {
            sunLuminance = luminance;
            std::memcpy(sunTravel, direction, sizeof(direction));
            haveSun = true;
        }
    }
}

void Device11::recordForEffects(VertexShader11* vs, ID3D11InputLayout* layout, D3D11_PRIMITIVE_TOPOLOGY topology, UINT stride, UINT vertexAt,
    UINT indexAt, UINT indexCount) {
    const bool additive = rs[D3DRS_ALPHABLENDENABLE] && rs[D3DRS_DESTBLEND] == D3DBLEND_ONE;
    if (!isPerspective(projection) || rs[D3DRS_ZENABLE] == D3DZB_FALSE || !rs[D3DRS_ZWRITEENABLE] || additive ||
        topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST || topology == D3D11_PRIMITIVE_TOPOLOGY_LINELIST || topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP) {
        return;
    }
    if (cameras.empty() || std::memcmp(&cameras.back().view, &view, sizeof(view)) != 0 ||
        std::memcmp(&cameras.back().projection, &projection, sizeof(projection)) != 0 || std::memcmp(&cameras.back().viewport, &viewport, sizeof(viewport)) != 0) {
        cameras.push_back({view, projection, viewport});
        cameraUses.push_back(0);
    }
    ++cameraUses.back();
    ShadowRecord record{vs, layout, topology, stride, vertexAt, indexAt, indexCount, lastVsConstants, static_cast<UINT>(cameras.size() - 1), nullptr, nullptr, 0};
    const DWORD alphaFunc = rs[D3DRS_ALPHAFUNC];
    if (rs[D3DRS_ALPHATESTENABLE] && (alphaFunc == D3DCMP_GREATER || alphaFunc == D3DCMP_GREATEREQUAL)) {
        if (auto* resource = dynamic_cast<ShaderResource11*>(textures[0]); resource != nullptr && !resource->cube && resource->srv != nullptr) {
            record.alphaView = resource->srv;
            record.alphaView->AddRef();
            record.alphaRef = std::max((rs[D3DRS_ALPHAREF] & 0xFF) / 255.0f, 1.0f / 255.0f);
        }
    }
    records.push_back(std::move(record));
}

void Device11::captureEffectLight(const void* vertices, UINT vertexCount, UINT stride) {
    if (!effectSettings().lights || vertexCount == 0 || effectLights.size() >= 256) {
        return;
    }
    const DWORD positionType = fvf & D3DFVF_POSITION_MASK;
    if (positionType == D3DFVF_XYZRHW || positionType == 0) {
        return;
    }
    const bool additive = rs[D3DRS_ALPHABLENDENABLE] && rs[D3DRS_DESTBLEND] == D3DBLEND_ONE;
    // Menus: the hangar's glowing panels.
    const MenuEmitter* emitter = nullptr;
    if (!additive) {
        const bool menus = _strnicmp(cw::options::g_missionName, "shell", 5) == 0;
        if (!menus || textures[0] == nullptr) {
            return;
        }
        const std::uint64_t hash = textureContentHash(textures[0]);
        for (const MenuEmitter& candidate : kMenuEmitters) {
            if (candidate.texture == hash) emitter = &candidate;
        }
        if (emitter == nullptr) {
            return;
        }
    }
    UINT colorOffset = 12;
    switch (positionType) {
    case D3DFVF_XYZB1: colorOffset = 16; break;
    case D3DFVF_XYZB2: colorOffset = 20; break;
    case D3DFVF_XYZB3: colorOffset = 24; break;
    case D3DFVF_XYZB4: colorOffset = 28; break;
    default: break;
    }
    if (fvf & D3DFVF_NORMAL) colorOffset += 12;
    if (fvf & D3DFVF_PSIZE) colorOffset += 4;
    const bool hasDiffuse = (fvf & D3DFVF_DIFFUSE) != 0;
    float low[3] = {1e30f, 1e30f, 1e30f}, high[3] = {-1e30f, -1e30f, -1e30f};
    double color[4] = {};
    const auto* bytes = static_cast<const std::uint8_t*>(vertices);
    for (UINT vertex = 0; vertex < vertexCount; ++vertex) {
        const auto* p = reinterpret_cast<const float*>(bytes + static_cast<std::size_t>(vertex) * stride);
        float w[3];
        transformPoint(p, world, w);
        for (int axis = 0; axis < 3; ++axis) {
            low[axis] = std::min(low[axis], w[axis]);
            high[axis] = std::max(high[axis], w[axis]);
        }
        if (hasDiffuse) {
            const std::uint8_t* c = bytes + static_cast<std::size_t>(vertex) * stride + colorOffset;
            color[0] += c[2] / 255.0;
            color[1] += c[1] / 255.0;
            color[2] += c[0] / 255.0;
            color[3] += c[3] / 255.0;
        }
    }
    float rgb[3] = {1, 1, 1};
    float alpha = 1;
    if (hasDiffuse) {
        for (int k = 0; k < 3; ++k) rgb[k] = static_cast<float>(color[k] / vertexCount);
        alpha = static_cast<float>(color[3] / vertexCount);
    }
    if (auto* resource = dynamic_cast<ShaderResource11*>(textures[0])) {
        for (int k = 0; k < 3; ++k) rgb[k] *= resource->average[k];
        alpha *= resource->average[3];
    }
    float size = 0;
    for (int axis = 0; axis < 3; ++axis) {
        size = std::max(size, high[axis] - low[axis]);
    }
    EffectLight light{};
    for (int axis = 0; axis < 3; ++axis) {
        light.position[axis] = (low[axis] + high[axis]) * 0.5f;
    }
    if (emitter != nullptr) {
        light.radius = std::clamp(size * 0.8f, 3.0f, 30.0f);
        for (int k = 0; k < 3; ++k) light.color[k] = emitter->color[k] * 0.9f;
    } else {
        if (size > 400.0f) {
            return;  // sky-sized glows are not lights
        }
        const float strength = rs[D3DRS_SRCBLEND] == D3DBLEND_SRCALPHA ? alpha : 1.0f;
        light.radius = std::clamp(size * 5.0f, 14.0f, 120.0f);
        // Hue from the effect's colors; brightness from how much it adds, kept up for thin textures (halos, rings),
        // which are mostly transparent.
        const float peak = std::max({rgb[0], rgb[1], rgb[2], 0.05f});
        const float brightness = std::clamp(peak * strength * 5.0f, 0.5f, 1.8f);
        for (int k = 0; k < 3; ++k) light.color[k] = rgb[k] / peak * brightness;
        if (light.color[0] + light.color[1] + light.color[2] < 0.05f) {
            return;
        }
    }
    // Merge with a light already near this one (a laser is several draws).
    for (EffectLight& existing : effectLights) {
        float d[3] = {existing.position[0] - light.position[0], existing.position[1] - light.position[1], existing.position[2] - light.position[2]};
        if (dot3(d, d) < 0.25f * existing.radius * existing.radius) {
            const float a = existing.color[0] + existing.color[1] + existing.color[2];
            const float b = light.color[0] + light.color[1] + light.color[2];
            for (int k = 0; k < 3; ++k) {
                existing.position[k] = (existing.position[k] * a + light.position[k] * b) / std::max(a + b, 1e-4f);
                existing.color[k] = std::min(existing.color[k] + light.color[k], 2.5f);
            }
            existing.radius = std::max(existing.radius, light.radius);
            return;
        }
    }
    effectLights.push_back(light);
}

ID3D11VertexShader* Device11::shadowVariant(VertexShader11* vs) {
    if (vs->shadowTried) {
        return vs->shadowShader;
    }
    vs->shadowTried = true;
    std::string source = vs->source;
    const std::size_t at = source.find("VSOut main(");
    if (at == std::string::npos) {
        return nullptr;
    }
    const std::size_t open = at + std::strlen("VSOut main(");
    const std::size_t close = source.find(')', open);
    const std::string parameters = source.substr(open, close - open);
    std::string names;
    for (std::size_t start = 0; start <= parameters.size();) {
        const std::size_t end = std::min(parameters.find(',', start), parameters.size());
        std::string parameter = parameters.substr(start, end - start);
        if (const std::size_t colon = parameter.find(':'); colon != std::string::npos) {
            parameter = parameter.substr(0, colon);
        }
        while (!parameter.empty() && std::isspace(static_cast<unsigned char>(parameter.back()))) parameter.pop_back();
        const std::size_t space = parameter.find_last_of(" \t");
        if (!parameter.empty()) {
            names += (names.empty() ? "" : ", ") + parameter.substr(space == std::string::npos ? 0 : space + 1);
        }
        start = end + 1;
    }
    source.replace(at, std::strlen("VSOut main("), "VSOut sceneMain(");
    source += kShadowVertexHlsl;
    source += "VSOut main(" + parameters + ") {\n    VSOut o = sceneMain(" + names + ");\n";
    source += R"(    float w = o.pos.w;
    float2 ndc = o.pos.xy / (abs(w) > 1e-6 ? w : 1e-6);
    float4 v = float4((ndc.x - shadowCameraProjection.z) * w / shadowCameraProjection.x, (ndc.y - shadowCameraProjection.w) * w / shadowCameraProjection.y, w, 1);
    o.pos = mul(mul(v, shadowInverseView), shadowSunProjection);
    return o;
}
)";
    ID3DBlob* code = compile(source, "shadow_vs", "vs_5_0");
    if (code != nullptr) {
        g_dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs->shadowShader);
        code->Release();
    }
    return vs->shadowShader;
}

void Device11::renderShadows(const Camera&, const D3DMATRIX sunProjection[2]) {
    ID3D11RenderTargetView* none[3] = {};
    g_ctx->OMSetRenderTargets(3, none, shadowDsv);
    g_ctx->ClearDepthStencilView(shadowDsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    g_ctx->RSSetState(shadowRaster);
    g_ctx->OMSetDepthStencilState(shadowDepthState, 0);
    g_ctx->OMSetBlendState(opaqueBlend, nullptr, 0xFFFFFFFF);
    ID3D11Buffer* alphaBuffers[] = {shadowAlphaConstants};
    std::vector<D3DMATRIX> inverseViews(cameras.size());
    for (std::size_t index = 0; index < cameras.size(); ++index) {
        inverseViews[index] = inverse(cameras[index].view);
    }
    // CW_SHADOW_CASTERS=shader|ff (diagnostics): only the game's vertex-shader draws, or only fixed-function ones.
    static const int casters = [] {
        const char* value = std::getenv("CW_SHADOW_CASTERS");
        return value == nullptr ? 0 : _stricmp(value, "shader") == 0 ? 1 : _stricmp(value, "ff") == 0 ? 2 : 0;
    }();
    for (const ShadowRecord& record : records) {
        const bool fixedFunction = record.vs->source.find("cbuffer FixedFunction") != std::string::npos;
        if ((casters == 1 && fixedFunction) || (casters == 2 && !fixedFunction)) {
            continue;
        }
        ID3D11VertexShader* shader = shadowVariant(record.vs);
        if (shader == nullptr || record.layout == nullptr || record.constants.empty()) {
            continue;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(g_ctx->Map(vsConstantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            continue;
        }
        std::memcpy(mapped.pData, record.constants.data(), record.constants.size());
        g_ctx->Unmap(vsConstantBuffer, 0);
        UINT stride = record.stride;
        UINT offset = record.vertexAt;
        g_ctx->IASetInputLayout(record.layout);
        g_ctx->IASetPrimitiveTopology(record.topology);
        g_ctx->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        g_ctx->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R16_UINT, record.indexAt);
        g_ctx->VSSetShader(shader, nullptr, 0);
        ID3D11Buffer* vsBuffers[4] = {vsConstantBuffer, nullptr, nullptr, shadowDrawConstants};
        g_ctx->VSSetConstantBuffers(0, 4, vsBuffers);
        if (record.alphaView != nullptr) {
            const float alpha[4] = {record.alphaRef, 1, 0, 0};
            if (SUCCEEDED(g_ctx->Map(shadowAlphaConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, alpha, sizeof(alpha));
                g_ctx->Unmap(shadowAlphaConstants, 0);
            }
            g_ctx->PSSetShader(shadowAlphaPS, nullptr, 0);
            g_ctx->PSSetShaderResources(0, 1, &record.alphaView);
            ID3D11SamplerState* wrap = cachedState(samplerStates,
                [] {
                    D3D11_SAMPLER_DESC desc{};
                    desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                    desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
                    desc.MaxLOD = D3D11_FLOAT32_MAX;
                    return desc;
                }(),
                [](const D3D11_SAMPLER_DESC& d, ID3D11SamplerState** state) { g_dev->CreateSamplerState(&d, state); });
            g_ctx->PSSetSamplers(0, 1, &wrap);
            g_ctx->PSSetConstantBuffers(4, 1, alphaBuffers);
        } else {
            g_ctx->PSSetShader(nullptr, nullptr, 0);
        }
        const Camera& camera = cameras[record.camera];
        ShadowDrawData data{};
        data.projection[0] = camera.projection._11;
        data.projection[1] = camera.projection._22;
        data.projection[2] = camera.projection._31;
        data.projection[3] = camera.projection._32;
        std::memcpy(data.inverseView, &inverseViews[record.camera], 64);
        for (int cascade = 0; cascade < 2; ++cascade) {
            std::memcpy(data.sunProjection, &sunProjection[cascade], 64);
            if (FAILED(g_ctx->Map(shadowDrawConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                continue;
            }
            std::memcpy(mapped.pData, &data, sizeof(data));
            g_ctx->Unmap(shadowDrawConstants, 0);
            const D3D11_VIEWPORT vp{static_cast<float>(cascade * kShadowSize), 0, static_cast<float>(kShadowSize), static_cast<float>(kShadowSize), 0, 1};
            g_ctx->RSSetViewports(1, &vp);
            g_ctx->DrawIndexed(record.indexCount, 0, 0);
        }
    }
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void Device11::fullscreenPass(ID3D11PixelShader* ps, const Target& target, ID3D11ShaderResourceView* const* views, UINT viewCount, ID3D11BlendState* blend) {
    ID3D11ShaderResourceView* bound[8] = {};
    g_ctx->PSSetShaderResources(0, 8, bound);
    g_ctx->OMSetRenderTargets(1, &target.rtv, nullptr);
    for (UINT index = 0; index < viewCount && index < 8; ++index) {
        bound[index] = views[index];
    }
    g_ctx->PSSetShaderResources(0, 8, bound);
    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(target.width), static_cast<float>(target.height), 0, 1};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(postRaster);
    g_ctx->OMSetDepthStencilState(noDepthState, 0);
    g_ctx->OMSetBlendState(blend != nullptr ? blend : opaqueBlend, nullptr, 0xFFFFFFFF);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(fullscreenVS, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    ID3D11SamplerState* samplers[3] = {pointClamp, linearClamp, shadowCompare};
    g_ctx->PSSetSamplers(0, 3, samplers);
    g_ctx->PSSetConstantBuffers(0, 1, &postConstants);
    g_ctx->Draw(3, 0);
}

void Device11::updateTrackedLights(const Camera& camera) {
    ++effectFrame;
    constexpr UINT kStableFrames = 30;     // seen this long in one place: a fixture (halo, ring, lamp)
    constexpr UINT kHoldFrames = 1800;     // fixtures off screen keep their light this long
    for (const EffectLight& light : effectLights) {
        TrackedLight* match = nullptr;
        float best = 1e30f;
        for (TrackedLight& tracked : trackedLights) {
            const float d[3] = {tracked.position[0] - light.position[0], tracked.position[1] - light.position[1], tracked.position[2] - light.position[2]};
            const float distance = dot3(d, d);
            const float reach = 0.35f * std::max(tracked.radius, light.radius);
            if (distance < reach * reach && distance < best && tracked.lastSeen != effectFrame) {
                best = distance;
                match = &tracked;
            }
        }
        if (match == nullptr) {
            trackedLights.push_back({{light.position[0], light.position[1], light.position[2]}, light.radius,
                {light.color[0], light.color[1], light.color[2]}, 0.0f, effectFrame, effectFrame});
            match = &trackedLights.back();
        }
        for (int k = 0; k < 3; ++k) {
            match->position[k] += (light.position[k] - match->position[k]) * 0.5f;
            match->color[k] += (light.color[k] - match->color[k]) * 0.3f;
        }
        match->radius += (light.radius - match->radius) * 0.3f;
        match->lastSeen = effectFrame;
    }
    const D3DMATRIX& v = camera.view;
    const D3DMATRIX& p = camera.projection;
    for (TrackedLight& tracked : trackedLights) {
        const bool seen = tracked.lastSeen == effectFrame;
        const bool fixture = tracked.lastSeen - tracked.firstSeen >= kStableFrames;
        if (seen) {
            // Shots and flashes light up at once; fixtures ease in.
            tracked.intensity = fixture ? std::min(1.0f, tracked.intensity + 0.15f) : 1.0f;
            continue;
        }
        bool onScreen = false;
        float view[3];
        transformPoint(tracked.position, v, view);
        if (view[2] > 1.0f) {
            const float x = view[0] * p._11 / view[2] + p._31;
            const float y = view[1] * p._22 / view[2] + p._32;
            onScreen = std::fabs(x) < 0.9f && std::fabs(y) < 0.9f;
        }
        if (fixture && !onScreen && effectFrame - tracked.lastSeen < kHoldFrames) {
            continue;  // out of view: the game does not draw it, but it is still there
        }
        tracked.intensity *= fixture ? 0.9f : 0.6f;
    }
    trackedLights.erase(std::remove_if(trackedLights.begin(), trackedLights.end(),
                            [&](const TrackedLight& tracked) { return tracked.intensity < 0.01f && tracked.lastSeen != effectFrame; }),
        trackedLights.end());
    if (trackedLights.size() > 256) {
        trackedLights.erase(trackedLights.begin(), trackedLights.begin() + (trackedLights.size() - 256));
    }
}

void Device11::compositeScene(bool full) {
    composited = true;
    {
        static std::string logged = cw::options::g_missionName;
        static unsigned count = 0;
        if (++count % 300 == 0) {
            logged = cw::options::g_missionName;
            logf("d3d11: frame effects for '%s': %s, %u 3D draws, %zu recorded, %zu cameras, %zu effect lights, sun %s", logged.c_str(),
                full ? "full" : "pass-through", sceneDraws3D, records.size(), cameras.size(), effectLights.size(), haveSun ? "yes" : "no");
        }
    }
    ID3D11RenderTargetView* none[3] = {};
    g_ctx->OMSetRenderTargets(3, none, nullptr);
    const EffectSettings& settings = effectSettings();
    auto upload = [&](const PostConstantsData& data) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(g_ctx->Map(postConstants, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            std::memcpy(mapped.pData, &data, sizeof(data));
            g_ctx->Unmap(postConstants, 0);
        }
    };
    static PostConstantsData post{};
    post.screen[0] = static_cast<float>(width);
    post.screen[1] = static_cast<float>(height);
    post.screen[2] = 1.0f / width;
    post.screen[3] = 1.0f / height;

    if (!effectsReady || !settings.enabled || !full || cameras.empty()) {
        upload(post);
        ID3D11ShaderResourceView* views[] = {hdr.srv};
        fullscreenPass(copyPS, ldr, views, 1);
    } else {
        // The main camera: the one most of the scene was drawn with.
        const std::size_t main = std::max_element(cameraUses.begin(), cameraUses.end()) - cameraUses.begin();
        const Camera& camera = cameras[main];
        const D3DMATRIX inverseView = inverse(camera.view);

        // The look: per planet, darkened to moonlight on the night copies.
        std::string mission = cw::options::g_missionName;
        // CW_FORCE_NIGHT_FX=1 (tests): every map at night.
        static const bool forceNight = std::getenv("CW_FORCE_NIGHT_FX") != nullptr;
        const bool night = _strnicmp(mission.c_str(), "night", 5) == 0 || (forceNight && _strnicmp(mission.c_str(), "multi", 5) == 0);
        if (night) {
            mission = "multi" + mission.substr(5);
        }
        const Look* look = &kDefaultLook;
        for (const Look& candidate : kLooks) {
            const std::size_t length = std::strlen(candidate.mission);
            if (_strnicmp(mission.c_str(), candidate.mission, length) == 0 && (mission.size() == length || mission[length] == '.')) {
                look = &candidate;
            }
        }
        const float exposure = fx(night ? FxExposureNight : FxExposureDay);
        post.viewport[0] = static_cast<float>(camera.viewport.X);
        post.viewport[1] = static_cast<float>(camera.viewport.Y);
        post.viewport[2] = static_cast<float>(camera.viewport.Width);
        post.viewport[3] = static_cast<float>(camera.viewport.Height);
        post.projection[0] = camera.projection._11;
        post.projection[1] = camera.projection._22;
        post.projection[2] = camera.projection._31;
        post.projection[3] = camera.projection._32;

        // Sun shadows from the game's own sun (its main directional light), in two cascades around the camera,
        // snapped to whole shadow-map texels so they stay still as the camera moves.
        const bool shadows = settings.shadows && haveSun && !records.empty();
        post.shadowParams[0] = shadows ? 1.0f : 0.0f;
        if (shadows) {
            float travel[3] = {sunTravel[0], sunTravel[1], sunTravel[2]};
            normalize3(travel);
            float up[3] = {0, 1, 0};
            if (std::fabs(travel[1]) > 0.99f) {
                up[0] = 1;
                up[1] = 0;
            }
            float right[3], up2[3];
            cross3(up, travel, right);
            normalize3(right);
            cross3(travel, right, up2);
            D3DMATRIX lightView = identityMatrix();
            lightView._11 = right[0]; lightView._21 = right[1]; lightView._31 = right[2];
            lightView._12 = up2[0]; lightView._22 = up2[1]; lightView._32 = up2[2];
            lightView._13 = travel[0]; lightView._23 = travel[1]; lightView._33 = travel[2];
            const float cameraPosition[3] = {inverseView._41, inverseView._42, inverseView._43};
            float forward[3] = {inverseView._31, inverseView._32, inverseView._33};
            normalize3(forward);
            D3DMATRIX sunProjection[2];
            constexpr float kDepthRange = 2500.0f;
            for (int cascade = 0; cascade < 2; ++cascade) {
                const float radius = fx(cascade == 0 ? FxShadowNear : FxShadowFar);
                const float center[3] = {cameraPosition[0] + forward[0] * radius * 0.6f, cameraPosition[1] + forward[1] * radius * 0.6f,
                    cameraPosition[2] + forward[2] * radius * 0.6f};
                const float texel = 2 * radius / kShadowSize;
                const float cx = std::floor(dot3(center, right) / texel) * texel;
                const float cy = std::floor(dot3(center, up2) / texel) * texel;
                const float cz = dot3(center, travel);
                D3DMATRIX ortho = identityMatrix();
                ortho._11 = 1 / radius;
                ortho._41 = -cx / radius;
                ortho._22 = 1 / radius;
                ortho._42 = -cy / radius;
                ortho._33 = 1 / (2 * kDepthRange);
                ortho._43 = (kDepthRange - cz) / (2 * kDepthRange);
                sunProjection[cascade] = multiply(lightView, ortho);
                D3DMATRIX lookup = identityMatrix();
                lookup._11 = 0.25f;
                lookup._41 = 0.25f + 0.5f * cascade;
                lookup._22 = -0.5f;
                lookup._42 = 0.5f;
                const D3DMATRIX viewToShadow = multiply(multiply(inverseView, sunProjection[cascade]), lookup);
                std::memcpy(post.viewToShadow[cascade], &viewToShadow, 64);
            }
            renderShadows(camera, sunProjection);
            static std::string loggedMission;
            if (loggedMission != cw::options::g_missionName) {
                loggedMission = cw::options::g_missionName;
                logf("d3d11: shadows: sun travels (%.2f, %.2f, %.2f), %zu casters, %zu cameras (main %zu: %u draws), camera at (%.0f, %.0f, %.0f)",
                    travel[0], travel[1], travel[2], records.size(), cameras.size(), main, cameraUses[main], cameraPosition[0], cameraPosition[1],
                    cameraPosition[2]);
            }
            const float toSun[3] = {-travel[0], -travel[1], -travel[2]};
            const D3DMATRIX& v = camera.view;
            float sunView[3] = {toSun[0] * v._11 + toSun[1] * v._21 + toSun[2] * v._31, toSun[0] * v._12 + toSun[1] * v._22 + toSun[2] * v._32,
                toSun[0] * v._13 + toSun[1] * v._23 + toSun[2] * v._33};
            normalize3(sunView);
            post.sunView[0] = sunView[0];
            post.sunView[1] = sunView[1];
            post.sunView[2] = sunView[2];
            post.sunView[3] = 1;
            post.shadowParams[1] = 2 * fx(FxShadowNear) / kShadowSize * 4.0f;  // view units
            post.shadowParams[2] = 0.00012f;                                     // 0.6 units of sun depth
            post.shadowParams[3] = 0.08f;
        }
        for (int k = 0; k < 3; ++k) {
            post.shadowColor[k] = std::min(1.0f, look->shadow[k] * fx(night ? FxShadowDarknessNight : FxShadowDarknessDay));
        }

        // Effect lights, strongest first, in view space.
        updateTrackedLights(camera);
        std::vector<const TrackedLight*> sorted;
        for (const TrackedLight& light : trackedLights) {
            if (light.intensity > 0.02f) sorted.push_back(&light);
        }
        std::sort(sorted.begin(), sorted.end(), [](const TrackedLight* a, const TrackedLight* b) {
            return a->intensity * (a->color[0] + a->color[1] + a->color[2]) > b->intensity * (b->color[0] + b->color[1] + b->color[2]);
        });
        int count = 0;
        for (const TrackedLight* light : sorted) {
            if (count >= kMaxLights || !settings.lights) {
                break;
            }
            float position[3];
            transformPoint(light->position, camera.view, position);
            if (position[2] < -light->radius) {
                continue;  // behind the camera
            }
            post.lightPos[count][0] = position[0];
            post.lightPos[count][1] = position[1];
            post.lightPos[count][2] = position[2];
            post.lightPos[count][3] = light->radius * fx(FxLightReach);
            // Glows right at the camera (the player's own engines and shots) would wash out the screen.
            const float distance = std::sqrt(dot3(position, position));
            const float closeness = std::clamp((distance - 4.0f) / 20.0f, fx(FxPlayerGlow), 1.0f);
            const float fixtureBoost = light->lastSeen - light->firstSeen >= 30 ? fx(FxLightBrightness) : 1.0f;
            post.lightColor[count][0] = light->color[0] * light->intensity * closeness * fixtureBoost;
            post.lightColor[count][1] = light->color[1] * light->intensity * closeness * fixtureBoost;
            post.lightColor[count][2] = light->color[2] * light->intensity * closeness * fixtureBoost;
            ++count;
        }
        post.lighting[0] = exposure;
        post.lighting[1] = settings.ao ? std::min(1.0f, look->ao * fx(FxOcclusionStrength)) : 0.0f;
        post.lighting[2] = fx(night ? FxLightStrengthNight : FxLightStrengthDay);
        post.lighting[3] = static_cast<float>(count);
        post.grade[0] = look->contrast * fx(FxContrast);
        post.grade[1] = (night ? look->saturation * 0.85f : look->saturation) * fx(FxSaturation);
        post.grade[2] = settings.bloom ? (night ? fx(FxBloomNight) : look->bloom * fx(FxBloomDay)) : 0.0f;
        post.grade[3] = fx(night ? FxVignetteNight : FxVignetteDay);
        post.tint[0] = look->tint[0] * (night ? 0.80f : 1.0f);
        post.tint[1] = look->tint[1] * (night ? 0.92f : 1.0f);
        post.tint[2] = look->tint[2] * (night ? 1.15f : 1.0f);
        post.tint[3] = fx(night ? FxBloomThresholdNight : FxBloomThresholdDay);
        const float aoRadius = fx(FxOcclusionRadius);
        post.aoParams[0] = aoRadius;
        post.aoParams[1] = 1 / aoRadius;
        post.aoParams[2] = aoRadius * 0.02f;
        post.aoParams[3] = fx(night ? FxEffectGlowNight : FxEffectGlowDay);
        static const float debugView = std::getenv("CW_FX_DEBUG") != nullptr ? static_cast<float>(std::atoi(std::getenv("CW_FX_DEBUG"))) : 0.0f;
        post.debugParams[0] = debugView;
        post.surfaceParams[0] = settings.ao ? fx(FxBounce) : 0.0f;
        post.surfaceParams[1] = look->reflect * fx(FxReflections);
        post.surfaceParams[2] = fx(FxReflectionReach);
        upload(post);

        if (settings.ao) {
            ID3D11ShaderResourceView* aoViews[] = {nullptr, linear.srv};
            fullscreenPass(aoPS, ao, aoViews, 2);
            ID3D11ShaderResourceView* blurViews[] = {nullptr, linear.srv, nullptr, ao.srv};
            fullscreenPass(aoBlurPS, aoBlur, blurViews, 4);
        }
        if (post.surfaceParams[0] > 0) {
            ID3D11ShaderResourceView* bounceViews[] = {hdr.srv, linear.srv};
            fullscreenPass(bouncePS, bounce, bounceViews, 2);
            ID3D11ShaderResourceView* blurViews[] = {nullptr, linear.srv, nullptr, nullptr, nullptr, nullptr, bounce.srv};
            fullscreenPass(bounceBlurPS, bounceBlur, blurViews, 7);
        }
        ID3D11ShaderResourceView* lightingViews[] = {hdr.srv, linear.srv, shadowSrv, aoBlur.srv, nullptr, glow.srv, bounceBlur.srv};
        fullscreenPass(lightingPS, lit, lightingViews, 7);

        if (settings.bloom) {
            ID3D11ShaderResourceView* prefilterViews[] = {lit.srv};
            fullscreenPass(bloomPrefilterPS, bloom[0], prefilterViews, 1);
            for (int level = 1; level < kBloomLevels; ++level) {
                post.screen[2] = 1.0f / bloom[level - 1].width;
                post.screen[3] = 1.0f / bloom[level - 1].height;
                upload(post);
                ID3D11ShaderResourceView* views[] = {bloom[level - 1].srv};
                fullscreenPass(bloomDownPS, bloom[level], views, 1);
            }
            for (int level = kBloomLevels - 2; level >= 0; --level) {
                post.screen[2] = 1.0f / bloom[level + 1].width;
                post.screen[3] = 1.0f / bloom[level + 1].height;
                upload(post);
                ID3D11ShaderResourceView* views[] = {bloom[level + 1].srv};
                fullscreenPass(bloomUpPS, bloom[level], views, 1, additiveBlend);
            }
            post.screen[2] = 1.0f / width;
            post.screen[3] = 1.0f / height;
            upload(post);
        }
        ID3D11ShaderResourceView* tonemapViews[] = {lit.srv, nullptr, nullptr, nullptr, settings.bloom ? bloom[0].srv : nullptr};
        fullscreenPass(tonemapPS, settings.fxaa ? ldrTemp : ldr, tonemapViews, 5);
        if (debugView != 0) {
            ID3D11ShaderResourceView* debugViews[] = {lit.srv};
            fullscreenPass(copyPS, ldr, debugViews, 1);
        } else if (settings.fxaa) {
            ID3D11ShaderResourceView* fxaaViews[] = {ldrTemp.srv};
            fullscreenPass(fxaaPS, ldr, fxaaViews, 1);
        }
    }
    ID3D11ShaderResourceView* unbound[8] = {};
    g_ctx->PSSetShaderResources(0, 8, unbound);
    g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    // The rest of the frame (the 2D) draws onto the finished image.
    scene->texture = ldr.texture;
    scene->rtv = ldr.rtv;
}

}  // namespace

IDirect3DDevice9* createDevice11(HWND window, UINT width, UINT height, bool vsync) {
    auto* device = new Device11;
    if (!device->initialize(window, width, height, vsync)) {
        return nullptr;
    }
    g_instance = device;
    return device;
}

bool isDevice11(IDirect3DDevice9* device) {
    return device != nullptr && device == g_instance;
}

IDirect3DVertexShader9* createXboxVertexShader11(IDirect3DDevice9* device, const std::string& hlsl) {
    std::string source = hlsl;
    replaceOnce(source,
        "float4 c[192] : register(c0);\nfloat4 vpScale : register(c192);\nfloat4 vpOffset : register(c193);\n"
        "float4 texScale[4] : register(c194);\nfloat4 fogParams : register(c198);\n",
        "cbuffer XboxVertexConstants : register(b0) { float4 c[192]; float4 vpScale; float4 vpOffset; float4 texScale[4]; float4 fogParams; };\n");
    const std::size_t structStart = source.find("struct VSOut {");
    if (structStart != std::string::npos) {
        const std::size_t structEnd = source.find("};", structStart);
        source.replace(structStart, structEnd + 3 - structStart, commonInterpolantsHlsl(false));
    }
    return makeVertexShader(source, "xbox_vs");
}

IDirect3DPixelShader9* createXboxPixelShader11(IDirect3DDevice9* device, const std::string& hlsl) {
    std::string source = hlsl;
    replaceOnce(source, "float4 c[27] : register(c0);\n", "cbuffer XboxPixelConstants : register(b1) { float4 c[27]; };\n");
    const std::size_t structStart = source.find("struct PSIn {");
    if (structStart != std::string::npos) {
        const std::size_t structEnd = source.find("};", structStart);
        source.replace(structStart, structEnd + 3 - structStart, std::string(commonInterpolantsHlsl(false)) + "typedef VSOut PSIn;\n");
    }
    replaceOnce(source, "float4 main(PSIn i) : COLOR {", "float4 xboxMain(PSIn i) {");
    source += commonPixelHlsl();
    source += "PSOut main(PSIn i) { return finishPixel(xboxMain(i), i.fog, i.pos); }\n";
    return makePixelShader(source, "xbox_ps");
}

}  // namespace cw::d3d
