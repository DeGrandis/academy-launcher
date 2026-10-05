#pragma once

#include <d3d9.h>

#include <cstddef>
#include <cstdint>

namespace cw::d3d {

// Upscaled texture pack (tools/upscale_textures.py): the game's textures, upscaled on the player's own machine, stored
// as <exe dir>\upscaled\<content hash>.png (CW_TEXTURE_PACK=<folder> elsewhere; CW_TEXTURE_PACK=0 off).
//
// CW_TEXTURE_DUMP=<folder> collects the textures the game uses (their top level as uploaded, <hash>.bin, listed in
// index.txt with size and format) for the upscaler; it can stay on while playing to pick up new ones.

// Saves a texture's top level for upscaling (when CW_TEXTURE_DUMP is set).
void dumpTextureForPack(std::uint64_t hash, UINT width, UINT height, D3DFORMAT format, const void* data, std::size_t size);

// The pack's version of a texture (hash = textureContentHash), with a full mip chain, or null.
IDirect3DTexture9* loadPackTexture(IDirect3DDevice9* device, std::uint64_t hash);

}  // namespace cw::d3d
