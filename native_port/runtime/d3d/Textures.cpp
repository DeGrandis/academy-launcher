#include "d3d/XboxD3D.h"

#include "Log.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cw::d3d {

namespace {

enum class Decode {
    Unsupported,
    Argb8888,
    Xrgb8888,
    Abgr8888,
    Bgra8888,
    Rgba8888,
    Argb1555,
    Xrgb1555,
    Argb4444,
    Rgb565,
    L8,
    Al8,
    A8,
    A8L8,
    L16,
    Yuy2,
    Uyvy,
    Dxt1,
    Dxt3,
    Dxt5,
};

struct FormatInfo {
    DWORD format;
    UINT bytesPerPixel;
    bool linear;
    Decode decode;
};

constexpr FormatInfo kFormats[] = {
    {0x00, 1, false, Decode::L8},
    {0x01, 1, false, Decode::Al8},
    {0x02, 2, false, Decode::Argb1555},
    {0x03, 2, false, Decode::Xrgb1555},
    {0x04, 2, false, Decode::Argb4444},
    {0x05, 2, false, Decode::Rgb565},
    {0x06, 4, false, Decode::Argb8888},
    {0x07, 4, false, Decode::Xrgb8888},
    {0x0B, 1, false, Decode::Unsupported},
    {0x0C, 0, false, Decode::Dxt1},
    {0x0E, 0, false, Decode::Dxt3},
    {0x0F, 0, false, Decode::Dxt5},
    {0x10, 2, true, Decode::Argb1555},
    {0x11, 2, true, Decode::Rgb565},
    {0x12, 4, true, Decode::Argb8888},
    {0x13, 1, true, Decode::L8},
    {0x19, 1, false, Decode::A8},
    {0x1A, 2, false, Decode::A8L8},
    {0x1B, 1, true, Decode::Al8},
    {0x1C, 2, true, Decode::Xrgb1555},
    {0x1D, 2, true, Decode::Argb4444},
    {0x1E, 4, true, Decode::Xrgb8888},
    {0x1F, 1, true, Decode::A8},
    {0x20, 2, true, Decode::A8L8},
    {0x24, 2, true, Decode::Yuy2},
    {0x25, 2, true, Decode::Uyvy},
    {0x2A, 4, false, Decode::Unsupported},
    {0x2C, 2, false, Decode::Unsupported},
    {0x2E, 4, true, Decode::Unsupported},
    {0x30, 2, true, Decode::Unsupported},
    {0x32, 2, false, Decode::L16},
    {0x35, 2, true, Decode::L16},
    {0x3A, 4, false, Decode::Abgr8888},
    {0x3B, 4, false, Decode::Bgra8888},
    {0x3C, 4, false, Decode::Rgba8888},
    {0x3F, 4, true, Decode::Abgr8888},
    {0x40, 4, true, Decode::Bgra8888},
    {0x41, 4, true, Decode::Rgba8888},
};

const FormatInfo* findFormat(DWORD format) {
    for (const FormatInfo& info : kFormats) {
        if (info.format == format) {
            return &info;
        }
    }
    return nullptr;
}

IDirect3DDevice9* g_device = nullptr;

struct CachedTexture {
    IDirect3DBaseTexture9* texture = nullptr;
    std::uint64_t hash = 0;
};
std::unordered_map<std::uint64_t, CachedTexture> g_cache;
std::unordered_map<const XPixelContainer*, IDirect3DBaseTexture9*> g_renderTargets;
std::unordered_set<DWORD> g_reportedFormats;

std::uint64_t hashBytes(const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 1469598103934665603ull;
    const std::size_t words = size / 8;
    const auto* values = reinterpret_cast<const std::uint64_t*>(data);
    for (std::size_t index = 0; index < words; ++index) {
        hash = (hash ^ values[index]) * 1099511628211ull;
    }
    for (std::size_t index = words * 8; index < size; ++index) {
        hash = (hash ^ data[index]) * 1099511628211ull;
    }
    return hash;
}

// NV2A swizzle interleaves U and V address bits (U first) up to the smaller dimension.
void unswizzle(const std::uint8_t* source, std::uint8_t* destination, UINT width, UINT height, UINT bytesPerPixel) {
    std::uint32_t maskU = 0;
    std::uint32_t maskV = 0;
    for (std::uint32_t size = 1, bit = 1; size < width || size < height; size <<= 1) {
        if (size < width) {
            maskU |= bit;
            bit <<= 1;
        }
        if (size < height) {
            maskV |= bit;
            bit <<= 1;
        }
    }

    std::uint32_t offsetV = 0;
    for (UINT y = 0; y < height; ++y) {
        std::uint32_t offsetU = 0;
        std::uint8_t* row = destination + static_cast<std::size_t>(y) * width * bytesPerPixel;
        for (UINT x = 0; x < width; ++x) {
            std::memcpy(row + x * bytesPerPixel, source + static_cast<std::size_t>(offsetU | offsetV) * bytesPerPixel, bytesPerPixel);
            offsetU = (offsetU - maskU) & maskU;
        }
        offsetV = (offsetV - maskV) & maskV;
    }
}

std::uint32_t expand(std::uint32_t value, int bits) {
    return bits == 0 ? 255 : (value * 255 + ((1u << bits) - 1) / 2) / ((1u << bits) - 1);
}

std::uint32_t argb(std::uint32_t a, std::uint32_t r, std::uint32_t g, std::uint32_t b) {
    return (a << 24) | (r << 16) | (g << 8) | b;
}

std::uint8_t clampByte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

void decodeRow(Decode decode, const std::uint8_t* source, std::uint32_t* destination, UINT width) {
    for (UINT x = 0; x < width; ++x) {
        switch (decode) {
        case Decode::Argb8888:
            destination[x] = reinterpret_cast<const std::uint32_t*>(source)[x];
            break;
        case Decode::Xrgb8888:
            destination[x] = reinterpret_cast<const std::uint32_t*>(source)[x] | 0xFF000000;
            break;
        case Decode::Abgr8888: {
            const std::uint32_t v = reinterpret_cast<const std::uint32_t*>(source)[x];
            destination[x] = (v & 0xFF00FF00) | ((v >> 16) & 0xFF) | ((v & 0xFF) << 16);
            break;
        }
        case Decode::Bgra8888: {
            const std::uint32_t v = reinterpret_cast<const std::uint32_t*>(source)[x];
            destination[x] = _byteswap_ulong(v);
            break;
        }
        case Decode::Rgba8888: {
            const std::uint32_t v = reinterpret_cast<const std::uint32_t*>(source)[x];
            destination[x] = (v >> 8) | (v << 24);
            break;
        }
        case Decode::Argb1555: {
            const std::uint16_t v = reinterpret_cast<const std::uint16_t*>(source)[x];
            destination[x] = argb((v & 0x8000) ? 255 : 0, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5));
            break;
        }
        case Decode::Xrgb1555: {
            const std::uint16_t v = reinterpret_cast<const std::uint16_t*>(source)[x];
            destination[x] = argb(255, expand((v >> 10) & 31, 5), expand((v >> 5) & 31, 5), expand(v & 31, 5));
            break;
        }
        case Decode::Argb4444: {
            const std::uint16_t v = reinterpret_cast<const std::uint16_t*>(source)[x];
            destination[x] = argb(expand(v >> 12, 4), expand((v >> 8) & 15, 4), expand((v >> 4) & 15, 4), expand(v & 15, 4));
            break;
        }
        case Decode::Rgb565: {
            const std::uint16_t v = reinterpret_cast<const std::uint16_t*>(source)[x];
            destination[x] = argb(255, expand(v >> 11, 5), expand((v >> 5) & 63, 6), expand(v & 31, 5));
            break;
        }
        case Decode::L8:
            destination[x] = argb(255, source[x], source[x], source[x]);
            break;
        case Decode::Al8:
            destination[x] = argb(source[x], source[x], source[x], source[x]);
            break;
        case Decode::A8:
            destination[x] = argb(source[x], 255, 255, 255);
            break;
        case Decode::A8L8: {
            const std::uint8_t l = source[x * 2];
            destination[x] = argb(source[x * 2 + 1], l, l, l);
            break;
        }
        case Decode::L16: {
            const std::uint8_t l = source[x * 2 + 1];
            destination[x] = argb(255, l, l, l);
            break;
        }
        case Decode::Yuy2:
        case Decode::Uyvy: {
            const std::uint8_t* pair = source + (x & ~1u) * 2;
            const bool yuy2 = decode == Decode::Yuy2;
            const int luma = yuy2 ? pair[(x & 1) * 2] : pair[(x & 1) * 2 + 1];
            const int u = (yuy2 ? pair[1] : pair[0]) - 128;
            const int v = (yuy2 ? pair[3] : pair[2]) - 128;
            const int c = luma - 16;
            destination[x] = argb(255, clampByte((298 * c + 409 * v + 128) >> 8), clampByte((298 * c - 100 * u - 208 * v + 128) >> 8),
                clampByte((298 * c + 516 * u + 128) >> 8));
            break;
        }
        default:
            destination[x] = 0xFFFF00FF;
            break;
        }
    }
}

D3DFORMAT compressedHostFormat(Decode decode) {
    switch (decode) {
    case Decode::Dxt1: return D3DFMT_DXT1;
    case Decode::Dxt3: return D3DFMT_DXT3;
    case Decode::Dxt5: return D3DFMT_DXT5;
    default: return D3DFMT_UNKNOWN;
    }
}

// Converts one mip level of Xbox texel data into a locked host level.
void uploadLevel(const FormatInfo& info, const TextureLayout& layout, UINT level, const std::uint8_t* source, D3DLOCKED_RECT& locked) {
    const UINT width = levelWidth(layout, level);
    const UINT height = levelHeight(layout, level);
    auto* destination = static_cast<std::uint8_t*>(locked.pBits);

    if (layout.compressed) {
        const UINT rowBytes = std::max(1u, (width + 3) / 4) * layout.blockBytes;
        const UINT rows = std::max(1u, (height + 3) / 4);
        for (UINT row = 0; row < rows; ++row) {
            std::memcpy(destination + row * locked.Pitch, source + row * rowBytes, rowBytes);
        }
        return;
    }

    std::vector<std::uint8_t> linear;
    const std::uint8_t* rows = source;
    UINT sourcePitch = levelPitch(layout, level);
    if (!layout.linear) {
        linear.resize(static_cast<std::size_t>(width) * height * info.bytesPerPixel);
        unswizzle(source, linear.data(), width, height, info.bytesPerPixel);
        rows = linear.data();
        sourcePitch = width * info.bytesPerPixel;
    }
    for (UINT y = 0; y < height; ++y) {
        decodeRow(info.decode, rows + static_cast<std::size_t>(y) * sourcePitch, reinterpret_cast<std::uint32_t*>(destination + y * locked.Pitch), width);
    }
}

IDirect3DBaseTexture9* createHostTexture(const XPixelContainer* container, const TextureLayout& layout, const FormatInfo& info) {
    const D3DFORMAT hostFormat = layout.compressed ? compressedHostFormat(info.decode) : D3DFMT_A8R8G8B8;
    const auto* base = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(container->Data));

    if (layout.cube) {
        IDirect3DCubeTexture9* cube = nullptr;
        if (FAILED(g_device->CreateCubeTexture(layout.width, layout.levels, 0, hostFormat, D3DPOOL_MANAGED, &cube, nullptr))) {
            return nullptr;
        }
        for (UINT face = 0; face < 6; ++face) {
            for (UINT level = 0; level < layout.levels; ++level) {
                D3DLOCKED_RECT locked;
                if (SUCCEEDED(cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, &locked, nullptr, 0))) {
                    uploadLevel(info, layout, level, base + face * faceSize(layout) + levelOffset(layout, level), locked);
                    cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level);
                }
            }
        }
        return cube;
    }

    IDirect3DTexture9* texture = nullptr;
    if (FAILED(g_device->CreateTexture(layout.width, layout.height, layout.levels, 0, hostFormat, D3DPOOL_MANAGED, &texture, nullptr))) {
        return nullptr;
    }
    for (UINT level = 0; level < layout.levels; ++level) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->LockRect(level, &locked, nullptr, 0))) {
            uploadLevel(info, layout, level, base + levelOffset(layout, level), locked);
            texture->UnlockRect(level);
        }
    }
    return texture;
}

} // namespace

bool isLinearFormat(DWORD xboxFormat) {
    const FormatInfo* info = findFormat(xboxFormat);
    return info != nullptr && info->linear;
}

bool isCompressedFormat(DWORD xboxFormat) {
    return xboxFormat == 0x0C || xboxFormat == 0x0E || xboxFormat == 0x0F;
}

UINT bytesPerPixelOf(DWORD xboxFormat) {
    const FormatInfo* info = findFormat(xboxFormat);
    return info == nullptr ? 4 : info->bytesPerPixel;
}

TextureLayout describe(const XPixelContainer* container) {
    TextureLayout layout;
    const DWORD format = container->Format;
    layout.format = (format >> 8) & 0xFF;
    const FormatInfo* info = findFormat(layout.format);
    layout.known = info != nullptr;
    layout.compressed = isCompressedFormat(layout.format);
    layout.blockBytes = layout.format == 0x0C ? 8 : 16;
    layout.bytesPerPixel = info == nullptr ? 4 : info->bytesPerPixel;
    layout.cube = (format & kFormatCubeMap) != 0;

    if (container->Size != 0) {
        layout.linear = true;
        layout.width = (container->Size & 0xFFF) + 1;
        layout.height = ((container->Size >> 12) & 0xFFF) + 1;
        layout.pitch = (((container->Size >> 24) & 0xFF) + 1) * 64;
        layout.levels = 1;
    } else {
        layout.linear = false;
        layout.width = 1u << ((format >> 20) & 0xF);
        layout.height = 1u << ((format >> 24) & 0xF);
        layout.depth = ((format >> 4) & 0xF) == 3 ? 1u << ((format >> 28) & 0xF) : 1;
        layout.levels = std::max<UINT>(1, (format >> 16) & 0xF);
    }
    return layout;
}

UINT levelWidth(const TextureLayout& layout, UINT level) {
    return std::max(1u, layout.width >> level);
}

UINT levelHeight(const TextureLayout& layout, UINT level) {
    return std::max(1u, layout.height >> level);
}

UINT levelPitch(const TextureLayout& layout, UINT level) {
    if (layout.linear) {
        return layout.pitch;
    }
    if (layout.compressed) {
        return std::max(1u, (levelWidth(layout, level) + 3) / 4) * layout.blockBytes;
    }
    return levelWidth(layout, level) * layout.bytesPerPixel;
}

UINT levelSize(const TextureLayout& layout, UINT level) {
    if (layout.compressed) {
        return levelPitch(layout, level) * std::max(1u, (levelHeight(layout, level) + 3) / 4);
    }
    return levelPitch(layout, level) * levelHeight(layout, level) * std::max(1u, layout.depth >> level);
}

UINT levelOffset(const TextureLayout& layout, UINT level) {
    UINT offset = 0;
    for (UINT index = 0; index < level; ++index) {
        offset += levelSize(layout, index);
    }
    return offset;
}

UINT faceSize(const TextureLayout& layout) {
    return (levelOffset(layout, layout.levels) + 127) & ~127u;
}

void initializeTextures(IDirect3DDevice9* device) {
    g_device = device;
}

void setRenderTargetOverride(const XPixelContainer* container, IDirect3DBaseTexture9* texture) {
    g_renderTargets[container] = texture;
}

IDirect3DBaseTexture9* hostTexture(const XPixelContainer* container) {
    if (container == nullptr || g_device == nullptr) {
        return nullptr;
    }
    auto renderTarget = g_renderTargets.find(container);
    if (renderTarget != g_renderTargets.end()) {
        return renderTarget->second;
    }

    const TextureLayout layout = describe(container);
    const FormatInfo* info = findFormat(layout.format);
    if (info == nullptr || info->decode == Decode::Unsupported || container->Data == 0) {
        if (g_reportedFormats.insert(layout.format).second) {
            logf("d3d: texture format 0x%02lX is not supported yet", layout.format);
        }
        return nullptr;
    }

    const auto* data = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(container->Data));
    const std::size_t dataSize = layout.cube ? static_cast<std::size_t>(faceSize(layout)) * 6 : levelOffset(layout, layout.levels);
    const std::uint64_t key = (static_cast<std::uint64_t>(container->Data) << 32) ^ (static_cast<std::uint64_t>(container->Format) * 31) ^ container->Size;
    const std::uint64_t hash = hashBytes(data, dataSize);

    CachedTexture& cached = g_cache[key];
    if (cached.texture != nullptr && cached.hash == hash) {
        return cached.texture;
    }
    if (cached.texture != nullptr) {
        cached.texture->Release();
    }
    cached.texture = createHostTexture(container, layout, *info);
    cached.hash = hash;
    return cached.texture;
}

} // namespace cw::d3d
