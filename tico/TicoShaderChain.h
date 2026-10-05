/// @file TicoShaderChain.h
/// @brief Runs a RetroArch slang preset on the core's software frame.
///
/// The core's frame is uploaded into a source image, every pass of the preset
/// renders into its own target, and the last pass renders at the size the
/// game occupies on screen. That image is handed to ImGui, so the overlay
/// draws the processed game exactly like it drew the raw frame before.
/// Without a preset a built-in nearest-neighbour pass does the scaling.
#pragma once

#include "TicoSlang.h"
#include "TicoVulkan.h"

#include <memory>
#include <string>
#include <vector>

/// Pixel layouts of a software-rendered frame.
enum class TicoPixelFormat
{
    Unknown,
    XRGB8888,
    RGB565,
    XRGB1555,
};

class TicoShaderChain
{
public:
    TicoShaderChain();
    ~TicoShaderChain();

    bool Init();
    void Shutdown();

    /// Load a .slangp. An empty path selects the built-in pass. On failure the
    /// previous preset stays active and `error` says why.
    bool LoadPreset(const std::string &path, std::string &error);
    const std::string &PresetPath() const { return m_presetPath; }

    /// Parameters of the active preset, with their current values.
    const std::vector<TicoSlang::Parameter> &Parameters() const;
    void SetParameter(const std::string &id, float value);
    void ResetParameters();

    /// Copy a new software-rendered frame.
    void SetSourceFrame(const void *data, unsigned width, unsigned height, size_t pitch,
                        TicoPixelFormat format);

    /// A new frame from a hardware-rendered core: the next Process copies
    /// `width` x `height` of `image` (in `layout`, left in it afterwards) into
    /// the source on the GPU.
    void SetSourceImage(VkImage image, VkImageLayout layout, unsigned width, unsigned height);

    /// Record the passes into `cmd` and return the final image for ImGui, or
    /// ImTextureID_Invalid if there is nothing to show yet.
    ImTextureID Process(VkCommandBuffer cmd, uint32_t viewportWidth, uint32_t viewportHeight,
                        float coreAspect, double coreFps);

    /// The last pass's image from the latest Process, in
    /// SHADER_READ_ONLY_OPTIMAL, sized to the viewport; null before the first.
    const TicoVulkan::Image *OutputImage() const;

    /// Turns the picture by `quarterTurns` x 90 degrees counter-clockwise in
    /// the last pass, as a core rotating its output asks (vertical arcade games);
    /// the earlier passes still see the game upright.
    void SetRotation(int quarterTurns) { m_rotation = ((quarterTurns % 4) + 4) % 4; }

    /// Bilinear filtering for passes that do not set filter_linear (the
    /// built-in one included); nearest otherwise, the default.
    void SetSmooth(bool smooth) { m_smooth = smooth; }

    /// The last final image as RGBA8 (the save state pictures). Waits for the GPU.
    bool ReadOutputRGBA(std::vector<uint8_t> &out, uint32_t &width, uint32_t &height);

    /// Debug: write the last final image to a PNG. Waits for the GPU.
    bool SaveOutputPNG(const std::string &path);

    struct Runtime; // defined in the .cpp

private:
    bool UploadSource(VkCommandBuffer cmd);

    std::unique_ptr<Runtime> m_runtime;
    std::string m_presetPath;

    // Latest core frame, tightly packed.
    std::vector<uint8_t> m_frame;
    unsigned m_frameWidth = 0;
    unsigned m_frameHeight = 0;
    TicoPixelFormat m_frameFormat = TicoPixelFormat::RGB565;
    bool m_frameDirty = false;
    int m_rotation = 0;
    bool m_smooth = false;
    VkImage m_extImage = VK_NULL_HANDLE; // SetSourceImage's frame, until copied
    VkImageLayout m_extLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t m_frameCount = 0;

    TicoVulkan::Buffer m_staging[TicoVulkan::kFramesInFlight];
    TicoVulkan::Image m_source;
    TicoPixelFormat m_sourceFormat = TicoPixelFormat::Unknown;
    std::vector<TicoVulkan::Image> m_history; // [0] = previous frame

    TicoVulkan::Image m_dummy; // bound for textures a preset names but lacks
    TicoVulkan::Buffer m_quadBuffer;
    VkSampler m_samplers[2][2][4] = {}; // [linear][mipmap][wrap]

    ImTextureID m_outputId = ImTextureID_Invalid;

    friend struct Runtime;
};
