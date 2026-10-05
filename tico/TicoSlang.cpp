/// @file TicoSlang.cpp
/// @brief Slang preset parsing, preprocessing, compilation and reflection.

#include "TicoSlang.h"
#include "TicoLogger.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_reflect.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

#define SLANG_TAG "SLANG"

namespace TicoSlang
{
namespace
{

constexpr int kMaxIncludeDepth = 16;

std::string DirOf(const std::string &path)
{
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

/// Resolve `rel` against `dir`, collapsing "." and ".." so paths stay short.
std::string ResolvePath(const std::string &dir, const std::string &rel)
{
    if (rel.empty())
        return rel;
    std::string joined;
    if (rel[0] == '/' || rel.find(":/") != std::string::npos)
        joined = rel;
    else
        joined = dir + rel;

    // Keep a device prefix such as "sdmc:" out of the segment walk.
    std::string prefix;
    size_t colon = joined.find(":/");
    if (colon != std::string::npos)
    {
        prefix = joined.substr(0, colon + 1);
        joined = joined.substr(colon + 1);
    }
    const bool absolute = !joined.empty() && joined[0] == '/';

    std::vector<std::string> parts;
    std::stringstream ss(joined);
    std::string part;
    while (std::getline(ss, part, '/'))
    {
        if (part.empty() || part == ".")
            continue;
        if (part == ".." && !parts.empty() && parts.back() != "..")
            parts.pop_back();
        else
            parts.push_back(part);
    }
    std::string out = prefix + (absolute ? "/" : "");
    for (size_t i = 0; i < parts.size(); i++)
        out += (i ? "/" : "") + parts[i];
    return out;
}

bool ReadFile(const std::string &path, std::string &out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.good())
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string Trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string Unquote(const std::string &s)
{
    std::string t = Trim(s);
    if (t.size() >= 2 && t.front() == '"')
    {
        size_t end = t.find('"', 1);
        return end == std::string::npos ? t.substr(1) : t.substr(1, end - 1);
    }
    // Unquoted values end at an inline comment.
    size_t hash = t.find('#');
    return Trim(hash == std::string::npos ? t : t.substr(0, hash));
}

bool ParseBool(const std::string &v)
{
    return v == "true" || v == "1";
}

WrapMode ParseWrap(const std::string &v)
{
    if (v == "clamp_to_edge")
        return WrapMode::ClampToEdge;
    if (v == "repeat")
        return WrapMode::Repeat;
    if (v == "mirrored_repeat")
        return WrapMode::MirroredRepeat;
    return WrapMode::ClampToBorder;
}

ScaleType ParseScaleType(const std::string &v)
{
    if (v == "source")
        return ScaleType::Source;
    if (v == "viewport")
        return ScaleType::Viewport;
    if (v == "absolute")
        return ScaleType::Absolute;
    return ScaleType::Unset;
}

//------------------------------------------------------------------------------
// Config files (.slangp)
//------------------------------------------------------------------------------

/// A value plus the directory of the file that set it, since paths in a
/// #reference'd preset are relative to that preset, not to the one loaded.
struct ConfigValue
{
    std::string value;
    std::string dir;
};
using Config = std::map<std::string, ConfigValue>;

bool ParseConfig(const std::string &path, Config &config, std::string &error, int depth)
{
    if (depth > kMaxIncludeDepth)
    {
        error = "#reference nesting too deep at " + path;
        return false;
    }
    std::string text;
    if (!ReadFile(path, text))
    {
        error = "Cannot read " + path;
        return false;
    }
    const std::string dir = DirOf(path);

    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        std::string t = Trim(line);
        if (t.empty())
            continue;
        if (t.compare(0, 10, "#reference") == 0)
        {
            std::string ref = Unquote(t.substr(10));
            if (!ParseConfig(ResolvePath(dir, ref), config, error, depth + 1))
                return false;
            continue;
        }
        if (t[0] == '#' || t.compare(0, 2, "//") == 0)
            continue;
        size_t eq = t.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = Trim(t.substr(0, eq));
        config[key] = {Unquote(t.substr(eq + 1)), dir};
    }
    return true;
}

const ConfigValue *Find(const Config &c, const std::string &key)
{
    auto it = c.find(key);
    return it == c.end() ? nullptr : &it->second;
}

//------------------------------------------------------------------------------
// Slang sources
//------------------------------------------------------------------------------

bool ExpandIncludes(const std::string &path, std::string &out, std::string &error, int depth)
{
    if (depth > kMaxIncludeDepth)
    {
        error = "#include nesting too deep at " + path;
        return false;
    }
    std::string text;
    if (!ReadFile(path, text))
    {
        error = "Cannot read " + path;
        return false;
    }
    const std::string dir = DirOf(path);
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        std::string t = Trim(line);
        if (t.compare(0, 8, "#include") == 0)
        {
            std::string inc;
            if (!ExpandIncludes(ResolvePath(dir, Unquote(t.substr(8))), inc, error, depth + 1))
                return false;
            out += inc;
            continue;
        }
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        out += line;
        out += '\n';
    }
    return true;
}

bool ParseParameterPragma(const std::string &rest, Parameter &p)
{
    // ID "Description" initial minimum maximum [step]
    std::string t = Trim(rest);
    size_t sp = t.find_first_of(" \t");
    if (sp == std::string::npos)
        return false;
    p.id = t.substr(0, sp);
    t = Trim(t.substr(sp));
    if (t.empty() || t[0] != '"')
        return false;
    size_t end = t.find('"', 1);
    if (end == std::string::npos)
        return false;
    p.description = t.substr(1, end - 1);
    std::stringstream nums(t.substr(end + 1));
    float v[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    int n = 0;
    while (n < 4 && nums >> v[n])
        n++;
    if (n < 3)
        return false;
    p.initial = v[0];
    p.minimum = v[1];
    p.maximum = v[2];
    p.step = n >= 4 ? v[3] : 0.0f;
    if (p.step <= 0.0f)
        p.step = (p.maximum - p.minimum) / 100.0f;
    p.value = p.initial;
    return true;
}

bool Preprocess(const std::string &name, const std::string &text, ShaderSource &out, std::string &error)
{
    enum { Both, Vertex, Fragment } stage = Both;
    std::string version;
    std::stringstream ss(text);
    std::string line;
    while (std::getline(ss, line))
    {
        std::string t = Trim(line);
        if (version.empty() && t.compare(0, 8, "#version") == 0)
        {
            version = t;
            // Keep line numbers in glslang errors aligned with the file.
            out.vertex += '\n';
            out.fragment += '\n';
            continue;
        }
        if (t.compare(0, 7, "#pragma") == 0)
        {
            std::stringstream ps(t.substr(7));
            std::string kind;
            ps >> kind;
            std::string rest;
            std::getline(ps, rest);
            if (kind == "stage")
            {
                std::string s = Trim(rest);
                stage = s == "vertex" ? Vertex : s == "fragment" ? Fragment : stage;
                line.clear();
            }
            else if (kind == "name")
            {
                out.name = Trim(rest);
                line.clear();
            }
            else if (kind == "format")
            {
                out.format = Trim(rest);
                line.clear();
            }
            else if (kind == "parameter")
            {
                Parameter p;
                if (ParseParameterPragma(rest, p))
                {
                    auto dup = std::find_if(out.parameters.begin(), out.parameters.end(),
                                            [&](const Parameter &q) { return q.id == p.id; });
                    if (dup == out.parameters.end())
                        out.parameters.push_back(p);
                }
                else
                    LOG_WARN(SLANG_TAG, "%s: bad parameter pragma: %s", name.c_str(), t.c_str());
                line.clear();
            }
        }
        if (stage != Fragment)
            out.vertex += line + '\n';
        else
            out.vertex += '\n';
        if (stage != Vertex)
            out.fragment += line + '\n';
        else
            out.fragment += '\n';
    }
    if (version.empty())
    {
        error = name + ": missing #version";
        return false;
    }
    out.vertex = version + out.vertex;
    out.fragment = version + out.fragment;
    return true;
}

void MergeParameters(Preset &preset, const Config &config)
{
    for (Pass &pass : preset.passes)
    {
        for (Parameter &p : pass.source.parameters)
        {
            if (const ConfigValue *v = Find(config, p.id))
                p.value = std::clamp((float)atof(v->value.c_str()), p.minimum, p.maximum);
            auto it = std::find_if(preset.parameters.begin(), preset.parameters.end(),
                                   [&](const Parameter &q) { return q.id == p.id; });
            if (it == preset.parameters.end())
                preset.parameters.push_back(p);
        }
    }
}

//------------------------------------------------------------------------------
// glslang
//------------------------------------------------------------------------------

bool s_glslangReady = false;

bool CompileStage(EShLanguage lang, const std::string &src, std::vector<uint32_t> &spirv, std::string &error)
{
    if (!s_glslangReady)
    {
        glslang::InitializeProcess();
        s_glslangReady = true;
    }

    glslang::TShader shader(lang);
    const char *text = src.c_str();
    shader.setStrings(&text, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    const EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    if (!shader.parse(GetDefaultResources(), 100, false, messages))
    {
        error = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages))
    {
        error = program.getInfoLog();
        return false;
    }
    glslang::SpvOptions options;
    options.generateDebugInfo = false;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spirv, &options);
    return !spirv.empty();
}

//------------------------------------------------------------------------------
// SPIRV-Reflect
//------------------------------------------------------------------------------

void AddMember(Reflection &r, const char *name, uint32_t offset, uint32_t size, bool push)
{
    if (!name || !*name)
        return;
    for (const UniformMember &m : r.members)
        if (m.name == name && m.inPushConstant == push)
            return;
    r.members.push_back({name, offset, size, push});
}

bool Reflect(const std::vector<uint32_t> &spirv, Reflection &r, std::string &error)
{
    SpvReflectShaderModule module;
    if (spvReflectCreateShaderModule(spirv.size() * 4, spirv.data(), &module) != SPV_REFLECT_RESULT_SUCCESS)
    {
        error = "SPIR-V reflection failed";
        return false;
    }

    uint32_t count = 0;
    spvReflectEnumerateDescriptorBindings(&module, &count, nullptr);
    std::vector<SpvReflectDescriptorBinding *> bindings(count);
    spvReflectEnumerateDescriptorBindings(&module, &count, bindings.data());
    for (SpvReflectDescriptorBinding *b : bindings)
    {
        if (b->set != 0)
        {
            error = "Only descriptor set 0 is supported";
            spvReflectDestroyShaderModule(&module);
            return false;
        }
        if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
        {
            if (r.hasUbo && r.uboBinding != b->binding)
            {
                error = "More than one uniform buffer";
                spvReflectDestroyShaderModule(&module);
                return false;
            }
            r.hasUbo = true;
            r.uboBinding = b->binding;
            r.uboSize = std::max(r.uboSize, b->block.padded_size ? b->block.padded_size : b->block.size);
            for (uint32_t i = 0; i < b->block.member_count; i++)
            {
                const SpvReflectBlockVariable &m = b->block.members[i];
                AddMember(r, m.name, m.offset, m.size, false);
            }
        }
        else if (b->descriptor_type == SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
        {
            bool seen = false;
            for (const SamplerBinding &s : r.samplers)
                seen |= s.binding == b->binding;
            if (!seen)
                r.samplers.push_back({b->name ? b->name : "", b->binding});
        }
    }

    count = 0;
    spvReflectEnumeratePushConstantBlocks(&module, &count, nullptr);
    std::vector<SpvReflectBlockVariable *> blocks(count);
    spvReflectEnumeratePushConstantBlocks(&module, &count, blocks.data());
    for (SpvReflectBlockVariable *block : blocks)
    {
        r.pushSize = std::max(r.pushSize, block->padded_size ? block->padded_size : block->size);
        for (uint32_t i = 0; i < block->member_count; i++)
        {
            const SpvReflectBlockVariable &m = block->members[i];
            AddMember(r, m.name, m.offset, m.size, true);
        }
    }

    spvReflectDestroyShaderModule(&module);
    return true;
}

//------------------------------------------------------------------------------
// SPIR-V cache: compiling a big preset takes seconds on the Switch's CPU.
//------------------------------------------------------------------------------

#ifdef __SWITCH__
const char *kCacheDir = "sdmc:/tico/cache/shaders/";
#else
const char *kCacheDir = "cache/shaders/";
#endif
constexpr uint32_t kCacheMagic = 0x56505354; // "TSPV"
constexpr uint32_t kCacheVersion = 1;        // bump when compile options change

uint64_t Fnv1a(const std::string &s, uint64_t h = 1469598103934665603ull)
{
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string CachePath(const ShaderSource &source)
{
    uint64_t h = Fnv1a(source.vertex);
    h = Fnv1a(std::string(1, '\0') + source.fragment, h);
    char name[40];
    snprintf(name, sizeof(name), "%016llx.spv", (unsigned long long)h);
    return std::string(kCacheDir) + name;
}

bool ReadCache(const std::string &path, CompiledPass &out)
{
    std::ifstream f(path, std::ios::binary);
    uint32_t header[4];
    if (!f.read((char *)header, sizeof(header)) || header[0] != kCacheMagic || header[1] != kCacheVersion)
        return false;
    out.vertexSpirv.resize(header[2]);
    out.fragmentSpirv.resize(header[3]);
    return f.read((char *)out.vertexSpirv.data(), header[2] * 4) &&
           f.read((char *)out.fragmentSpirv.data(), header[3] * 4);
}

void WriteCache(const std::string &path, const CompiledPass &in)
{
    // Create the directory chain; failures just mean no caching.
    std::string dir = kCacheDir;
    for (size_t at = dir.find('/', dir.find(":/") != std::string::npos ? dir.find(":/") + 2 : 1);
         at != std::string::npos; at = dir.find('/', at + 1))
        mkdir(dir.substr(0, at).c_str(), 0777);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        uint32_t header[4] = {kCacheMagic, kCacheVersion, (uint32_t)in.vertexSpirv.size(),
                              (uint32_t)in.fragmentSpirv.size()};
        f.write((const char *)header, sizeof(header));
        f.write((const char *)in.vertexSpirv.data(), in.vertexSpirv.size() * 4);
        f.write((const char *)in.fragmentSpirv.data(), in.fragmentSpirv.size() * 4);
        if (!f)
            return;
    }
    rename(tmp.c_str(), path.c_str());
}

} // namespace

//==============================================================================
// Public API
//==============================================================================

bool LoadPreset(const std::string &path, Preset &out, std::string &error)
{
    out = {};
    out.path = path;
    Config config;
    if (!ParseConfig(path, config, error, 0))
        return false;

    const ConfigValue *shaders = Find(config, "shaders");
    int count = shaders ? atoi(shaders->value.c_str()) : 0;
    if (count <= 0)
    {
        error = path + ": no shaders";
        return false;
    }

    for (int i = 0; i < count; i++)
    {
        const std::string n = std::to_string(i);
        const ConfigValue *shader = Find(config, "shader" + n);
        if (!shader)
        {
            error = path + ": missing shader" + n;
            return false;
        }
        Pass pass;
        pass.path = ResolvePath(shader->dir, shader->value);

        auto get = [&](const std::string &key) -> const ConfigValue * { return Find(config, key + n); };
        if (auto v = get("alias"))
            pass.alias = v->value;
        if (auto v = get("filter_linear"))
        {
            pass.filterLinear = ParseBool(v->value);
            pass.filterSet = true;
        }
        if (auto v = get("wrap_mode"))
            pass.wrap = ParseWrap(v->value);
        else if (auto v2 = get("texture_wrap_mode"))
            pass.wrap = ParseWrap(v2->value);
        if (auto v = get("mipmap_input"))
            pass.mipmapInput = ParseBool(v->value);
        if (auto v = get("float_framebuffer"))
            pass.floatFramebuffer = ParseBool(v->value);
        if (auto v = get("srgb_framebuffer"))
            pass.srgbFramebuffer = ParseBool(v->value);
        if (auto v = get("frame_count_mod"))
            pass.frameCountMod = (uint32_t)atoi(v->value.c_str());

        if (auto v = get("scale_type"))
            pass.scaleTypeX = pass.scaleTypeY = ParseScaleType(v->value);
        if (auto v = get("scale_type_x"))
            pass.scaleTypeX = ParseScaleType(v->value);
        if (auto v = get("scale_type_y"))
            pass.scaleTypeY = ParseScaleType(v->value);
        if (auto v = get("scale"))
            pass.scaleX = pass.scaleY = (float)atof(v->value.c_str());
        if (auto v = get("scale_x"))
            pass.scaleX = (float)atof(v->value.c_str());
        if (auto v = get("scale_y"))
            pass.scaleY = (float)atof(v->value.c_str());

        std::string text;
        if (!ExpandIncludes(pass.path, text, error, 0) ||
            !Preprocess(pass.path, text, pass.source, error))
            return false;
        if (pass.alias.empty())
            pass.alias = pass.source.name;
        out.passes.push_back(std::move(pass));
    }

    if (const ConfigValue *textures = Find(config, "textures"))
    {
        std::stringstream ss(textures->value);
        std::string name;
        while (std::getline(ss, name, ';'))
        {
            name = Trim(name);
            if (name.empty())
                continue;
            const ConfigValue *p = Find(config, name);
            if (!p)
            {
                error = path + ": texture " + name + " has no path";
                return false;
            }
            Texture tex;
            tex.name = name;
            tex.path = ResolvePath(p->dir, p->value);
            if (auto v = Find(config, name + "_linear"))
                tex.linear = ParseBool(v->value);
            if (auto v = Find(config, name + "_mipmap"))
                tex.mipmap = ParseBool(v->value);
            if (auto v = Find(config, name + "_wrap_mode"))
                tex.wrap = ParseWrap(v->value);
            out.textures.push_back(tex);
        }
    }

    MergeParameters(out, config);
    return true;
}

bool PresetFromSource(const std::string &name, const std::string &text, Preset &out, std::string &error)
{
    out = {};
    out.path = name;
    Pass pass;
    pass.path = name;
    if (!Preprocess(name, text, pass.source, error))
        return false;
    pass.alias = pass.source.name;
    out.passes.push_back(std::move(pass));
    MergeParameters(out, Config());
    return true;
}

bool Compile(const ShaderSource &source, CompiledPass &out, std::string &error)
{
    out = {};
    const std::string cache = CachePath(source);
    if (!ReadCache(cache, out))
    {
        out = {};
        std::string log;
        if (!CompileStage(EShLangVertex, source.vertex, out.vertexSpirv, log))
        {
            error = "vertex: " + log;
            return false;
        }
        if (!CompileStage(EShLangFragment, source.fragment, out.fragmentSpirv, log))
        {
            error = "fragment: " + log;
            return false;
        }
        WriteCache(cache, out);
    }
    return Reflect(out.vertexSpirv, out.reflection, error) &&
           Reflect(out.fragmentSpirv, out.reflection, error);
}

void Shutdown()
{
    if (s_glslangReady)
        glslang::FinalizeProcess();
    s_glslangReady = false;
}

} // namespace TicoSlang
