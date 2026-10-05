#include "d3d/TexturePack.h"

#include "Log.h"

#include <windows.h>
#include <wincodec.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace cw::d3d {

namespace {

std::string exeDirectory() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string directory = path;
    return directory.substr(0, directory.find_last_of("\\/") + 1);
}

const std::string& packFolder() {
    static const std::string folder = [] {
        const char* value = std::getenv("CW_TEXTURE_PACK");
        if (value != nullptr && std::strcmp(value, "0") == 0) {
            return std::string();
        }
        std::string path = value != nullptr ? std::string(value) : exeDirectory() + "upscaled";
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            return std::string();
        }
        logf("d3d: upscaled textures from %s", path.c_str());
        return path;
    }();
    return folder;
}

std::string hashName(std::uint64_t hash) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(hash));
    return name;
}

// BGRA texels of a PNG (any format WIC reads), or false.
bool decodeImage(const std::string& path, UINT& width, UINT& height, std::vector<std::uint8_t>& bgra) {
    static IWICImagingFactory* factory = [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IWICImagingFactory* created = nullptr;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&created));
        return created;
    }();
    if (factory == nullptr) {
        return false;
    }
    const std::wstring wide(path.begin(), path.end());
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool ok = SUCCEEDED(factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) &&
              SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
              SUCCEEDED(converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
              SUCCEEDED(converter->GetSize(&width, &height));
    if (ok) {
        bgra.resize(static_cast<std::size_t>(width) * height * 4);
        ok = SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(bgra.size()), bgra.data()));
    }
    if (converter != nullptr) converter->Release();
    if (frame != nullptr) frame->Release();
    if (decoder != nullptr) decoder->Release();
    return ok;
}

// The next mip level: a 2x2 box filter (alpha-weighted color, so cut-out edges do not darken).
std::vector<std::uint8_t> halve(const std::vector<std::uint8_t>& source, UINT width, UINT height, UINT& outWidth, UINT& outHeight) {
    outWidth = std::max(1u, width / 2);
    outHeight = std::max(1u, height / 2);
    std::vector<std::uint8_t> out(static_cast<std::size_t>(outWidth) * outHeight * 4);
    for (UINT y = 0; y < outHeight; ++y) {
        for (UINT x = 0; x < outWidth; ++x) {
            unsigned sum[4] = {};
            unsigned alpha = 0;
            for (UINT dy = 0; dy < 2; ++dy) {
                for (UINT dx = 0; dx < 2; ++dx) {
                    const UINT sx = std::min(width - 1, x * 2 + dx), sy = std::min(height - 1, y * 2 + dy);
                    const std::uint8_t* p = &source[(static_cast<std::size_t>(sy) * width + sx) * 4];
                    for (int c = 0; c < 3; ++c) sum[c] += p[c] * (p[3] + 1u);
                    alpha += p[3] + 1u;
                    sum[3] += p[3];
                }
            }
            std::uint8_t* q = &out[(static_cast<std::size_t>(y) * outWidth + x) * 4];
            for (int c = 0; c < 3; ++c) q[c] = static_cast<std::uint8_t>(sum[c] / alpha);
            q[3] = static_cast<std::uint8_t>(sum[3] / 4);
        }
    }
    return out;
}

}  // namespace

void dumpTextureForPack(std::uint64_t hash, UINT width, UINT height, D3DFORMAT format, const void* data, std::size_t size) {
    static const char* folder = std::getenv("CW_TEXTURE_DUMP");
    if (folder == nullptr || width < 32 || height < 32) {
        return;
    }
    static std::mutex lock;
    static std::unordered_set<std::uint64_t> written = [] {
        std::unordered_set<std::uint64_t> existing;
        if (FILE* index = std::fopen((std::string(folder) + "\\index.txt").c_str(), "r")) {
            unsigned long long value = 0;
            char rest[128];
            while (std::fscanf(index, "%llx %127[^\n]", &value, rest) == 2) existing.insert(value);
            std::fclose(index);
        }
        return existing;
    }();
    std::lock_guard guard(lock);
    if (!written.insert(hash).second) {
        return;
    }
    CreateDirectoryA(folder, nullptr);
    const std::string name = hashName(hash);
    if (FILE* file = std::fopen((std::string(folder) + "\\" + name + ".bin").c_str(), "wb")) {
        std::fwrite(data, 1, size, file);
        std::fclose(file);
    }
    if (FILE* index = std::fopen((std::string(folder) + "\\index.txt").c_str(), "a")) {
        std::fprintf(index, "%s %u %u %u\n", name.c_str(), width, height, static_cast<unsigned>(format));
        std::fclose(index);
    }
}

IDirect3DTexture9* loadPackTexture(IDirect3DDevice9* device, std::uint64_t hash) {
    const std::string& folder = packFolder();
    if (folder.empty() || hash == 0) {
        return nullptr;
    }
    const std::string path = folder + "\\" + hashName(hash) + ".png";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return nullptr;
    }
    UINT width = 0, height = 0;
    std::vector<std::uint8_t> level;
    if (!decodeImage(path, width, height, level)) {
        logf("d3d: could not read %s", path.c_str());
        return nullptr;
    }
    UINT levels = 1;
    for (UINT size = std::max(width, height); size > 1; size /= 2) ++levels;
    IDirect3DTexture9* texture = nullptr;
    if (FAILED(device->CreateTexture(width, height, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, nullptr))) {
        return nullptr;
    }
    UINT w = width, h = height;
    for (UINT index = 0; index < levels; ++index) {
        D3DLOCKED_RECT locked;
        if (SUCCEEDED(texture->LockRect(index, &locked, nullptr, 0))) {
            for (UINT y = 0; y < h; ++y) {
                std::memcpy(static_cast<std::uint8_t*>(locked.pBits) + static_cast<std::size_t>(y) * locked.Pitch, &level[static_cast<std::size_t>(y) * w * 4], w * 4);
            }
            texture->UnlockRect(index);
        }
        if (index + 1 < levels) {
            UINT nextWidth = 0, nextHeight = 0;
            level = halve(level, w, h, nextWidth, nextHeight);
            w = nextWidth;
            h = nextHeight;
        }
    }
    return texture;
}

}  // namespace cw::d3d
