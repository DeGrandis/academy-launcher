#pragma once

#include <d3d9.h>

namespace cw::d3d {

constexpr UINT kPixelShaderConstantCount = 27;

// Compiles (and caches) a host pixel shader for an X_D3DPIXELSHADERDEF (60 DWORDs).
IDirect3DPixelShader9* hostPixelShader(IDirect3DDevice9* device, const DWORD* definition);

// Fills c0-c7 stage C0, c8-c15 stage C1, c16/c17 final constants, c18-c21 bump matrices, c22-c25 bump luminance (c26 fog is the caller's).
void pixelShaderConstants(const DWORD* definition, const DWORD* textureStates, int stageSize, float out[kPixelShaderConstantCount][4]);

}  // namespace cw::d3d
