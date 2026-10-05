/// @file TicoSlang.h
/// @brief RetroArch slang shader presets: parsing, preprocessing, compilation
/// and reflection. No Vulkan objects are created here; see TicoShaderChain.
///
/// Follows the slang spec (libretro/slang-shaders README): a .slangp preset
/// lists passes and textures, each .slang file holds both stages split by
/// `#pragma stage`, and shader inputs are matched by name ("semantics").
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace TicoSlang
{

enum class ScaleType
{
    Unset,
    Source,
    Viewport,
    Absolute,
};

enum class WrapMode
{
    ClampToBorder, // RetroArch's default
    ClampToEdge,
    Repeat,
    MirroredRepeat,
};

struct Parameter
{
    std::string id;
    std::string description;
    float initial = 0.0f;
    float minimum = 0.0f;
    float maximum = 1.0f;
    float step = 0.01f;
    float value = 0.0f; // current value (preset override applied)
};

/// One `#pragma parameter`-free shader with its stages separated.
struct ShaderSource
{
    std::string vertex;
    std::string fragment;
    std::string name;   // #pragma name
    std::string format; // #pragma format, e.g. "R16G16B16A16_SFLOAT"
    std::vector<Parameter> parameters;
};

struct Pass
{
    std::string path;
    std::string alias;          // preset aliasN, else #pragma name
    bool filterLinear = false;  // how this pass samples its Source
    bool filterSet = false;
    WrapMode wrap = WrapMode::ClampToBorder;
    bool mipmapInput = false;
    bool floatFramebuffer = false;
    bool srgbFramebuffer = false;
    uint32_t frameCountMod = 0;
    ScaleType scaleTypeX = ScaleType::Unset;
    ScaleType scaleTypeY = ScaleType::Unset;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
    ShaderSource source;
};

struct Texture
{
    std::string name;
    std::string path;
    bool linear = false;
    bool mipmap = false;
    WrapMode wrap = WrapMode::ClampToBorder;
};

struct Preset
{
    std::string path;
    std::vector<Pass> passes;
    std::vector<Texture> textures;
    std::vector<Parameter> parameters; // unique by id, in first-seen order
};

/// Parse a .slangp (following #reference) and preprocess every pass.
/// On failure returns false and fills `error`.
bool LoadPreset(const std::string &path, Preset &out, std::string &error);

/// Build a single-pass preset from source text (the built-in stock pass).
bool PresetFromSource(const std::string &name, const std::string &text, Preset &out, std::string &error);

//------------------------------------------------------------------------------
// Compilation + reflection
//------------------------------------------------------------------------------

enum class UniformKind
{
    Float,
    Int,
    UInt,
    Vec4,
    Mat4,
};

/// A member of the UBO or push-constant block, located by name.
struct UniformMember
{
    std::string name;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool inPushConstant = false;
};

struct SamplerBinding
{
    std::string name;
    uint32_t binding = 0;
};

struct Reflection
{
    bool hasUbo = false;
    uint32_t uboBinding = 0;
    uint32_t uboSize = 0;
    uint32_t pushSize = 0;
    std::vector<UniformMember> members;
    std::vector<SamplerBinding> samplers;
};

struct CompiledPass
{
    std::vector<uint32_t> vertexSpirv;
    std::vector<uint32_t> fragmentSpirv;
    Reflection reflection;
};

bool Compile(const ShaderSource &source, CompiledPass &out, std::string &error);

/// Release glslang's process-wide state.
void Shutdown();

} // namespace TicoSlang
