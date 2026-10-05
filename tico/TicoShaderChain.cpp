/// @file TicoShaderChain.cpp
/// @brief Vulkan runtime for RetroArch slang presets. See TicoShaderChain.h.
///
/// Semantics follow RetroArch's Vulkan filter chain: a pass's filter/wrap/mip
/// settings describe how it samples its Source, so PassOutputN is sampled with
/// the settings of pass N+1; Original uses pass 0's; the last pass defaults to
/// viewport scale, and an explicitly scaled last pass gets a stock pass after
/// it so the result still lands at viewport size.

#include "TicoShaderChain.h"
#include "TicoLogger.h"
#include "deps/stb/stb_image.h"
// a frontend whose core already builds stb_image_write defines this
#ifndef TICO_STB_IMAGE_WRITE_EXTERNAL
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include "deps/stb/stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#define CHAIN_TAG "SHADER"

using TicoVulkan::Buffer;
using TicoVulkan::Image;

namespace
{

constexpr uint32_t kFrames = TicoVulkan::kFramesInFlight;

const char *kStockShader = R"(#version 450
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main()
{
    gl_Position = global.MVP * Position;
    vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main()
{
    FragColor = vec4(texture(Source, vTexCoord).rgb, 1.0);
}
)";

struct Target
{
    Image img;
    VkImageView rtView = VK_NULL_HANDLE; // mip 0, for the framebuffer
    VkFramebuffer fb = VK_NULL_HANDLE;
    ImTextureID imguiId = ImTextureID_Invalid;
};

void DestroyTarget(Target &t)
{
    if (!t.img.image)
        return;
    if (t.imguiId != ImTextureID_Invalid)
        TicoVulkan::UnregisterImage(t.imguiId);
    Target copy = t;
    t = {};
    TicoVulkan::DeferDestroy([copy]() mutable {
        VkDevice dev = TicoVulkan::Ctx().device;
        if (copy.fb)
            vkDestroyFramebuffer(dev, copy.fb, nullptr);
        if (copy.rtView)
            vkDestroyImageView(dev, copy.rtView, nullptr);
        TicoVulkan::DestroyImage(copy.img);
    });
}

struct PassRuntime
{
    TicoSlang::Pass cfg;
    TicoSlang::Reflection refl;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet sets[kFrames] = {};
    Buffer ubo[kFrames];
    std::vector<uint8_t> uboData;
    std::vector<uint8_t> pushData;

    Target output;
    Target feedback; // previous frame's output, when something reads it
    bool hasFeedback = false;
    bool genMips = false; // the next pass samples this output with mipmaps
    uint32_t outW = 0;
    uint32_t outH = 0;
};

struct Lut
{
    std::string name;
    Image img;
    bool linear = false;
    bool mipmap = false;
    TicoSlang::WrapMode wrap = TicoSlang::WrapMode::ClampToBorder;
};

VkFormat ParseFormat(const std::string &s)
{
    static const struct { const char *name; VkFormat fmt; } kFormats[] = {
        {"R8_UNORM", VK_FORMAT_R8_UNORM},
        {"R8_UINT", VK_FORMAT_R8_UINT},
        {"R8_SINT", VK_FORMAT_R8_SINT},
        {"R8G8_UNORM", VK_FORMAT_R8G8_UNORM},
        {"R8G8_UINT", VK_FORMAT_R8G8_UINT},
        {"R8G8_SINT", VK_FORMAT_R8G8_SINT},
        {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM},
        {"R8G8B8A8_UINT", VK_FORMAT_R8G8B8A8_UINT},
        {"R8G8B8A8_SINT", VK_FORMAT_R8G8B8A8_SINT},
        {"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB},
        {"A2B10G10R10_UNORM_PACK32", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
        {"A2B10G10R10_UINT_PACK32", VK_FORMAT_A2B10G10R10_UINT_PACK32},
        {"R16_UINT", VK_FORMAT_R16_UINT},
        {"R16_SINT", VK_FORMAT_R16_SINT},
        {"R16_SFLOAT", VK_FORMAT_R16_SFLOAT},
        {"R16G16_UINT", VK_FORMAT_R16G16_UINT},
        {"R16G16_SINT", VK_FORMAT_R16G16_SINT},
        {"R16G16_SFLOAT", VK_FORMAT_R16G16_SFLOAT},
        {"R16G16B16A16_UINT", VK_FORMAT_R16G16B16A16_UINT},
        {"R16G16B16A16_SINT", VK_FORMAT_R16G16B16A16_SINT},
        {"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT},
        {"R32_UINT", VK_FORMAT_R32_UINT},
        {"R32_SINT", VK_FORMAT_R32_SINT},
        {"R32_SFLOAT", VK_FORMAT_R32_SFLOAT},
        {"R32G32_UINT", VK_FORMAT_R32G32_UINT},
        {"R32G32_SINT", VK_FORMAT_R32G32_SINT},
        {"R32G32_SFLOAT", VK_FORMAT_R32G32_SFLOAT},
        {"R32G32B32A32_UINT", VK_FORMAT_R32G32B32A32_UINT},
        {"R32G32B32A32_SINT", VK_FORMAT_R32G32B32A32_SINT},
        {"R32G32B32A32_SFLOAT", VK_FORMAT_R32G32B32A32_SFLOAT},
    };
    for (const auto &f : kFormats)
        if (s == f.name)
            return f.fmt;
    return VK_FORMAT_UNDEFINED;
}

bool StartsWith(const std::string &s, const char *prefix, std::string &rest)
{
    size_t n = strlen(prefix);
    if (s.compare(0, n, prefix) != 0)
        return false;
    rest = s.substr(n);
    return true;
}

bool ParseIndex(const std::string &s, int &out)
{
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos)
        return false;
    out = atoi(s.c_str());
    return true;
}

uint32_t MipCount(uint32_t w, uint32_t h)
{
    uint32_t levels = 1;
    while ((w | h) >> levels)
        levels++;
    return levels;
}

void GenerateMips(VkCommandBuffer cmd, Image &img)
{
    // Level 0 is in SHADER_READ_ONLY after its render pass or upload.
    TicoVulkan::TransitionImage(cmd, img.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT,
                                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t w = (int32_t)img.width, h = (int32_t)img.height;
    for (uint32_t level = 1; level < img.mipLevels; level++)
    {
        VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
        blit.srcOffsets[1] = {w, h, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        vkCmdBlitImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        w = nw;
        h = nh;
    }
    TicoVulkan::TransitionImage(cmd, img.image, img.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

/// Clear a fresh image to black and leave it readable, so feedback and
/// history textures are valid on their first frame.
void ClearToReadable(VkCommandBuffer cmd, Image &img)
{
    TicoVulkan::TransitionImage(cmd, img.image, img.mipLevels, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue black = {};
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, img.mipLevels, 0, 1};
    vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    TicoVulkan::TransitionImage(cmd, img.image, img.mipLevels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

} // namespace

//==============================================================================
// Runtime: one loaded preset
//==============================================================================

struct TicoShaderChain::Runtime
{
    std::vector<PassRuntime> passes;
    std::vector<Lut> luts;
    std::vector<TicoSlang::Parameter> parameters;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint32_t historyDepth = 0; // previous source frames any pass reads

    ~Runtime()
    {
        VkDevice dev = TicoVulkan::Ctx().device;
        for (PassRuntime &p : passes)
        {
            DestroyTarget(p.output);
            DestroyTarget(p.feedback);
            for (Buffer &b : p.ubo)
                TicoVulkan::DeferDestroyBuffer(b);
            VkPipeline pipe = p.pipeline;
            VkPipelineLayout layout = p.layout;
            VkDescriptorSetLayout setLayout = p.setLayout;
            VkRenderPass rp = p.renderPass;
            VkShaderModule vs = p.vs, fs = p.fs;
            TicoVulkan::DeferDestroy([dev, pipe, layout, setLayout, rp, vs, fs]() {
                vkDestroyPipeline(dev, pipe, nullptr);
                vkDestroyPipelineLayout(dev, layout, nullptr);
                vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
                vkDestroyRenderPass(dev, rp, nullptr);
                vkDestroyShaderModule(dev, vs, nullptr);
                vkDestroyShaderModule(dev, fs, nullptr);
            });
        }
        for (Lut &l : luts)
            TicoVulkan::DeferDestroyImage(l.img);
        if (pool)
        {
            VkDescriptorPool p = pool;
            TicoVulkan::DeferDestroy([dev, p]() { vkDestroyDescriptorPool(dev, p, nullptr); });
        }
    }
};

namespace
{

bool CreateShaderModule(const std::vector<uint32_t> &spirv, VkShaderModule &out)
{
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    return vkCreateShaderModule(TicoVulkan::Ctx().device, &ci, nullptr, &out) == VK_SUCCESS;
}

bool CreatePassPipeline(PassRuntime &p, const TicoSlang::CompiledPass &compiled, std::string &error)
{
    VkDevice dev = TicoVulkan::Ctx().device;
    if (!CreateShaderModule(compiled.vertexSpirv, p.vs) || !CreateShaderModule(compiled.fragmentSpirv, p.fs))
    {
        error = "vkCreateShaderModule failed";
        return false;
    }

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    if (p.refl.hasUbo)
        bindings.push_back({p.refl.uboBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr});
    for (const TicoSlang::SamplerBinding &s : p.refl.samplers)
        bindings.push_back({s.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr});
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = (uint32_t)bindings.size();
    dl.pBindings = bindings.data();
    vkCreateDescriptorSetLayout(dev, &dl, nullptr, &p.setLayout);

    VkPushConstantRange push = {stages, 0, p.refl.pushSize};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.setLayout;
    pl.pushConstantRangeCount = p.refl.pushSize ? 1 : 0;
    pl.pPushConstantRanges = &push;
    vkCreatePipelineLayout(dev, &pl, nullptr, &p.layout);

    VkAttachmentDescription color = {};
    color.format = p.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    // Earlier reads of this image (last frame's feedback, ImGui) must finish
    // before it is overwritten, and later passes must see what it wrote.
    VkSubpassDependency deps[2] = {};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                           VK_PIPELINE_STAGE_TRANSFER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    VkRenderPassCreateInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    if (vkCreateRenderPass(dev, &rp, nullptr, &p.renderPass) != VK_SUCCESS)
    {
        error = "vkCreateRenderPass failed";
        return false;
    }

    VkPipelineShaderStageCreateInfo shaderStages[2] = {};
    shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    shaderStages[0].module = p.vs;
    shaderStages[0].pName = "main";
    shaderStages[1] = shaderStages[0];
    shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    shaderStages[1].module = p.fs;

    VkVertexInputBindingDescription vb = {0, 6 * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription va[2] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, 4 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = 2;
    vi.pVertexAttributeDescriptions = va;

    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blendAtt = {};
    blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blendAtt;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = shaderStages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &dyn;
    gp.layout = p.layout;
    gp.renderPass = p.renderPass;
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, nullptr, &p.pipeline) != VK_SUCCESS)
    {
        error = "vkCreateGraphicsPipelines failed";
        return false;
    }

    p.uboData.assign(p.refl.uboSize, 0);
    p.pushData.assign(p.refl.pushSize, 0);
    if (p.refl.hasUbo)
        for (Buffer &b : p.ubo)
            if (!TicoVulkan::CreateBuffer(b, std::max<uint32_t>(p.refl.uboSize, 16),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT))
            {
                error = "uniform buffer allocation failed";
                return false;
            }
    return true;
}

bool LoadLut(VkCommandBuffer cmd, Lut &lut, const std::string &path, std::string &error)
{
    int w, h, ch;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels)
    {
        error = "Cannot load texture " + path;
        return false;
    }
    const uint32_t levels = lut.mipmap ? MipCount(w, h) : 1;
    Buffer staging;
    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    bool ok = TicoVulkan::CreateImage(lut.img, w, h, VK_FORMAT_R8G8B8A8_UNORM,
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                      levels) &&
              TicoVulkan::CreateBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!ok)
    {
        stbi_image_free(pixels);
        TicoVulkan::DestroyImage(lut.img);
        error = "Texture allocation failed for " + path;
        return false;
    }
    memcpy(staging.mapped, pixels, (size_t)size);
    stbi_image_free(pixels);

    TicoVulkan::TransitionImage(cmd, lut.img.image, levels, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, lut.img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TicoVulkan::TransitionImage(cmd, lut.img.image, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
    lut.img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (levels > 1)
        GenerateMips(cmd, lut.img);
    TicoVulkan::DeferDestroyBuffer(staging);
    return true;
}

} // namespace

//==============================================================================
// TicoShaderChain
//==============================================================================

TicoShaderChain::TicoShaderChain() = default;

TicoShaderChain::~TicoShaderChain()
{
    Shutdown();
}

bool TicoShaderChain::Init()
{
    // Full-screen quad in RetroArch's convention: positions and texcoords both
    // span 0..1 with (0,0) top-left; the MVP maps that onto clip space.
    // The quad four times, its texture turned 0, 90, 180 and 270 degrees
    // counter-clockwise: the last pass draws the one SetRotation picked.
    const float quad[] = {
        0, 0, 0, 1, 0, 0,  1, 0, 0, 1, 1, 0,  0, 1, 0, 1, 0, 1,  1, 1, 0, 1, 1, 1,
        0, 0, 0, 1, 1, 0,  1, 0, 0, 1, 1, 1,  0, 1, 0, 1, 0, 0,  1, 1, 0, 1, 0, 1,
        0, 0, 0, 1, 1, 1,  1, 0, 0, 1, 0, 1,  0, 1, 0, 1, 1, 0,  1, 1, 0, 1, 0, 0,
        0, 0, 0, 1, 0, 1,  1, 0, 0, 1, 0, 0,  0, 1, 0, 1, 1, 1,  1, 1, 0, 1, 1, 0,
    };
    if (!TicoVulkan::CreateBuffer(m_quadBuffer, sizeof(quad), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT))
        return false;
    memcpy(m_quadBuffer.mapped, quad, sizeof(quad));

    VkDevice dev = TicoVulkan::Ctx().device;
    const VkSamplerAddressMode wraps[4] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for (int linear = 0; linear < 2; linear++)
        for (int mip = 0; mip < 2; mip++)
            for (int wrap = 0; wrap < 4; wrap++)
            {
                VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
                ci.magFilter = ci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
                ci.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
                ci.addressModeU = ci.addressModeV = ci.addressModeW = wraps[wrap];
                ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
                ci.maxLod = mip ? VK_LOD_CLAMP_NONE : 0.0f;
                vkCreateSampler(dev, &ci, nullptr, &m_samplers[linear][mip][wrap]);
            }

    const unsigned char black[4] = {0, 0, 0, 255};
    Buffer staging;
    if (!TicoVulkan::CreateImage(m_dummy, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !TicoVulkan::CreateBuffer(staging, 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return false;
    memcpy(staging.mapped, black, 4);
    VkCommandBuffer cmd = TicoVulkan::BeginOneShot();
    TicoVulkan::TransitionImage(cmd, m_dummy.image, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {1, 1, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_dummy.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TicoVulkan::TransitionImage(cmd, m_dummy.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    TicoVulkan::EndOneShot(cmd);
    TicoVulkan::DestroyBuffer(staging);
    m_dummy.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    std::string error;
    if (!LoadPreset("", error))
    {
        LOG_ERROR(CHAIN_TAG, "Built-in pass failed: %s", error.c_str());
        return false;
    }
    return true;
}

void TicoShaderChain::Shutdown()
{
    if (!TicoVulkan::Ctx().device)
        return;
    m_runtime.reset();
    for (Buffer &b : m_staging)
        TicoVulkan::DeferDestroyBuffer(b);
    TicoVulkan::DeferDestroyBuffer(m_quadBuffer);
    TicoVulkan::DeferDestroyImage(m_source);
    for (Image &h : m_history)
        TicoVulkan::DeferDestroyImage(h);
    m_history.clear();
    TicoVulkan::DeferDestroyImage(m_dummy);
    VkDevice dev = TicoVulkan::Ctx().device;
    for (auto &a : m_samplers)
        for (auto &b : a)
            for (VkSampler &s : b)
                if (s)
                {
                    VkSampler copy = s;
                    TicoVulkan::DeferDestroy([dev, copy]() { vkDestroySampler(dev, copy, nullptr); });
                    s = VK_NULL_HANDLE;
                }
    m_outputId = ImTextureID_Invalid;
}

bool TicoShaderChain::LoadPreset(const std::string &path, std::string &error)
{
    TicoSlang::Preset preset;
    if (path.empty())
    {
        if (!TicoSlang::PresetFromSource("stock", kStockShader, preset, error))
            return false;
    }
    else
    {
        if (!TicoSlang::LoadPreset(path, preset, error))
            return false;
        // An explicitly scaled last pass renders offscreen; RetroArch then
        // adds a stock pass so the image still reaches viewport size.
        const TicoSlang::Pass &last = preset.passes.back();
        const bool viewportLast =
            (last.scaleTypeX == TicoSlang::ScaleType::Unset && last.scaleTypeY == TicoSlang::ScaleType::Unset) ||
            (last.scaleTypeX == TicoSlang::ScaleType::Viewport && last.scaleTypeY == TicoSlang::ScaleType::Viewport &&
             last.scaleX == 1.0f && last.scaleY == 1.0f);
        if (!viewportLast)
        {
            TicoSlang::Preset stock;
            if (!TicoSlang::PresetFromSource("stock", kStockShader, stock, error))
                return false;
            preset.passes.push_back(stock.passes[0]);
        }
    }

    auto rt = std::make_unique<Runtime>();
    rt->parameters = preset.parameters;

    uint32_t uboCount = 0, samplerCount = 0;
    for (size_t i = 0; i < preset.passes.size(); i++)
    {
        PassRuntime p;
        p.cfg = preset.passes[i];
        TicoSlang::CompiledPass compiled;
        if (!TicoSlang::Compile(p.cfg.source, compiled, error))
        {
            error = p.cfg.path + ": " + error;
            return false;
        }
        p.refl = compiled.reflection;
        if (p.refl.pushSize > TicoVulkan::Ctx().props.limits.maxPushConstantsSize)
        {
            error = p.cfg.path + ": push constants too large";
            return false;
        }

        const bool last = i + 1 == preset.passes.size();
        VkFormat fmt = ParseFormat(p.cfg.source.format);
        if (fmt == VK_FORMAT_UNDEFINED)
            fmt = p.cfg.floatFramebuffer ? VK_FORMAT_R16G16B16A16_SFLOAT
                : p.cfg.srgbFramebuffer  ? VK_FORMAT_R8G8B8A8_SRGB
                                         : VK_FORMAT_R8G8B8A8_UNORM;
        // ImGui reads the last target and writes a UNORM swapchain.
        p.format = last ? VK_FORMAT_R8G8B8A8_UNORM : fmt;

        if (!CreatePassPipeline(p, compiled, error))
        {
            error = p.cfg.path + ": " + error;
            return false;
        }
        uboCount += p.refl.hasUbo ? 1 : 0;
        samplerCount += (uint32_t)p.refl.samplers.size();
        rt->passes.push_back(std::move(p));
    }

    // Work out which outputs need history, feedback copies or mipmaps.
    for (size_t i = 0; i < rt->passes.size(); i++)
    {
        PassRuntime &p = rt->passes[i];
        if (i + 1 < rt->passes.size() && rt->passes[i + 1].cfg.mipmapInput)
            p.genMips = true;
        for (const TicoSlang::SamplerBinding &s : p.refl.samplers)
        {
            std::string rest;
            int n = 0;
            if (StartsWith(s.name, "OriginalHistory", rest) && ParseIndex(rest, n))
                rt->historyDepth = std::max<uint32_t>(rt->historyDepth, n);
            else if (StartsWith(s.name, "PassFeedback", rest) && ParseIndex(rest, n) &&
                     n < (int)rt->passes.size())
                rt->passes[n].hasFeedback = true;
            else
            {
                for (PassRuntime &q : rt->passes)
                    if (!q.cfg.alias.empty() && s.name == q.cfg.alias + "Feedback")
                        q.hasFeedback = true;
            }
        }
    }

    VkCommandBuffer cmd = TicoVulkan::BeginOneShot();
    for (const TicoSlang::Texture &t : preset.textures)
    {
        Lut lut;
        lut.name = t.name;
        lut.linear = t.linear;
        lut.mipmap = t.mipmap;
        lut.wrap = t.wrap;
        if (!LoadLut(cmd, lut, t.path, error))
        {
            TicoVulkan::EndOneShot(cmd);
            return false;
        }
        rt->luts.push_back(lut);
    }
    TicoVulkan::EndOneShot(cmd);

    VkDescriptorPoolSize sizes[2] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, std::max(1u, uboCount * kFrames)},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, std::max(1u, samplerCount * kFrames)},
    };
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = (uint32_t)rt->passes.size() * kFrames;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    VkDevice dev = TicoVulkan::Ctx().device;
    if (vkCreateDescriptorPool(dev, &dp, nullptr, &rt->pool) != VK_SUCCESS)
    {
        error = "vkCreateDescriptorPool failed";
        return false;
    }
    for (PassRuntime &p : rt->passes)
    {
        VkDescriptorSetLayout layouts[kFrames];
        std::fill(layouts, layouts + kFrames, p.setLayout);
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = rt->pool;
        ai.descriptorSetCount = kFrames;
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(dev, &ai, p.sets) != VK_SUCCESS)
        {
            error = "vkAllocateDescriptorSets failed";
            return false;
        }
    }

    m_runtime = std::move(rt);
    m_presetPath = path;
    m_outputId = ImTextureID_Invalid;
    // History depth may have changed; rebuild on the next frame.
    for (Image &h : m_history)
        TicoVulkan::DeferDestroyImage(h);
    m_history.clear();

    LOG_INFO(CHAIN_TAG, "Loaded %s: %zu pass(es), %zu texture(s), %zu parameter(s)",
             path.empty() ? "built-in pass" : path.c_str(), m_runtime->passes.size(),
             m_runtime->luts.size(), m_runtime->parameters.size());
    return true;
}

const std::vector<TicoSlang::Parameter> &TicoShaderChain::Parameters() const
{
    static const std::vector<TicoSlang::Parameter> kNone;
    return m_runtime ? m_runtime->parameters : kNone;
}

void TicoShaderChain::SetParameter(const std::string &id, float value)
{
    if (!m_runtime)
        return;
    for (TicoSlang::Parameter &p : m_runtime->parameters)
        if (p.id == id)
            p.value = std::clamp(value, p.minimum, p.maximum);
}

void TicoShaderChain::ResetParameters()
{
    if (!m_runtime)
        return;
    for (TicoSlang::Parameter &p : m_runtime->parameters)
        p.value = p.initial;
}

void TicoShaderChain::SetSourceFrame(const void *data, unsigned width, unsigned height, size_t pitch,
                                     TicoPixelFormat format)
{
    if (!data || width == 0 || height == 0)
        return; // frame dupe: keep the previous one
    const unsigned bpp = format == TicoPixelFormat::XRGB8888 ? 4 : 2;
    const size_t row = (size_t)width * bpp;
    m_frame.resize(row * height);
    for (unsigned y = 0; y < height; y++)
        memcpy(m_frame.data() + y * row, (const uint8_t *)data + y * pitch, row);
    if (width != m_frameWidth || height != m_frameHeight)
        LOG_INFO(CHAIN_TAG, "Core frame %ux%u (pitch %zu, format %d)", width, height, pitch, (int)format);
    m_frameWidth = width;
    m_frameHeight = height;
    m_frameFormat = format;
    m_frameDirty = true;
    m_frameCount++;
}

void TicoShaderChain::SetSourceImage(VkImage image, VkImageLayout layout, unsigned width, unsigned height)
{
    if (!image || !width || !height)
        return;
    m_extImage = image;
    m_extLayout = layout;
    m_frameWidth = width;
    m_frameHeight = height;
    m_frameFormat = TicoPixelFormat::XRGB8888; // the blit converts to it
    m_frameDirty = true;
    m_frameCount++;
}

const TicoVulkan::Image *TicoShaderChain::OutputImage() const
{
    if (!m_runtime || m_runtime->passes.empty() || !m_runtime->passes.back().output.img.image)
        return nullptr;
    return &m_runtime->passes.back().output.img;
}

bool TicoShaderChain::UploadSource(VkCommandBuffer cmd)
{
    if (!m_frameDirty)
        return m_source.image != VK_NULL_HANDLE;
    m_frameDirty = false;

    VkFormat fmt;
    bool alphaOne = true;
    switch (m_frameFormat)
    {
    case TicoPixelFormat::XRGB8888: fmt = VK_FORMAT_B8G8R8A8_UNORM; break;
    case TicoPixelFormat::XRGB1555: fmt = VK_FORMAT_A1R5G5B5_UNORM_PACK16; break;
    default: fmt = VK_FORMAT_R5G6B5_UNORM_PACK16; break;
    }

    if (m_source.width != m_frameWidth || m_source.height != m_frameHeight || m_sourceFormat != m_frameFormat)
    {
        TicoVulkan::DeferDestroyImage(m_source);
        for (Image &h : m_history)
            TicoVulkan::DeferDestroyImage(h);
        m_history.clear();
        if (!TicoVulkan::CreateImage(m_source, m_frameWidth, m_frameHeight, fmt,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                         VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                     1, alphaOne))
            return false;
        m_sourceFormat = m_frameFormat;
    }

    // History: the ring holds the frames before the current one.
    const uint32_t depth = m_runtime ? m_runtime->historyDepth : 0;
    while (m_history.size() < depth)
    {
        Image h;
        if (!TicoVulkan::CreateImage(h, m_frameWidth, m_frameHeight, fmt,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 1, alphaOne))
            return false;
        ClearToReadable(cmd, h);
        m_history.push_back(h);
    }
    if (depth > 0 && m_source.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
    {
        std::rotate(m_history.rbegin(), m_history.rbegin() + 1, m_history.rend());
        Image &dst = m_history[0];
        TicoVulkan::TransitionImage(cmd, m_source.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        TicoVulkan::TransitionImage(cmd, dst.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageCopy copy = {};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = copy.srcSubresource;
        copy.extent = {m_frameWidth, m_frameHeight, 1};
        vkCmdCopyImage(cmd, m_source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        TicoVulkan::TransitionImage(cmd, dst.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        m_source.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    if (m_extImage)
    {
        // a GPU frame: blit it in, leaving the core's image as it was
        const VkImage ext = m_extImage;
        m_extImage = VK_NULL_HANDLE;
        TicoVulkan::TransitionImage(cmd, ext, 1, m_extLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        TicoVulkan::TransitionImage(cmd, m_source.image, 1, m_source.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit = {};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = blit.srcSubresource;
        blit.srcOffsets[1] = {(int32_t)m_frameWidth, (int32_t)m_frameHeight, 1};
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cmd, ext, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_source.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        TicoVulkan::TransitionImage(cmd, ext, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_extLayout,
                                    VK_ACCESS_TRANSFER_READ_BIT,
                                    VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        TicoVulkan::TransitionImage(cmd, m_source.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        m_source.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return true;
    }

    Buffer &staging = m_staging[TicoVulkan::FrameIndex()];
    if (staging.size < m_frame.size())
    {
        TicoVulkan::DeferDestroyBuffer(staging);
        if (!TicoVulkan::CreateBuffer(staging, m_frame.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
            return false;
    }
    memcpy(staging.mapped, m_frame.data(), m_frame.size());

    TicoVulkan::TransitionImage(cmd, m_source.image, 1, m_source.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                                VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_frameWidth, m_frameHeight, 1};
    vkCmdCopyBufferToImage(cmd, staging.buffer, m_source.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    TicoVulkan::TransitionImage(cmd, m_source.image, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    m_source.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

namespace
{

/// A texture a shader can read, with the sampler settings that apply to it.
struct TexRef
{
    const Image *img = nullptr;
    bool linear = false;
    bool mipmap = false;
    TicoSlang::WrapMode wrap = TicoSlang::WrapMode::ClampToBorder;
};

// `smooth` is the filter for a pass that does not choose one (RetroArch's
// video_smooth).
TexRef FromPassSettings(const Image *img, const TicoSlang::Pass &settings, bool smooth)
{
    TexRef r;
    r.img = img;
    r.linear = settings.filterSet ? settings.filterLinear : smooth;
    r.mipmap = settings.mipmapInput && img && img->mipLevels > 1;
    r.wrap = settings.wrap;
    return r;
}

} // namespace

ImTextureID TicoShaderChain::Process(VkCommandBuffer cmd, uint32_t viewportWidth, uint32_t viewportHeight,
                                     float coreAspect, double coreFps)
{
    if (!m_runtime || !cmd || viewportWidth == 0 || viewportHeight == 0)
        return ImTextureID_Invalid;
    if (!UploadSource(cmd))
        return ImTextureID_Invalid;

    Runtime &rt = *m_runtime;
    VkDevice dev = TicoVulkan::Ctx().device;
    const uint32_t slot = TicoVulkan::FrameIndex();

    // Last frame's outputs become this frame's feedback.
    for (PassRuntime &p : rt.passes)
        if (p.hasFeedback)
            std::swap(p.output, p.feedback);

    // Size and (re)create every target.
    uint32_t prevW = m_source.width, prevH = m_source.height;
    for (size_t i = 0; i < rt.passes.size(); i++)
    {
        PassRuntime &p = rt.passes[i];
        const bool last = i + 1 == rt.passes.size();
        TicoSlang::ScaleType sx = p.cfg.scaleTypeX, sy = p.cfg.scaleTypeY;
        if (sx == TicoSlang::ScaleType::Unset && sy == TicoSlang::ScaleType::Unset)
            sx = sy = last ? TicoSlang::ScaleType::Viewport : TicoSlang::ScaleType::Source;
        auto axis = [](TicoSlang::ScaleType t, float scale, uint32_t prev, uint32_t vp) -> uint32_t {
            switch (t)
            {
            case TicoSlang::ScaleType::Viewport: return (uint32_t)std::lround(vp * scale);
            case TicoSlang::ScaleType::Absolute: return (uint32_t)std::lround(scale);
            default: return (uint32_t)std::lround(prev * scale);
            }
        };
        uint32_t w = std::clamp<uint32_t>(axis(sx, p.cfg.scaleX, prevW, viewportWidth), 1, 8192);
        uint32_t h = std::clamp<uint32_t>(axis(sy, p.cfg.scaleY, prevH, viewportHeight), 1, 8192);
        if (last)
        {
            w = viewportWidth;
            h = viewportHeight;
        }
        p.outW = w;
        p.outH = h;
        prevW = w;
        prevH = h;

        Target *targets[2] = {&p.output, p.hasFeedback ? &p.feedback : nullptr};
        for (Target *t : targets)
        {
            if (!t || (t->img.width == w && t->img.height == h))
                continue;
            DestroyTarget(*t);
            const uint32_t levels = p.genMips ? MipCount(w, h) : 1;
            if (!TicoVulkan::CreateImage(t->img, w, h, p.format,
                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                         levels))
                return ImTextureID_Invalid;
            VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = t->img.image;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = p.format;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCreateImageView(dev, &vci, nullptr, &t->rtView);
            VkFramebufferCreateInfo fci = {VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fci.renderPass = p.renderPass;
            fci.attachmentCount = 1;
            fci.pAttachments = &t->rtView;
            fci.width = w;
            fci.height = h;
            fci.layers = 1;
            vkCreateFramebuffer(dev, &fci, nullptr, &t->fb);
            ClearToReadable(cmd, t->img);
        }
    }

    // Resolve a texture name for pass `index` (RetroArch semantics).
    auto settingsAfter = [&](size_t k) -> const TicoSlang::Pass & {
        return k + 1 < rt.passes.size() ? rt.passes[k + 1].cfg : rt.passes[k].cfg;
    };
    auto resolve = [&](const std::string &name, size_t index) -> TexRef {
        std::string rest;
        int n = 0;
        if (name == "Original")
            return FromPassSettings(&m_source, rt.passes[0].cfg, m_smooth);
        if (name == "Source")
            return FromPassSettings(index == 0 ? &m_source : &rt.passes[index - 1].output.img,
                                    rt.passes[index].cfg, m_smooth);
        if (StartsWith(name, "OriginalHistory", rest) && ParseIndex(rest, n))
        {
            const Image *img = n == 0 ? &m_source
                             : (size_t)n <= m_history.size() ? &m_history[n - 1] : nullptr;
            return FromPassSettings(img, rt.passes[0].cfg, m_smooth);
        }
        if (StartsWith(name, "PassOutput", rest) && ParseIndex(rest, n) && (size_t)n < index)
            return FromPassSettings(&rt.passes[n].output.img, settingsAfter(n), m_smooth);
        if (StartsWith(name, "PassFeedback", rest) && ParseIndex(rest, n) && (size_t)n < rt.passes.size() &&
            rt.passes[n].hasFeedback)
            return FromPassSettings(&rt.passes[n].feedback.img, settingsAfter(n), m_smooth);
        if (StartsWith(name, "User", rest) && ParseIndex(rest, n) && (size_t)n < rt.luts.size())
        {
            const Lut &l = rt.luts[n];
            return {&l.img, l.linear, l.mipmap && l.img.mipLevels > 1, l.wrap};
        }
        for (size_t k = 0; k < rt.passes.size(); k++)
        {
            const std::string &alias = rt.passes[k].cfg.alias;
            if (alias.empty())
                continue;
            if (name == alias && k < index)
                return FromPassSettings(&rt.passes[k].output.img, settingsAfter(k), m_smooth);
            if (name == alias + "Feedback" && rt.passes[k].hasFeedback)
                return FromPassSettings(&rt.passes[k].feedback.img, settingsAfter(k), m_smooth);
        }
        for (const Lut &l : rt.luts)
            if (name == l.name)
                return {&l.img, l.linear, l.mipmap && l.img.mipLevels > 1, l.wrap};
        return {};
    };
    auto sizeOf = [&](const std::string &name, size_t index, float out[4]) -> bool {
        std::string rest, tex;
        if (name == "OutputSize")
        {
            const PassRuntime &p = rt.passes[index];
            out[0] = (float)p.outW, out[1] = (float)p.outH;
        }
        else if (name == "FinalViewportSize")
            out[0] = (float)viewportWidth, out[1] = (float)viewportHeight;
        else
        {
            if (StartsWith(name, "OriginalHistorySize", rest))
                tex = "OriginalHistory" + rest;
            else if (StartsWith(name, "PassOutputSize", rest))
                tex = "PassOutput" + rest;
            else if (StartsWith(name, "PassFeedbackSize", rest))
                tex = "PassFeedback" + rest;
            else if (StartsWith(name, "UserSize", rest))
                tex = "User" + rest;
            else if (name.size() > 4 && name.compare(name.size() - 4, 4, "Size") == 0)
                tex = name.substr(0, name.size() - 4);
            else
                return false;
            TexRef r = resolve(tex, index);
            if (!r.img || !r.img->width)
                return false;
            out[0] = (float)r.img->width, out[1] = (float)r.img->height;
        }
        out[2] = 1.0f / out[0];
        out[3] = 1.0f / out[1];
        return true;
    };

    static const float kMvp[16] = {2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1};

    for (size_t i = 0; i < rt.passes.size(); i++)
    {
        PassRuntime &p = rt.passes[i];

        // Uniforms.
        std::fill(p.uboData.begin(), p.uboData.end(), 0);
        std::fill(p.pushData.begin(), p.pushData.end(), 0);
        for (const TicoSlang::UniformMember &m : p.refl.members)
        {
            std::vector<uint8_t> &data = m.inPushConstant ? p.pushData : p.uboData;
            if (m.offset + m.size > data.size())
                continue;
            uint8_t *dst = data.data() + m.offset;
            float vec[4];
            if (m.name == "MVP" && m.size >= 64)
                memcpy(dst, kMvp, 64);
            else if (m.name == "FrameCount" && m.size >= 4)
            {
                uint32_t v = p.cfg.frameCountMod ? m_frameCount % p.cfg.frameCountMod : m_frameCount;
                memcpy(dst, &v, 4);
            }
            else if (m.name == "FrameDirection" && m.size >= 4)
            {
                int32_t v = 1;
                memcpy(dst, &v, 4);
            }
            else if ((m.name == "TotalSubFrames" || m.name == "CurrentSubFrame") && m.size >= 4)
            {
                uint32_t v = 1;
                memcpy(dst, &v, 4);
            }
            else if (m.name == "FrameTimeDelta" && m.size >= 4)
            {
                uint32_t v = coreFps > 0 ? (uint32_t)(1000000.0 / coreFps) : 16667;
                memcpy(dst, &v, 4);
            }
            else if ((m.name == "OriginalFPS" || m.name == "CoreFPS") && m.size >= 4)
            {
                float v = (float)coreFps;
                memcpy(dst, &v, 4);
            }
            else if ((m.name == "OriginalAspect" || m.name == "OriginalAspectRotated") && m.size >= 4)
                memcpy(dst, &coreAspect, 4);
            else if (m.size >= 16 && sizeOf(m.name, i, vec))
                memcpy(dst, vec, 16);
            else if (m.size == 4)
            {
                for (const TicoSlang::Parameter &param : rt.parameters)
                    if (param.id == m.name)
                        memcpy(dst, &param.value, 4);
            }
        }

        // Descriptors.
        std::vector<VkWriteDescriptorSet> writes;
        std::vector<VkDescriptorImageInfo> images(p.refl.samplers.size());
        VkDescriptorBufferInfo uboInfo = {};
        if (p.refl.hasUbo)
        {
            memcpy(p.ubo[slot].mapped, p.uboData.data(), p.uboData.size());
            uboInfo = {p.ubo[slot].buffer, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = p.sets[slot];
            w.dstBinding = p.refl.uboBinding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &uboInfo;
            writes.push_back(w);
        }
        for (size_t s = 0; s < p.refl.samplers.size(); s++)
        {
            TexRef r = resolve(p.refl.samplers[s].name, i);
            if (!r.img || !r.img->view)
                r = {&m_dummy, false, false, TicoSlang::WrapMode::ClampToEdge};
            images[s].sampler = m_samplers[r.linear][r.mipmap][(int)r.wrap];
            images[s].imageView = r.img->view;
            images[s].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = p.sets[slot];
            w.dstBinding = p.refl.samplers[s].binding;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[s];
            writes.push_back(w);
        }
        vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);

        // Draw.
        VkClearValue clear = {};
        VkRenderPassBeginInfo rp = {VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = p.renderPass;
        rp.framebuffer = p.output.fb;
        rp.renderArea.extent = {p.outW, p.outH};
        rp.clearValueCount = 1;
        rp.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport vp = {0, 0, (float)p.outW, (float)p.outH, 0, 1};
        VkRect2D scissor = {{0, 0}, {p.outW, p.outH}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.layout, 0, 1, &p.sets[slot], 0, nullptr);
        if (!p.pushData.empty())
            vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               (uint32_t)p.pushData.size(), p.pushData.data());
        const bool lastPass = &p == &rt.passes.back();
        VkDeviceSize offset = lastPass ? (VkDeviceSize)m_rotation * 4 * 6 * sizeof(float) : 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_quadBuffer.buffer, &offset);
        vkCmdDraw(cmd, 4, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        p.output.img.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        if (p.genMips)
            GenerateMips(cmd, p.output.img);
    }

    Target &final = rt.passes.back().output;
    if (final.imguiId == ImTextureID_Invalid)
        final.imguiId = TicoVulkan::RegisterImage(final.img.view);
    m_outputId = final.imguiId;
    return m_outputId;
}

bool TicoShaderChain::ReadOutputRGBA(std::vector<uint8_t> &out, uint32_t &width, uint32_t &height)
{
    if (!m_runtime || m_runtime->passes.empty())
        return false;
    Image &img = m_runtime->passes.back().output.img;
    if (!img.image)
        return false;
    TicoVulkan::WaitIdle();

    Buffer readback;
    const VkDeviceSize size = (VkDeviceSize)img.width * img.height * 4;
    if (!TicoVulkan::CreateBuffer(readback, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT))
        return false;
    VkCommandBuffer cmd = TicoVulkan::BeginOneShot();
    TicoVulkan::TransitionImage(cmd, img.image, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                                VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {img.width, img.height, 1};
    vkCmdCopyImageToBuffer(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &region);
    TicoVulkan::TransitionImage(cmd, img.image, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    TicoVulkan::EndOneShot(cmd);
    const uint8_t *pixels = (const uint8_t *)readback.mapped;
    out.assign(pixels, pixels + size);
    // BGRA outputs come back with red and blue swapped
    if (img.format == VK_FORMAT_B8G8R8A8_UNORM || img.format == VK_FORMAT_B8G8R8A8_SRGB)
        for (size_t i = 0; i + 3 < out.size(); i += 4)
            std::swap(out[i], out[i + 2]);
    for (size_t i = 3; i < out.size(); i += 4)
        out[i] = 255;
    width = img.width;
    height = img.height;
    TicoVulkan::DestroyBuffer(readback);
    return true;
}

bool TicoShaderChain::SaveOutputPNG(const std::string &path)
{
    std::vector<uint8_t> rgba;
    uint32_t width = 0, height = 0;
    if (!ReadOutputRGBA(rgba, width, height))
        return false;
    return stbi_write_png(path.c_str(), (int)width, (int)height, 4, rgba.data(), (int)width * 4) != 0;
}
