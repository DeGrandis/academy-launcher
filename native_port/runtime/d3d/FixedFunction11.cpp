#include "d3d/FixedFunction11.h"

#include <algorithm>
#include <string>

namespace cw::d3d {

namespace {

std::string number(int value) {
    return std::to_string(value);
}

// ---------------------------------------------------------------- vertex shaders

UINT texCoordComponents(DWORD fvf, UINT set) {
    static constexpr UINT kComponents[] = {2, 3, 4, 1};
    return kComponents[(fvf >> (16 + set * 2)) & 3];
}

UINT blendWeights(DWORD fvf) {
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZB1: return 1;
    case D3DFVF_XYZB2: return 2;
    case D3DFVF_XYZB3: return 3;
    case D3DFVF_XYZB4: return 4;
    case D3DFVF_XYZB5: return 5;
    default: return 0;
    }
}

std::string floatType(UINT components) {
    return components == 1 ? "float" : "float" + number(static_cast<int>(components));
}

// A texture coordinate set widened to four components the way Direct3D 9 feeds texture transforms: the first missing
// component is 1, the rest 0 (so a 2D coordinate becomes (u, v, 1, 0) and UV scrolls in _31/_32 work).
std::string widened(const std::string& value, UINT components) {
    switch (components) {
    case 1: return "float4(" + value + ", 1, 0, 0)";
    case 2: return "float4(" + value + ", 1, 0)";
    case 3: return "float4(" + value + ", 1)";
    default: return value;
    }
}

std::string lightCode(int index, int type, bool hasNormal, bool specular) {
    const std::string l = "lights[" + number(index) + "]";
    std::string code = "    {\n";
    if (type == 3) {  // directional
        code += "        float3 L = -" + l + ".direction.xyz;\n        float attenuation = 1;\n";
    } else {
        code += "        float3 toLight = " + l + ".position.xyz - viewPosition;\n";
        code += "        float d = length(toLight);\n        float3 L = toLight / max(d, 1e-6);\n";
        code += "        float attenuation = d <= " + l + ".range.x ? 1 / max(" + l + ".range.z + " + l + ".range.w * d + " + l + ".spot.x * d * d, 1e-6) : 0;\n";
        if (type == 2) {  // spot
            code += "        float rho = dot(-L, " + l + ".direction.xyz);\n";
            code += "        float spot = rho > " + l + ".spot.y ? 1 : rho <= " + l + ".spot.z ? 0 : pow(saturate((rho - " + l + ".spot.z) / max(" + l +
                    ".spot.y - " + l + ".spot.z, 1e-6)), " + l + ".range.y);\n";
            code += "        attenuation *= spot;\n";
        }
    }
    code += "        ambient += attenuation * " + l + ".ambient.rgb;\n";
    if (hasNormal) {
        code += "        float NdotL = dot(N, L);\n";
        code += "        diffuse += attenuation * max(NdotL, 0) * " + l + ".diffuse.rgb;\n";
        if (specular) {
            code += "        float3 H = normalize(L + V);\n";
            code += "        specular += NdotL > 0 ? attenuation * pow(max(dot(N, H), 0), max(materialPower.x, 1e-6)) * " + l + ".specular.rgb : 0;\n";
        }
    }
    return code + "    }\n";
}

}  // namespace

const char* commonInterpolantsHlsl(bool flat) {
    return flat ? "struct VSOut { float4 pos : SV_Position; nointerpolation float4 d0 : COLOR0; nointerpolation float4 d1 : COLOR1;\n"
                  "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };\n"
                : "struct VSOut { float4 pos : SV_Position; float4 d0 : COLOR0; float4 d1 : COLOR1;\n"
                  "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3; float fog : FOG; };\n";
}

const char* commonPixelHlsl() {
    return R"(cbuffer PixelCommon : register(b2) { float4 fogColor; float4 textureFactor; float4 alphaTest; };
// Color, and the view depth (clip w) of opaque 3D pixels for the frame effects (target 1; its write mask decides).
// Target 2 collects what additive effects add (the glow the frame effects keep bright at night).
struct PSOut { float4 color : SV_Target0; float depth : SV_Target1; float4 glow : SV_Target2; };
PSOut finishPixel(float4 color, float fog, float4 position) {
    // Alpha test (alphaTest.y: D3DCMPFUNC) on 8-bit alpha, as Direct3D 9 compares.
    float a = round(saturate(color.a) * 255);
    float r = round(alphaTest.x * 255);
    int func = (int)alphaTest.y;
    bool passed = func == 1 ? false : func == 2 ? a < r : func == 3 ? a == r : func == 4 ? a <= r : func == 5 ? a > r :
                func == 6 ? a != r : func == 7 ? a >= r : true;
    if (!passed) discard;
    if (alphaTest.z > 0) color.rgb = lerp(fogColor.rgb, color.rgb, saturate(fog));
    PSOut o;
    o.color = color;
    o.depth = position.w;
    o.glow = color;
    return o;
}
)";
}

std::string generateFfVertexShader(const FfVertexKey& key) {
    const DWORD fvf = key.fvf;
    const DWORD position = fvf & D3DFVF_POSITION_MASK;
    const bool pretransformed = position == D3DFVF_XYZRHW;
    const bool hasNormal = (fvf & D3DFVF_NORMAL) != 0;
    const bool hasDiffuse = (fvf & D3DFVF_DIFFUSE) != 0;
    const bool hasSpecular = (fvf & D3DFVF_SPECULAR) != 0;
    const UINT texCount = std::min<UINT>((fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT, 8);

    std::string hlsl = R"(cbuffer FixedFunction : register(b0) {
    row_major float4x4 world; row_major float4x4 view; row_major float4x4 projection; row_major float4x4 worldView;
    row_major float4x4 textureMatrix[4];
    float4 materialDiffuse; float4 materialAmbient; float4 materialSpecular; float4 materialEmissive; float4 materialPower;
    float4 globalAmbient;
    struct Light { float4 diffuse; float4 specular; float4 ambient; float4 position; float4 direction; float4 range; float4 spot; } lights[8];
    float4 targetSize; float4 fogParams;
};
)";
    hlsl += commonInterpolantsHlsl(false);
    hlsl += "struct VSIn {\n";
    hlsl += pretransformed ? "    float4 position : POSITION;\n" : "    float3 position : POSITION;\n";
    if (const UINT weights = blendWeights(fvf)) {
        hlsl += "    " + floatType(std::min<UINT>(weights, 4)) + " weights : BLENDWEIGHT;\n";
    }
    if (hasNormal) hlsl += "    float3 normal : NORMAL;\n";
    if (fvf & D3DFVF_PSIZE) hlsl += "    float pointSize : PSIZE;\n";
    if (hasDiffuse) hlsl += "    float4 color0 : COLOR0;\n";
    if (hasSpecular) hlsl += "    float4 color1 : COLOR1;\n";
    for (UINT set = 0; set < texCount; ++set) {
        hlsl += "    " + floatType(texCoordComponents(fvf, set)) + " tex" + number(set) + " : TEXCOORD" + number(set) + ";\n";
    }
    hlsl += "};\n";

    std::string body;
    body += "    VSOut o;\n";
    body += std::string("    float4 vertexDiffuse = ") + (hasDiffuse ? "i.color0" : "float4(1, 1, 1, 1)") + ";\n";
    body += std::string("    float4 vertexSpecular = ") + (hasSpecular ? "i.color1" : "float4(0, 0, 0, 0)") + ";\n";
    body += "    float3 viewPosition = 0;\n    float3 N = float3(0, 0, 1);\n";
    if (pretransformed) {
        // Screen pixels (Direct3D 9 pixel centers on integers) to clip space; w = 1 / rhw keeps texturing perspective-correct.
        body += "    float w = i.position.w != 0 ? 1 / i.position.w : 1;\n";
        body += "    float2 ndc = float2((i.position.x + 0.5) * targetSize.z * 2 - 1, 1 - (i.position.y + 0.5) * targetSize.w * 2);\n";
        body += "    o.pos = float4(ndc * w, i.position.z * w, w);\n";
        body += "    viewPosition = float3(0, 0, i.position.z);\n";
        body += "    o.d0 = saturate(vertexDiffuse);\n    o.d1 = saturate(vertexSpecular);\n";
    } else {
        body += "    float4 worldPosition = mul(float4(i.position, 1), world);\n";
        body += "    float4 viewPosition4 = mul(worldPosition, view);\n";
        body += "    viewPosition = viewPosition4.xyz;\n";
        body += "    o.pos = mul(viewPosition4, projection);\n";
        if (hasNormal) {
            body += "    N = normalize(mul(i.normal, (float3x3)worldView));\n";
        }
        if (key.lighting) {
            auto source = [&](int component, const char* material) -> std::string {
                const int from = key.colorVertex ? key.materialSource[component] : 0;
                if (from == 1 && hasDiffuse) return "i.color0";
                if (from == 2 && hasSpecular) return "i.color1";
                return material;
            };
            body += "    float4 matDiffuse = " + source(0, "materialDiffuse") + ";\n";
            body += "    float4 matAmbient = " + source(1, "materialAmbient") + ";\n";
            body += "    float4 matSpecular = " + source(2, "materialSpecular") + ";\n";
            body += "    float4 matEmissive = " + source(3, "materialEmissive") + ";\n";
            body += std::string("    float3 V = ") + (key.localViewer ? "-normalize(viewPosition)" : "float3(0, 0, -1)") + ";\n";
            body += "    float3 ambient = 0, diffuse = 0, specular = 0;\n";
            for (int index = 0; index < 8; ++index) {
                if (key.lightType[index] != 0) {
                    body += lightCode(index, key.lightType[index], hasNormal, key.specularEnable != 0);
                }
            }
            body += "    o.d0 = saturate(float4(matEmissive.rgb + globalAmbient.rgb * matAmbient.rgb + ambient * matAmbient.rgb + diffuse * matDiffuse.rgb, matDiffuse.a));\n";
            body += "    o.d1 = saturate(float4(specular * matSpecular.rgb, vertexSpecular.a));\n";
        } else {
            body += "    o.d0 = saturate(vertexDiffuse);\n    o.d1 = saturate(vertexSpecular);\n";
        }
    }

    // Texture coordinates per stage (the pixel shader reads stage n from tn).
    for (int stage = 0; stage < 4; ++stage) {
        const DWORD index = key.texCoordIndex[stage] & 0xFFFF;
        const DWORD mode = key.texCoordIndex[stage] & 0xFFFF0000;
        const DWORD transform = key.textureTransform[stage];
        const std::string out = "o.t" + number(stage);
        std::string input;
        switch (mode) {
        case D3DTSS_TCI_CAMERASPACENORMAL: input = "float4(N, 1)"; break;
        case D3DTSS_TCI_CAMERASPACEPOSITION: input = "float4(viewPosition, 1)"; break;
        case D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR: input = "float4(reflect(normalize(viewPosition), N), 1)"; break;
        case D3DTSS_TCI_SPHEREMAP:
            input = "float4(reflect(normalize(viewPosition), N).xy * 0.5 / sqrt(2 * (1 - reflect(normalize(viewPosition), N).z) + 1e-6) + 0.5, 1, 1)";
            break;
        default:
            input = index < texCount ? widened("i.tex" + number(static_cast<int>(index)), texCoordComponents(fvf, index)) : "float4(0, 0, 0, 1)";
            break;
        }
        if ((transform & 0xFF) != 0 && !pretransformed) {
            body += "    " + out + " = mul(" + input + ", textureMatrix[" + number(stage) + "]);\n";
        } else {
            body += "    " + out + " = " + input + ";\n";
        }
    }

    // Fog factor: table fog from view depth (or distance), otherwise the specular alpha (Direct3D 9 vertex fog).
    if (key.fogTableMode != D3DFOG_NONE) {
        body += std::string("    float fogDepth = ") + (key.rangeFog && !pretransformed ? "length(viewPosition)" : "abs(viewPosition.z)") + ";\n";
        switch (key.fogTableMode) {
        case D3DFOG_EXP: body += "    o.fog = exp(-fogParams.z * fogDepth);\n"; break;
        case D3DFOG_EXP2: body += "    o.fog = exp(-pow(fogParams.z * fogDepth, 2));\n"; break;
        default: body += "    o.fog = (fogParams.y - fogDepth) / max(fogParams.y - fogParams.x, 1e-6);\n"; break;
        }
    } else {
        body += std::string("    o.fog = ") + (hasSpecular ? "vertexSpecular.a" : "1") + ";\n";
    }
    body += "    o.fog = saturate(o.fog);\n";
    body += "    return o;\n";
    return hlsl + "VSOut main(VSIn i) {\n" + body + "}\n";
}

// ---------------------------------------------------------------- pixel shaders

namespace {

std::string argument(DWORD code, int stage) {
    std::string value;
    switch (code & D3DTA_SELECTMASK) {
    case D3DTA_DIFFUSE: value = "i.d0"; break;
    case D3DTA_CURRENT: value = stage == 0 ? "i.d0" : "current"; break;
    case D3DTA_TEXTURE: value = "t" + number(stage); break;
    case D3DTA_TFACTOR: value = "textureFactor"; break;
    case D3DTA_SPECULAR: value = "i.d1"; break;
    case D3DTA_TEMP: value = "temp"; break;
    default: value = "float4(1, 1, 1, 1)"; break;
    }
    if (code & D3DTA_ALPHAREPLICATE) value = value + ".aaaa";
    if (code & D3DTA_COMPLEMENT) value = "(1 - " + value + ")";
    return value;
}

// One texture-stage operation on float4 arguments (the caller takes .rgb or .a).
std::string operation(DWORD op, const std::string& a0, const std::string& a1, const std::string& a2, int stage) {
    const std::string t = "t" + number(stage);
    switch (op) {
    case D3DTOP_SELECTARG1: return a1;
    case D3DTOP_SELECTARG2: return a2;
    case D3DTOP_MODULATE: return a1 + " * " + a2;
    case D3DTOP_MODULATE2X: return "2 * " + a1 + " * " + a2;
    case D3DTOP_MODULATE4X: return "4 * " + a1 + " * " + a2;
    case D3DTOP_ADD: return a1 + " + " + a2;
    case D3DTOP_ADDSIGNED: return a1 + " + " + a2 + " - 0.5";
    case D3DTOP_ADDSIGNED2X: return "2 * (" + a1 + " + " + a2 + " - 0.5)";
    case D3DTOP_SUBTRACT: return a1 + " - " + a2;
    case D3DTOP_ADDSMOOTH: return a1 + " + " + a2 + " - " + a1 + " * " + a2;
    case D3DTOP_BLENDDIFFUSEALPHA: return "lerp(" + a2 + ", " + a1 + ", i.d0.a)";
    case D3DTOP_BLENDTEXTUREALPHA: return "lerp(" + a2 + ", " + a1 + ", " + t + ".a)";
    case D3DTOP_BLENDFACTORALPHA: return "lerp(" + a2 + ", " + a1 + ", textureFactor.a)";
    case D3DTOP_BLENDTEXTUREALPHAPM: return a1 + " + " + a2 + " * (1 - " + t + ".a)";
    case D3DTOP_BLENDCURRENTALPHA: return "lerp(" + a2 + ", " + a1 + ", " + (stage == 0 ? std::string("i.d0") : "current") + ".a)";
    case D3DTOP_MODULATEALPHA_ADDCOLOR: return "float4(" + a1 + ".rgb + " + a1 + ".a * " + a2 + ".rgb, " + a1 + ".a)";
    case D3DTOP_MODULATECOLOR_ADDALPHA: return "float4(" + a1 + ".rgb * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case D3DTOP_MODULATEINVALPHA_ADDCOLOR: return "float4((1 - " + a1 + ".a) * " + a2 + ".rgb + " + a1 + ".rgb, " + a1 + ".a)";
    case D3DTOP_MODULATEINVCOLOR_ADDALPHA: return "float4((1 - " + a1 + ".rgb) * " + a2 + ".rgb + " + a1 + ".a, " + a1 + ".a)";
    case D3DTOP_DOTPRODUCT3: return "saturate(4 * dot(" + a1 + ".rgb - 0.5, " + a2 + ".rgb - 0.5)).xxxx";
    case D3DTOP_MULTIPLYADD: return a0 + " + " + a1 + " * " + a2;
    case D3DTOP_LERP: return "lerp(" + a2 + ", " + a1 + ", " + a0 + ")";
    case D3DTOP_PREMODULATE: return a1;
    default: return stage == 0 ? "i.d0" : "current";  // bump mapping: the stage passes the current color on
    }
}

}  // namespace

std::string generateFfPixelShader(const FfPixelKey& key) {
    std::string declarations;
    std::string body = "    float4 current = i.d0, temp = 0;\n";
    for (int stage = 0; stage < 4; ++stage) {
        const FfPixelKey::Stage& s = key.stage[stage];
        if (s.colorOp == D3DTOP_DISABLE) {
            break;
        }
        const std::string n = number(stage);
        const std::string coord = "i.t" + n;
        if (s.textureType == 2) {
            declarations += "TextureCube tex" + n + " : register(t" + n + ");\nSamplerState samp" + n + " : register(s" + n + ");\n";
            body += "    float4 t" + n + " = tex" + n + ".Sample(samp" + n + ", " + coord + ".xyz);\n";
        } else if (s.textureType == 1) {
            declarations += "Texture2D tex" + n + " : register(t" + n + ");\nSamplerState samp" + n + " : register(s" + n + ");\n";
            std::string uv = coord + ".xy";
            if (s.projected >= 2) {
                static constexpr char kComponents[] = "xyzw";
                const std::string q = coord + "." + kComponents[s.projected - 1];
                uv = "(" + q + " != 0 ? " + coord + ".xy / " + q + " : " + coord + ".xy)";
            }
            body += "    float4 t" + n + " = tex" + n + ".Sample(samp" + n + ", " + uv + ");\n";
        } else {
            body += "    float4 t" + n + " = float4(1, 1, 1, 1);\n";
        }
        const std::string color = operation(s.colorOp, argument(s.colorArg[0], stage), argument(s.colorArg[1], stage), argument(s.colorArg[2], stage), stage);
        const std::string previousAlpha = stage == 0 ? "i.d0.a" : "current.a";
        std::string alpha = s.alphaOp == D3DTOP_DISABLE
            ? previousAlpha
            : "(" + operation(s.alphaOp, argument(s.alphaArg[0], stage), argument(s.alphaArg[1], stage), argument(s.alphaArg[2], stage), stage) + ").a";
        if (s.colorOp == D3DTOP_DOTPRODUCT3) {
            alpha = "(" + color + ").a";  // the dot product replicates into alpha
        }
        const std::string target = (s.resultArg & D3DTA_SELECTMASK) == D3DTA_TEMP ? "temp" : "current";
        body += "    {\n        float4 c = " + color + ";\n        float a = " + alpha + ";\n";
        body += "        " + target + " = saturate(float4(c.rgb, a));\n    }\n";
    }
    if (key.specularAdd) {
        body += "    current.rgb = saturate(current.rgb + i.d1.rgb);\n";
    }
    body += "    return finishPixel(current, i.fog, i.pos);\n";
    return declarations + commonInterpolantsHlsl(key.flatShading != 0) + commonPixelHlsl() + "PSOut main(VSOut i) {\n" + body + "}\n";
}

}  // namespace cw::d3d
