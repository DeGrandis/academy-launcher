#include "d3d/PixelShader.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>

namespace cw::d3d {

namespace {

// X_D3DPIXELSHADERDEF field indices (matching the D3DRS_PS* render-state order).
enum Def : int {
    AlphaInputs = 0, FinalAbcd = 8, FinalEfg = 9, Constant0 = 10, Constant1 = 18, AlphaOutputs = 26, RgbInputs = 34,
    CompareMode = 42, FinalConstant0 = 43, FinalConstant1 = 44, RgbOutputs = 45, CombinerCount = 53, TextureModes = 54,
    DotMapping = 55, InputTexture = 56,
};

enum TexMode : DWORD {
    ModeNone, ModeProject2D, ModeProject3D, ModeCube, ModePassthru, ModeClipPlane, ModeBump, ModeBumpLum, ModeBrdf,
    ModeDotST, ModeDotZW, ModeDotReflectDiffuse, ModeDotReflectSpecular, ModeDotStr3D, ModeDotStrCube, ModeDependentAR,
    ModeDependentGB, ModeDotProduct, ModeDotReflectSpecularConst,
};

using Program = std::array<DWORD, 60>;
std::map<Program, IDirect3DPixelShader9*> g_cache;

std::string registerName(DWORD reg, int stage) {
    switch (reg) {
    case 1: return stage < 0 ? "c[16]" : "c[" + std::to_string(stage) + "]";
    case 2: return stage < 0 ? "c[17]" : "c[" + std::to_string(8 + stage) + "]";
    case 3: return "fogc";
    case 4: return "v0";
    case 5: return "v1";
    case 8: return "t0";
    case 9: return "t1";
    case 10: return "t2";
    case 11: return "t3";
    case 12: return "r0";
    case 13: return "r1";
    case 14: return "sumv1r0";
    case 15: return "prodef";
    default: return "zero";
    }
}

std::string mapped(const std::string& value, DWORD mapping) {
    switch (mapping) {
    case 0: return "max(" + value + ", 0)";
    case 1: return "(1 - saturate(" + value + "))";
    case 2: return "(2 * max(" + value + ", 0) - 1)";
    case 3: return "(1 - 2 * max(" + value + ", 0))";
    case 4: return "(max(" + value + ", 0) - 0.5)";
    case 5: return "(0.5 - max(" + value + ", 0))";
    case 6: return value;
    default: return "(-" + value + ")";
    }
}

// One 8-bit combiner input; alpha selects .a, otherwise .rgb (RGB portion) or .b (alpha portion).
std::string input(DWORD code, bool alphaPortion, int stage) {
    const std::string reg = registerName(code & 0xF, stage);
    const bool alpha = (code & 0x10) != 0;
    const std::string channel = alphaPortion ? (alpha ? ".a" : ".b") : (alpha ? ".aaa" : ".rgb");
    return mapped(reg + channel, (code >> 5) & 7);
}

std::string outputMapping(const std::string& value, DWORD outputs) {
    switch ((outputs >> 15) & 7) {
    case 1: return "(" + value + " - 0.5)";
    case 2: return "(" + value + " * 2)";
    case 3: return "((" + value + " - 0.5) * 2)";
    case 4: return "(" + value + " * 4)";
    case 6: return "(" + value + " / 2)";
    default: return value;
    }
}

std::string combinerStage(const Program& p, int stage, bool muxMsb) {
    std::string code;
    for (int portion = 0; portion < 2; ++portion) {
        const bool alpha = portion == 1;
        const DWORD inputs = alpha ? p[AlphaInputs + stage] : p[RgbInputs + stage];
        const DWORD outputs = alpha ? p[AlphaOutputs + stage] : p[RgbOutputs + stage];
        const std::string type = alpha ? "float" : "float3";
        const std::string prefix = alpha ? "a" : "c";
        const std::string a = input(inputs >> 24, alpha, stage);
        const std::string b = input(inputs >> 16, alpha, stage);
        const std::string c = input(inputs >> 8, alpha, stage);
        const std::string d = input(inputs, alpha, stage);
        const bool abDot = !alpha && (outputs & 0x2000);
        const bool cdDot = !alpha && (outputs & 0x1000);
        code += "    " + type + " " + prefix + "ab = " + (abDot ? "dot(" + a + ", " + b + ").xxx" : a + " * " + b) + ";\n";
        code += "    " + type + " " + prefix + "cd = " + (cdDot ? "dot(" + c + ", " + d + ").xxx" : c + " * " + d) + ";\n";
        const std::string select = muxMsb ? "r0.a > 0.5" : "frac(r0.a * 255 / 2) > 0.25";
        code += "    " + type + " " + prefix + "sum = " + ((outputs & 0x4000) ? "(" + select + ") ? " + prefix + "cd : " + prefix + "ab" : prefix + "ab + " + prefix + "cd") + ";\n";
        for (const char* name : {"ab", "cd", "sum"}) {
            code += "    " + prefix + name + " = clamp(" + outputMapping(prefix + name, outputs) + ", -1, 1);\n";
        }
    }
    // Writes happen after both portions have read their inputs.
    for (int portion = 0; portion < 2; ++portion) {
        const bool alpha = portion == 1;
        const DWORD outputs = alpha ? p[AlphaOutputs + stage] : p[RgbOutputs + stage];
        const std::string prefix = alpha ? "a" : "c";
        const DWORD targets[3] = {(outputs >> 4) & 0xF, outputs & 0xF, (outputs >> 8) & 0xF};
        const char* names[3] = {"ab", "cd", "sum"};
        for (int index = 0; index < 3; ++index) {
            if (targets[index] == 0) {
                continue;
            }
            const std::string reg = registerName(targets[index], stage);
            code += "    " + reg + (alpha ? ".a" : ".rgb") + " = " + prefix + names[index] + ";\n";
            if (!alpha && index < 2 && (outputs & (index == 0 ? 0x40000 : 0x80000))) {
                code += "    " + reg + ".a = " + prefix + names[index] + ".b;\n";
            }
        }
    }
    return "    {\n" + code + "    }\n";
}

std::string dotSource(const Program& p, int stage, const std::string& source) {
    const DWORD mapping = (p[DotMapping] >> (4 * (stage - 1))) & 7;
    return mapping >= 1 && mapping <= 3 ? "(" + source + ".rgb * 2 - 1)" : source + ".rgb";
}

std::string textureStage(const Program& p, int stage, DWORD mode, std::string& samplers) {
    const std::string s = std::to_string(stage);
    const std::string t = "t" + s;
    const std::string coord = "i.t" + s;
    const bool cube = mode == ModeCube || mode == ModeDotStrCube || mode == ModeDotReflectDiffuse || mode == ModeDotReflectSpecular ||
                      mode == ModeDotReflectSpecularConst;
    if (mode != ModeNone && mode != ModePassthru && mode != ModeClipPlane && mode != ModeDotProduct) {
        samplers += std::string(cube ? "samplerCUBE" : "sampler2D") + " s" + s + " : register(s" + s + ");\n";
    }
    DWORD sourceStage = 0;
    if (stage == 2) sourceStage = (p[InputTexture] >> 16) & 1;
    if (stage == 3) sourceStage = (p[InputTexture] >> 20) & 3;
    const std::string source = "t" + std::to_string(sourceStage);
    const std::string bump = "c[" + std::to_string(18 + stage) + "]";
    const std::string uvBump = coord + ".xy + float2(" + bump + ".x * " + source + ".r + " + bump + ".z * " + source + ".g, " + bump + ".y * " +
                               source + ".r + " + bump + ".w * " + source + ".g)";
    switch (mode) {
    case ModeProject2D:
    case ModeProject3D:
        // Fixed-function vertices with 2D texture coordinates leave q (w) at 0 on the host; the NV2A treats a
        // missing q as 1, so only divide when q is set.
        return "    float4 " + t + " = tex2D(s" + s + ", " + coord + ".w != 0 ? " + coord + ".xy / " + coord + ".w : " + coord + ".xy);\n";
    case ModeCube:
        return "    float4 " + t + " = texCUBE(s" + s + ", " + coord + ".xyz);\n";
    case ModePassthru:
        return "    float4 " + t + " = saturate(" + coord + ");\n";
    case ModeClipPlane: {
        std::string code;
        static constexpr char kComponents[] = "xyzw";
        for (int component = 0; component < 4; ++component) {
            const bool greaterEqual = (p[CompareMode] >> (stage * 4 + component)) & 1;
            code += std::string("    clip(") + (greaterEqual ? "-" : "") + coord + "." + kComponents[component] + (greaterEqual ? " - 0.00001" : "") + ");\n";
        }
        return code + "    float4 " + t + " = 0;\n";
    }
    case ModeBump:
        return "    float4 " + t + " = tex2D(s" + s + ", " + uvBump + ");\n";
    case ModeBumpLum:
        return "    float4 " + t + " = tex2D(s" + s + ", " + uvBump + ");\n    " + t + ".rgb *= saturate(" + source + ".b * c[" +
               std::to_string(22 + stage) + "].x + c[" + std::to_string(22 + stage) + "].y);\n";
    case ModeDotProduct:
        return "    float4 " + t + " = dot(" + coord + ".xyz, " + dotSource(p, stage, source) + ").xxxx;\n";
    case ModeDotST:
        return "    float4 " + t + " = tex2D(s" + s + ", float2(t" + std::to_string(stage - 1) + ".x, dot(" + coord + ".xyz, " +
               dotSource(p, stage, source) + ")));\n";
    case ModeDotStr3D:
    case ModeDotStrCube:
        return "    float4 " + t + " = texCUBE(s" + s + ", float3(t1.x, t2.x, dot(" + coord + ".xyz, " + dotSource(p, stage, source) + ")));\n";
    case ModeDependentAR:
        return "    float4 " + t + " = tex2D(s" + s + ", " + source + ".ar);\n";
    case ModeDependentGB:
        return "    float4 " + t + " = tex2D(s" + s + ", " + source + ".gb);\n";
    default:
        if (mode != ModeNone) {
            logf("d3d: pixel shader texture mode %lu is approximated as empty", mode);
        }
        return "    float4 " + t + " = 0;\n";
    }
}

std::string generateHlsl(const Program& p) {
    std::string samplers;
    std::string body;
    for (int stage = 0; stage < 4; ++stage) {
        body += textureStage(p, stage, (p[TextureModes] >> (stage * 5)) & 0x1F, samplers);
    }
    body += "    float4 r0 = float4(0, 0, 0, t0.a), r1 = 0;\n";
    if (const char* debugOutput = std::getenv("CW_PS_OUTPUT")) {
        body += std::string("    if (c[26].a > 0) return float4((") + debugOutput + ").rgb, 1);\n";
    }
    const DWORD count = std::min<DWORD>(p[CombinerCount] & 0xFF, 8);
    const bool muxMsb = (p[CombinerCount] & 0x100) != 0;
    for (DWORD stage = 0; stage < count; ++stage) {
        body += combinerStage(p, static_cast<int>(stage), muxMsb);
    }
    if (p[FinalAbcd] == 0 && p[FinalEfg] == 0) {
        body += "    return saturate(r0);\n";
    } else {
        const DWORD abcd = p[FinalAbcd];
        const DWORD efg = p[FinalEfg];
        const std::string v1 = (efg & 0x40) ? "(1 - v1.rgb)" : "v1.rgb";
        const std::string r0 = (efg & 0x20) ? "(1 - r0.rgb)" : "r0.rgb";
        body += "    float4 sumv1r0 = float4(" + v1 + " + " + r0 + ", 0);\n";
        if (efg & 0x80) body += "    sumv1r0 = saturate(sumv1r0);\n";
        body += "    float4 prodef = float4(" + input(efg >> 24, false, -1) + " * " + input(efg >> 16, false, -1) + ", 0);\n";
        body += "    float3 fa = " + input(abcd >> 24, false, -1) + ";\n";
        body += "    float3 rgb = fa * " + input(abcd >> 16, false, -1) + " + (1 - fa) * " + input(abcd >> 8, false, -1) + " + " + input(abcd, false, -1) + ";\n";
        body += "    return saturate(float4(rgb, " + input(efg >> 8, true, -1) + "));\n";
    }
    return samplers + "float4 c[27] : register(c0);\n"
        "struct PSIn { float4 d0 : COLOR0; float4 d1 : COLOR1; float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; };\n"
        "float4 main(PSIn i) : COLOR {\n"
        "    float4 zero = 0, v0 = i.d0, v1 = i.d1, fogc = float4(c[26].rgb, 1);\n" + body + "}\n";
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

}  // namespace

IDirect3DPixelShader9* hostPixelShader(IDirect3DDevice9* device, const DWORD* definition) {
    Program program;
    std::copy(definition, definition + 60, program.begin());
    // Only the fields that shape the generated code form the cache key; constants are uniforms.
    for (int index = 0; index < 8; ++index) {
        program[Constant0 + index] = program[Constant1 + index] = 0;
    }
    program[FinalConstant0] = program[FinalConstant1] = 0;
    program[57] = program[58] = program[59] = 0;
    const auto found = g_cache.find(program);
    if (found != g_cache.end()) {
        return found->second;
    }
    IDirect3DPixelShader9* shader = nullptr;
    const std::string hlsl = generateHlsl(program);
    if (std::getenv("CW_SHADERLOG") != nullptr) {
        const std::string name = "ps_" + std::to_string(g_cache.size()) + ".hlsl";
        if (FILE* file = std::fopen(name.c_str(), "w")) {
            std::fputs(hlsl.c_str(), file);
            std::fclose(file);
        }
    }
    for (const char* profile : {"ps_2_a", "ps_2_b", "ps_2_0"}) {
        ID3DBlob* code = nullptr;
        ID3DBlob* errors = nullptr;
        const HRESULT result = D3DCompile(hlsl.data(), hlsl.size(), "xbox_ps", nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
        if (SUCCEEDED(result) && SUCCEEDED(device->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &shader))) {
            code->Release();
            break;
        }
        if (errors != nullptr) {
            logf("d3d: pixel shader compile failed (%s): %s", profile, static_cast<const char*>(errors->GetBufferPointer()));
            errors->Release();
        }
        if (code != nullptr) code->Release();
        shader = nullptr;
    }
    g_cache[program] = shader;
    return shader;
}

void pixelShaderConstants(const DWORD* definition, const DWORD* textureStates, int stageSize, float out[kPixelShaderConstantCount][4]) {
    const bool uniqueC0 = (definition[CombinerCount] & 0x1000) != 0;
    const bool uniqueC1 = (definition[CombinerCount] & 0x10000) != 0;
    for (int stage = 0; stage < 8; ++stage) {
        unpackColor(definition[Constant0 + (uniqueC0 ? stage : 0)], out[stage]);
        unpackColor(definition[Constant1 + (uniqueC1 ? stage : 0)], out[8 + stage]);
    }
    unpackColor(definition[FinalConstant0], out[16]);
    unpackColor(definition[FinalConstant1], out[17]);
    for (int stage = 0; stage < 4; ++stage) {
        const DWORD* ts = textureStates + stage * stageSize;
        out[18 + stage][0] = asFloat(ts[22]);
        out[18 + stage][1] = asFloat(ts[23]);
        out[18 + stage][2] = asFloat(ts[25]);
        out[18 + stage][3] = asFloat(ts[24]);
        out[22 + stage][0] = asFloat(ts[26]);
        out[22 + stage][1] = asFloat(ts[27]);
        out[22 + stage][2] = out[22 + stage][3] = 0.0f;
    }
}

}  // namespace cw::d3d
