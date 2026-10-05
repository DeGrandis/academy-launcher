#pragma once

#include <d3d9.h>

#include <string>

namespace cw::d3d {

// The Direct3D 11 renderer (CW_RENDERER=d3d11): a device that implements the part of IDirect3DDevice9 the
// translation layer (Device.cpp, Textures.cpp, the shader translators) uses, on Direct3D 11. Fixed-function state is
// turned into generated shaders (FixedFunction11.cpp), so every draw goes through shaders this renderer owns.

// Creates the device for window at width x height (the back buffer the game draws into). Null on failure.
IDirect3DDevice9* createDevice11(HWND window, UINT width, UINT height, bool vsync);

// True when device is a Direct3D 11 device from createDevice11.
bool isDevice11(IDirect3DDevice9* device);

// Shaders from the Xbox shader translators' HLSL (Direct3D 9 dialect, adapted here). Null on failure.
IDirect3DVertexShader9* createXboxVertexShader11(IDirect3DDevice9* device, const std::string& hlsl);
IDirect3DPixelShader9* createXboxPixelShader11(IDirect3DDevice9* device, const std::string& hlsl);

}  // namespace cw::d3d
