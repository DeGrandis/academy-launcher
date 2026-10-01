#pragma once

#include <windows.h>
#include <d3d9.h>

#include <cstdint>

namespace cw::d3d {

// Xbox D3D resource headers as laid out by XDK 5233.
struct XResource {
    DWORD Common;
    DWORD Data;
    DWORD Lock;
};

struct XPixelContainer : XResource {
    DWORD Format;
    DWORD Size;
};

struct XSurface : XPixelContainer {
    XPixelContainer* Parent;
};

constexpr DWORD kCommonRefCountMask = 0x0000FFFF;
constexpr DWORD kCommonTypeVertexBuffer = 0x00000000;
constexpr DWORD kCommonTypeIndexBuffer = 0x00010000;
constexpr DWORD kCommonTypeTexture = 0x00040000;
constexpr DWORD kCommonTypeSurface = 0x00050000;
constexpr DWORD kCommonD3DCreated = 0x01000000;

constexpr DWORD kFormatDmaA = 0x00000001;
constexpr DWORD kFormatCubeMap = 0x00000004;

constexpr DWORD kFmtLinX8R8G8B8 = 0x1E;
constexpr DWORD kFmtLinD24S8 = 0x2E;

struct TextureLayout {
    DWORD format = 0;
    UINT width = 0;
    UINT height = 0;
    UINT depth = 1;
    UINT levels = 1;
    UINT pitch = 0;
    bool linear = false;
    bool compressed = false;
    bool cube = false;
    UINT bytesPerPixel = 0;
    UINT blockBytes = 0;
    bool known = false;
};

TextureLayout describe(const XPixelContainer* container);
UINT levelWidth(const TextureLayout& layout, UINT level);
UINT levelHeight(const TextureLayout& layout, UINT level);
UINT levelPitch(const TextureLayout& layout, UINT level);
UINT levelSize(const TextureLayout& layout, UINT level);
UINT levelOffset(const TextureLayout& layout, UINT level);
UINT faceSize(const TextureLayout& layout);
UINT bytesPerPixelOf(DWORD xboxFormat);
bool isLinearFormat(DWORD xboxFormat);
bool isCompressedFormat(DWORD xboxFormat);

void initializeTextures(IDirect3DDevice9* device);
IDirect3DBaseTexture9* hostTexture(const XPixelContainer* container);
void setRenderTargetOverride(const XPixelContainer* container, IDirect3DBaseTexture9* texture);
HWND gameWindow();

} // namespace cw::d3d
