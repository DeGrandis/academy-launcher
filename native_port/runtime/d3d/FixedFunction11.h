#pragma once

#include <d3d9.h>

#include <cstdint>
#include <string>

namespace cw::d3d {

// Shader generation for the Direct3D 9 fixed-function pipeline on Direct3D 11 (Device11.cpp). A key holds exactly the
// state that changes the generated code; everything else (matrices, lights, material, fog range, alpha reference,
// texture factor) is constant-buffer data.

#pragma pack(push, 1)
struct FfVertexKey {
    DWORD fvf = 0;
    std::uint8_t lighting = 0;
    std::uint8_t colorVertex = 0;
    std::uint8_t specularEnable = 0;
    std::uint8_t localViewer = 0;
    std::uint8_t materialSource[4] = {};  // diffuse, ambient, specular, emissive: 0 material, 1 color1, 2 color2
    std::uint8_t lightType[8] = {};       // 0 off, 1 point, 2 spot, 3 directional
    std::uint8_t fogTableMode = 0;        // D3DFOGMODE
    std::uint8_t rangeFog = 0;
    std::uint8_t pad[2] = {};
    DWORD texCoordIndex[4] = {};          // D3DTSS_TEXCOORDINDEX per stage (index | D3DTSS_TCI_* mode)
    DWORD textureTransform[4] = {};       // D3DTSS_TEXTURETRANSFORMFLAGS per stage
};

struct FfPixelKey {
    struct Stage {
        std::uint8_t colorOp = D3DTOP_DISABLE;
        std::uint8_t alphaOp = D3DTOP_DISABLE;
        std::uint8_t colorArg[3] = {};  // arg0, arg1, arg2 (D3DTA_*)
        std::uint8_t alphaArg[3] = {};
        std::uint8_t resultArg = D3DTA_CURRENT;
        std::uint8_t textureType = 0;   // 0 none bound, 1 2D, 2 cube
        std::uint8_t projected = 0;     // 0, or the coordinate component count whose last element divides
    };
    Stage stage[4];
    std::uint8_t specularAdd = 0;
    std::uint8_t flatShading = 0;
    std::uint8_t pad[2] = {};
};
#pragma pack(pop)

// Shared by every vertex and pixel shader of the renderer (Xbox shaders included): the interpolants, and the pixel
// shader epilogue (fog and alpha test from the PixelCommon constant buffer, register b2).
const char* commonInterpolantsHlsl(bool flat);
const char* commonPixelHlsl();

std::string generateFfVertexShader(const FfVertexKey& key);
std::string generateFfPixelShader(const FfPixelKey& key);

// Constant buffer b0 of the generated fixed-function vertex shaders.
struct alignas(16) FfVertexConstants {
    float world[16];
    float view[16];
    float projection[16];
    float worldView[16];
    float textureMatrix[4][16];
    float materialDiffuse[4];
    float materialAmbient[4];
    float materialSpecular[4];
    float materialEmissive[4];
    float materialPower[4];
    float globalAmbient[4];
    struct Light {
        float diffuse[4];
        float specular[4];
        float ambient[4];
        float position[4];   // view space
        float direction[4];  // view space, normalized
        float range[4];      // range, falloff, attenuation0, attenuation1
        float spot[4];       // attenuation2, cos(theta / 2), cos(phi / 2), 0
    } lights[8];
    float targetSize[4];  // width, height, 1 / width, 1 / height of the bound render target
    float fog[4];         // start, end, density, 0
};

// Constant buffer b2 of every pixel shader.
struct alignas(16) PixelCommonConstants {
    float fogColor[4];
    float textureFactor[4];
    float alphaTest[4];  // reference (0..1), D3DCMPFUNC, fog enable, 0
};

}  // namespace cw::d3d
