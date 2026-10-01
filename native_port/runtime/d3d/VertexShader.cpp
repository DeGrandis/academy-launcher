#include "d3d/VertexShader.h"

#include "Log.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cw::d3d {

namespace {

DWORD field(const DWORD* instruction, int word, int shift, int bits) {
    return (instruction[word] >> shift) & ((1u << bits) - 1);
}

enum Mux : DWORD { MuxTemp = 1, MuxInput = 2, MuxConstant = 3 };
enum MacOp : DWORD { MacNop, MacMov, MacMul, MacAdd, MacMad, MacDp3, MacDph, MacDp4, MacDst, MacMin, MacMax, MacSlt, MacSge, MacArl };
enum IluOp : DWORD { IluNop, IluMov, IluRcp, IluRcc, IluRsq, IluExp, IluLog, IluLit };

std::string mask(DWORD bits) {
    std::string result;
    if (bits & 8) result += 'x';
    if (bits & 4) result += 'y';
    if (bits & 2) result += 'z';
    if (bits & 1) result += 'w';
    return result;
}

// Operand 0 = A, 1 = B, 2 = C; the field layout follows the NV2A microcode encoding.
std::string operand(const DWORD* ins, int which) {
    static constexpr int kNeg[3][3] = {{1, 8, 1}, {2, 25, 1}, {2, 10, 1}};
    static constexpr int kSwizzle[3][2] = {{1, 0}, {2, 17}, {2, 2}};
    static constexpr int kMux[3][2] = {{2, 26}, {2, 11}, {3, 28}};
    const DWORD mux = field(ins, kMux[which][0], kMux[which][1], 2);
    DWORD reg = 0;
    if (which == 0) reg = field(ins, 2, 28, 4);
    else if (which == 1) reg = field(ins, 2, 13, 4);
    else reg = (field(ins, 2, 0, 2) << 2) | field(ins, 3, 30, 2);

    std::string text;
    if (mux == MuxTemp) {
        text = "r" + std::to_string(reg);
    } else if (mux == MuxInput) {
        text = "v" + std::to_string(field(ins, 1, 9, 4));
    } else {
        const DWORD constant = field(ins, 1, 13, 8);
        text = field(ins, 3, 1, 1) ? "c[a0 + " + std::to_string(constant) + "]" : "c[" + std::to_string(constant) + "]";
    }
    static constexpr char kComponents[] = "xyzw";
    const int word = kSwizzle[which][0];
    const int shift = kSwizzle[which][1];
    text += '.';
    text += kComponents[field(ins, word, shift + 6, 2)];
    text += kComponents[field(ins, word, shift + 4, 2)];
    text += kComponents[field(ins, word, shift + 2, 2)];
    text += kComponents[field(ins, word, shift, 2)];
    if (field(ins, kNeg[which][0], kNeg[which][1], kNeg[which][2])) {
        text = "-" + text;
    }
    return "(" + text + ")";
}

std::string macExpression(DWORD op, const std::string& a, const std::string& b, const std::string& c) {
    switch (op) {
    case MacMov: case MacArl: return a;
    case MacMul: return a + " * " + b;
    case MacAdd: return a + " + " + c;
    case MacMad: return a + " * " + b + " + " + c;
    case MacDp3: return "dot(" + a + ".xyz, " + b + ".xyz).xxxx";
    case MacDph: return "(dot(" + a + ".xyz, " + b + ".xyz) + " + b + ".w).xxxx";
    case MacDp4: return "dot(" + a + ", " + b + ").xxxx";
    case MacDst: return "float4(1, " + a + ".y * " + b + ".y, " + a + ".z, " + b + ".w)";
    case MacMin: return "min(" + a + ", " + b + ")";
    case MacMax: return "max(" + a + ", " + b + ")";
    case MacSlt: return "(float4)(" + a + " < " + b + ")";
    case MacSge: return "(float4)(" + a + " >= " + b + ")";
    default: return "float4(0, 0, 0, 0)";
    }
}

std::string iluExpression(DWORD op, const std::string& c) {
    switch (op) {
    case IluMov: return c;
    case IluRcp: return "(1.0f / " + c + ".x).xxxx";
    case IluRcc: return "rccx(" + c + ".x)";
    case IluRsq: return "rsqrt(abs(" + c + ".x)).xxxx";
    case IluExp: return "expx(" + c + ".x)";
    case IluLog: return "logx(" + c + ".x)";
    case IluLit: return "litx(" + c + ")";
    default: return "float4(0, 0, 0, 0)";
    }
}

std::string outputRegister(DWORD address) {
    switch (address) {
    case 0: return "r12";
    case 3: return "oD0";
    case 4: return "oD1";
    case 5: return "oFog";
    case 9: return "oT0";
    case 10: return "oT1";
    case 11: return "oT2";
    case 12: return "oT3";
    default: return "oUnused";
    }
}

constexpr char kPrologue[] = R"(
float4 c[192] : register(c0);
float4 vpScale : register(c192);
float4 vpOffset : register(c193);
float4 texScale[4] : register(c194);
float4 fogParams : register(c198);
float4 rccx(float s) { float r = 1.0f / s; float m = clamp(abs(r), 5.42101e-20f, 1.884467e19f); return (r < 0 ? -m : m).xxxx; }
float4 expx(float s) { float f = floor(s); return float4(exp2(f), s - f, exp2(s), 1); }
float4 logx(float s) { s = abs(s); float e = floor(log2(s)); return float4(e, s / exp2(e), log2(s), 1); }
float4 litx(float4 s) { float d = max(s.x, 0); float n = max(s.y, 0); float p = clamp(s.w, -128, 128); return float4(1, d, s.x > 0 ? pow(n, p) : 0, 1); }
struct VSOut { float4 pos : POSITION; float4 d0 : COLOR0; float4 d1 : COLOR1;
    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };
)";

std::string generateHlsl(const std::vector<DWORD>& function, const std::vector<ShaderInput>& inputs) {
    std::string inputList;
    std::string body;
    bool declared[16] = {};
    for (const ShaderInput& input : inputs) {
        if (!declared[input.reg]) {
            declared[input.reg] = true;
            inputList += (inputList.empty() ? "" : ", ") + std::string("float4 i") + std::to_string(input.reg) + " : TEXCOORD" + std::to_string(input.reg);
        }
    }
    for (int reg = 0; reg < 16; ++reg) {
        body += "    float4 v" + std::to_string(reg) + " = " + (declared[reg] ? "i" + std::to_string(reg) : std::string("float4(0, 0, 0, 1)")) + ";\n";
    }
    body += "    float4 r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0, r5 = 0, r6 = 0, r7 = 0, r8 = 0, r9 = 0, r10 = 0, r11 = 0, r12 = 0;\n";
    body += "    float4 oD0 = 1, oD1 = 0, oFog = 1, oT0 = float4(0, 0, 0, 1), oT1 = float4(0, 0, 0, 1), oT2 = float4(0, 0, 0, 1), oT3 = float4(0, 0, 0, 1), oUnused = 0;\n";
    body += "    int a0 = 0;\n";

    const std::size_t count = function.empty() ? 0 : (function.size() - 1) / 4;
    for (std::size_t index = 0; index < count; ++index) {
        const DWORD* ins = function.data() + 1 + index * 4;
        const DWORD ilu = field(ins, 1, 25, 3);
        const DWORD mac = field(ins, 1, 21, 4);
        const std::string a = operand(ins, 0);
        const std::string b = operand(ins, 1);
        const std::string c = operand(ins, 2);
        const DWORD outTemp = field(ins, 3, 20, 4);
        const DWORD macMask = field(ins, 3, 24, 4);
        const DWORD iluMask = field(ins, 3, 16, 4);
        const DWORD outMask = field(ins, 3, 12, 4);
        const bool toOutput = field(ins, 3, 11, 1) != 0;
        const DWORD outAddress = field(ins, 3, 3, 8);
        const bool outFromIlu = field(ins, 3, 2, 1) != 0;

        body += "    {\n";
        if (mac != MacNop) body += "        float4 m = " + macExpression(mac, a, b, c) + ";\n";
        if (ilu != IluNop) body += "        float4 l = " + iluExpression(ilu, c) + ";\n";
        if (mac != MacNop && mac != MacArl && macMask != 0) {
            body += "        r" + std::to_string(outTemp) + "." + mask(macMask) + " = m." + mask(macMask) + ";\n";
        }
        if (ilu != IluNop && iluMask != 0) {
            // A paired ILU result always lands in r1.
            const DWORD reg = mac != MacNop ? 1 : outTemp;
            body += "        r" + std::to_string(reg) + "." + mask(iluMask) + " = l." + mask(iluMask) + ";\n";
        }
        if (outMask != 0 && toOutput && ((outFromIlu && ilu != IluNop) || (!outFromIlu && mac != MacNop))) {
            body += "        " + outputRegister(outAddress) + "." + mask(outMask) + " = " + (outFromIlu ? "l." : "m.") + mask(outMask) + ";\n";
        }
        if (mac == MacArl) body += "        a0 = (int)floor(m.x + 0.0001f);\n";
        body += "    }\n";
        if (field(ins, 3, 0, 1)) {
            break;
        }
    }

    std::string hlsl = kPrologue;
    hlsl += "VSOut main(" + inputList + ") {\n" + body;
    hlsl += "    VSOut o;\n";
    hlsl += "    float4 p = r12;\n";
    // Xbox shaders emit viewport-scaled screen coordinates; undo it to get clip space.
    hlsl += "    p.xyz = (p.xyz - vpOffset.xyz) / vpScale.xyz;\n";
    hlsl += "    o.pos = float4(p.xyz * p.w, p.w);\n";
    hlsl += "    o.d0 = saturate(oD0); o.d1 = saturate(oD1);\n";
    hlsl += "    o.t0 = oT0 * texScale[0]; o.t1 = oT1 * texScale[1]; o.t2 = oT2 * texScale[2]; o.t3 = oT3 * texScale[3];\n";
    hlsl += "    float d = abs(oFog.x);\n";
    hlsl += "    float f = fogParams.w == 3 ? (fogParams.y - d) / (fogParams.y - fogParams.x)"
            " : fogParams.w == 1 ? exp(-fogParams.z * d) : fogParams.w == 2 ? exp(-pow(fogParams.z * d, 2)) : oFog.x;\n";
    hlsl += "    o.fog = saturate(f);\n";
    hlsl += "    return o;\n}\n";
    return hlsl;
}

UINT attributeSize(DWORD type) {
    const DWORD count = type >> 4;
    switch (type & 0xF) {
    case 0x0: return 4;
    case 0x1: case 0x5: return 2 * count;
    case 0x2: return type == 0x72 ? 12 : 4 * count;
    case 0x4: return count;
    case 0x6: return 4;
    default: return 0;
    }
}

}  // namespace

void convertAttribute(const std::uint8_t* source, DWORD type, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    const DWORD count = type >> 4;
    switch (type & 0xF) {
    case 0x0: {
        // D3DCOLOR is stored BGRA and read as RGBA.
        out[0] = source[2] / 255.0f;
        out[1] = source[1] / 255.0f;
        out[2] = source[0] / 255.0f;
        out[3] = source[3] / 255.0f;
        break;
    }
    case 0x1:
        for (DWORD i = 0; i < count && i < 4; ++i) {
            std::int16_t value;
            std::memcpy(&value, source + i * 2, 2);
            out[i] = std::max(-1.0f, value / 32767.0f);
        }
        break;
    case 0x5:
        for (DWORD i = 0; i < count && i < 4; ++i) {
            std::int16_t value;
            std::memcpy(&value, source + i * 2, 2);
            out[i] = value;
        }
        break;
    case 0x2:
        if (type == 0x72) {
            std::memcpy(out, source, 8);
            std::memcpy(out + 3, source + 8, 4);
        } else {
            std::memcpy(out, source, 4 * std::min<DWORD>(count, 4));
        }
        break;
    case 0x4:
        for (DWORD i = 0; i < count && i < 4; ++i) {
            out[i] = source[i] / 255.0f;
        }
        break;
    case 0x6: {
        DWORD packed;
        std::memcpy(&packed, source, 4);
        const auto x = static_cast<std::int32_t>(packed << 21) >> 21;
        const auto y = static_cast<std::int32_t>(packed << 10) >> 21;
        const auto z = static_cast<std::int32_t>(packed) >> 22;
        out[0] = std::max(-1.0f, x / 1023.0f);
        out[1] = std::max(-1.0f, y / 1023.0f);
        out[2] = std::max(-1.0f, z / 511.0f);
        break;
    }
    default:
        break;
    }
}

void translateVertexShader(IDirect3DDevice9* device, const std::vector<DWORD>& declaration,
    const std::vector<DWORD>& function, HostVertexShader& out) {
    out.translated = true;
    UINT stream = 0;
    UINT offsets[16] = {};
    for (std::size_t index = 0; index < declaration.size(); ++index) {
        const DWORD token = declaration[index];
        switch (token >> 29) {
        case 1:
            stream = token & 0xF;
            break;
        case 2:
            if (token & 0x10000000) {
                offsets[stream] += (token & 0x08000000) ? (token >> 16) & 0xFF : ((token >> 16) & 0xF) * 4;
            } else {
                const ShaderInput input{stream, offsets[stream], (token >> 16) & 0xFF, token & 0xF};
                offsets[stream] += attributeSize(input.type);
                out.inputs.push_back(input);
            }
            break;
        case 4:
            logf("d3d: vertex shader declaration constants are not supported");
            index += ((token >> 25) & 0xF) * 4;
            break;
        case 5:
            index += (token >> 24) & 0x1F;
            break;
        default:
            break;
        }
    }
    if (function.empty()) {
        logf("d3d: fixed-function vertex shader declarations are not supported yet");
        return;
    }

    std::vector<D3DVERTEXELEMENT9> elements;
    for (std::size_t index = 0; index < out.inputs.size(); ++index) {
        elements.push_back({0, static_cast<WORD>(index * 16), D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD,
            static_cast<BYTE>(out.inputs[index].reg)});
    }
    elements.push_back(D3DDECL_END());

    const std::string hlsl = generateHlsl(function, out.inputs);
    if (std::getenv("CW_SHADERLOG") != nullptr) {
        static int shaderNumber = 0;
        const std::string name = "vs_" + std::to_string(shaderNumber++) + ".hlsl";
        if (FILE* file = std::fopen(name.c_str(), "w")) {
            std::fprintf(file, "// declaration:");
            for (const DWORD token : declaration) {
                std::fprintf(file, " %08lX", token);
            }
            std::fprintf(file, "\n%s", hlsl.c_str());
            std::fclose(file);
        }
    }
    ID3DBlob* code = nullptr;
    ID3DBlob* errors = nullptr;
    const HRESULT result = D3DCompile(hlsl.data(), hlsl.size(), "xbox_vs", nullptr, nullptr, "main", "vs_2_a",
        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &errors);
    if (FAILED(result)) {
        logf("d3d: vertex shader compile failed: %s", errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
        logf("%s", hlsl.c_str());
    } else if (FAILED(device->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), &out.shader)) ||
               FAILED(device->CreateVertexDeclaration(elements.data(), &out.declaration))) {
        logf("d3d: could not create the host vertex shader");
    }
    if (code != nullptr) code->Release();
    if (errors != nullptr) errors->Release();
}

}  // namespace cw::d3d
