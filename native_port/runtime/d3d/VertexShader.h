#pragma once

#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

namespace cw::d3d {

struct ShaderInput {
    UINT stream = 0;
    UINT offset = 0;
    DWORD type = 0;
    UINT reg = 0;
};

struct HostVertexShader {
    bool translated = false;
    std::vector<ShaderInput> inputs;
    IDirect3DVertexShader9* shader = nullptr;
    IDirect3DVertexDeclaration9* declaration = nullptr;
};

// Constant registers appended after the 192 Xbox constants.
constexpr UINT kViewportScaleConstant = 192;
constexpr UINT kViewportOffsetConstant = 193;
constexpr UINT kTexScaleConstant = 194;
constexpr UINT kFogConstant = 198;

// Translates an Xbox D3DVSD declaration and NV2A microcode program into a Direct3D 9 shader.
void translateVertexShader(IDirect3DDevice9* device, const std::vector<DWORD>& declaration,
    const std::vector<DWORD>& function, HostVertexShader& out);

// Expands one Xbox vertex attribute to float4 (missing components default to 0,0,0,1).
void convertAttribute(const std::uint8_t* source, DWORD type, float out[4]);

}  // namespace cw::d3d
