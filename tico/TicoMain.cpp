/// @file TicoMain.cpp
/// @brief Entry point for tico-integrated mupen64plus NRO
///
/// The emulator runs on its own thread (core 1). Each frame it scans out is
/// handed to the main thread (core 2) through a one-slot handoff; the main
/// thread draws it with the overlay and presents, and only then lets the
/// emulator post the next one, so FIFO vsync paces emulation. Holding a frame
/// (the quick menu is open) pauses the game. The renderer is fixed at launch:
/// paraLLEl-RDP on Vulkan, or GLideN64 on OpenGL (NVC0 or Zink).

#include "TicoAudio.h"
#include "TicoSession.h"
#include "TicoConfig.h"
#include "TicoCore.h"
#include "TicoGL.h"
#include "TicoLogger.h"
#include "TicoRenderer.h"
#include "TicoUtils.h"
#ifdef TICO_HAVE_VULKAN
#include "TicoShaderChain.h"
#include "TicoVulkan.h"
#endif
#include "UsbStorage.h"
#include "m64p/tico_m64p.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"

#include <SDL.h>
#include <curl/curl.h>
#include <dirent.h>
#include <json.hpp>
#include <strings.h>
#include <switch.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "deps/stb/stb_image.h"
// stb_image_write is built here, for the state pictures; TicoShaderChain uses
// it too (TICO_STB_IMAGE_WRITE_EXTERNAL), but only in Vulkan builds.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "deps/stb/stb_image_write.h"
#include "imgui.h"

//==============================================================================
// NX System Configuration (extern "C")
//==============================================================================

extern "C" {
u32 __NvOptimusEnablement = 1;
u32 __NvDeveloperOption = 1;
u32 __nx_applet_type = AppletType_Application;
u32 __nx_applet_exit_mode = 0; // 0 = standard exit (return to Homebrew ABI loader if NRO). 1 = forceful applet exit
size_t __nx_heap_size = 0;
}

//==============================================================================
// Globals
//==============================================================================

namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayConfig = SwitchFrontend::TicoConfig;
using SwitchFrontend::OverlayTranslation::tr;

static std::unique_ptr<TicoCore> g_core;
#ifdef TICO_HAVE_VULKAN
static std::unique_ptr<TicoShaderChain> g_chain; // Vulkan only
using GameCommandBuffer = VkCommandBuffer;
#else
// Built for a Mesa without Vulkan: OpenGL is the only renderer.
using GameCommandBuffer = void *;
#endif
static std::string g_activePreset = "\x01";     // forces the first load

// This NRO's path and the launch it was started with, for Restart and the
// library, which start the emulator again in a fresh process.
static std::string g_selfPath;
static std::string g_slugArg;
static std::string g_titleArg;
// Started without a game: the library lists the ROM folders, and leaving a
// game returns to it instead of chainloading tico.
static bool g_standalone = false;
static bool g_fromLibrary = false;
static void RelaunchSelf(const std::vector<std::string> &args);

// Quick menu
static bool g_menuOpen = false;
static bool g_overlayReady = false;
static bool g_toggleHeld = false;
static uint32_t g_navHeldPrev = 0;
static int g_navRepeatFrames = 0;
static constexpr int kNavInitialDelayFrames = 14;
static constexpr int kNavRepeatFrames = 6;

// Fast forward (Display > Fast Forward): the hotkey, held or toggled, runs the
// game at fast_forward_speed by letting extra frames through per presented
// one; "unlimited" drops vsync instead.
static bool g_ffHotkeyHeld = false;
static bool g_ffLatched = false;
static float g_ffFrameBudget = 0.0f;
static void StopFastForward();

// HUD frame counter
static int g_hudFrames = 0;
static float g_hudSeconds = 0.0f;
static float g_hudFps = 0.0f;

static bool g_running = true;
static bool g_exitToTico = false;
static TicoAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;
static SDL_GameController *g_controllers[4] = {nullptr, nullptr, nullptr, nullptr};
static bool g_controllersDirty = true;
static u8 g_lastOperationMode = 255;

//==============================================================================
// Frame handoff
//==============================================================================

// One frame at a time from the emulation thread to the main thread. The
// emulator posts a frame only once the previous one was released; the main
// thread releases a frame after it submitted its present, so the emulator
// never renders into an image still being presented. Not releasing pauses
// the game. While draining (a save state is being taken) posts never wait:
// the newest frame replaces the last and the main thread presents none.
namespace {
struct PostedFrame
{
    uint32_t slot = 0;
    unsigned width = 0;
    unsigned height = 0;
    uint64_t serial = 0;
};

struct FrameHandoff
{
    std::mutex mutex;
    std::condition_variable cond;
    bool full = false;
    bool shutdown = false;
    bool drain = false;
    PostedFrame frame;
    uint64_t serial = 0;
};
FrameHandoff g_handoff;

std::atomic<uint64_t> g_presented{0};
} // namespace

// Emulation thread.
static void PostFrame(uint32_t slot, unsigned width, unsigned height)
{
    {
        std::unique_lock<std::mutex> lock(g_handoff.mutex);
        g_handoff.cond.wait(lock, [] { return !g_handoff.full || g_handoff.shutdown || g_handoff.drain; });
        if (g_handoff.shutdown)
            return;
        g_handoff.full = true;
        g_handoff.frame = {slot, width, height, ++g_handoff.serial};
    }
    g_handoff.cond.notify_all();
}

// Main thread: the posted frame, waiting up to `timeout` for one.
static bool PeekFrame(PostedFrame &frame, std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(g_handoff.mutex);
    if (!g_handoff.cond.wait_for(lock, timeout, [] { return g_handoff.full; }))
        return false;
    frame = g_handoff.frame;
    return true;
}

static void ReleaseFrame()
{
    {
        std::lock_guard<std::mutex> lock(g_handoff.mutex);
        g_handoff.full = false;
    }
    g_handoff.cond.notify_all();
}

static void SetDrain(bool drain)
{
    {
        std::lock_guard<std::mutex> lock(g_handoff.mutex);
        g_handoff.drain = drain;
    }
    g_handoff.cond.notify_all();
}

static void ShutdownHandoff()
{
    {
        std::lock_guard<std::mutex> lock(g_handoff.mutex);
        g_handoff.shutdown = true;
    }
    g_handoff.cond.notify_all();
}

// Set when the emulator ran on while the menu held its frame: the screen
// should show the newest frame it posted.
static bool g_refreshShownFrame = false;

// Runs `fn` (a save state) while the emulator runs freely to its next safe
// point, presenting nothing meanwhile.
template <typename Fn>
static auto WithEmulatorRunning(Fn fn) -> decltype(fn())
{
    // the emulator may need GL to get there, and the menu holds it
    const int glDepth = TicoGL::Suspend();
    SetDrain(true);
    auto result = fn();
    SetDrain(false);
    TicoGL::Resume(glDepth);
    g_refreshShownFrame = true;
    return result;
}

#ifdef TICO_HAVE_VULKAN
// Emulation thread: paraLLEl-RDP scanned out a frame into its current slot.
static void PresentVulkanFromCore(unsigned width, unsigned height)
{
    const uint32_t slot = TicoVulkan::AdvanceCoreFrame();
    if (g_core && width && height)
        g_core->SetFrameSize((int)width, (int)height);
    PostFrame(slot, width, height);
}
#endif

// Emulation thread: GLideN64 swapped; copy its frame out for the main thread.
static void PresentGLFromCore(unsigned width, unsigned height)
{
    const uint32_t slot = TicoGL::PostCoreFrame(width, height);
    // the main thread draws it, and may wait here for its turn at GL
    TicoGL::Unlock();
    PostFrame(slot, width, height);
    TicoGL::Lock();
}

//==============================================================================
// Switch
//==============================================================================

static void ApplySwitchPerformanceProfile()
{
    Result rcNormal = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220007);
    Result rcBoost = apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, 0x92220008);
    if (R_FAILED(rcNormal) || R_FAILED(rcBoost))
        LOG_WARN("HOME", "Switch performance profile failed (normal=0x%x boost=0x%x)", rcNormal, rcBoost);
    else
        LOG_INFO("HOME", "Applied Switch performance profile");
}

static void PinCurrentThreadToCore(int core, const char *label)
{
    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc))
        LOG_WARN("HOME", "Failed to pin %s thread to core %d (rc=0x%x)", label, core, rc);
    else
        LOG_INFO("HOME", "Pinned %s thread to core %d", label, core);
}

static void GetDisplayResolution(int &w, int &h)
{
    const bool handheld = appletGetOperationMode() == AppletOperationMode_Handheld;
    w = handheld ? 1280 : 1920;
    h = handheld ? 720 : 1080;
}

static bool UpdateScreenMode()
{
    u8 operationMode = appletGetOperationMode();
    if (operationMode == g_lastOperationMode)
        return false;

    // Size the window to the mode and crop from the top-left, so the
    // swapchain always matches what is on screen.
    const bool handheld = operationMode == AppletOperationMode_Handheld;
    const u32 w = handheld ? 1280 : 1920, h = handheld ? 720 : 1080;
    nwindowSetDimensions(nwindowGetDefault(), w, h);
    nwindowSetCrop(nwindowGetDefault(), 0, 0, w, h);
#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
        TicoVulkan::Resize(w, h);
    else
#endif
        TicoGL::Resize(w, h);
    LOG_INFO("DISPLAY", "Mode -> %s (%ux%u)", handheld ? "Handheld" : "Docked", w, h);
    if (ImGui::GetCurrentContext())
        ImGui::GetIO().FontGlobalScale = handheld ? 1.0f : 1.5f;
    g_lastOperationMode = operationMode;
    return true;
}

//==============================================================================
// Controllers
//==============================================================================

static void CloseControllers()
{
    for (SDL_GameController *&controller : g_controllers)
    {
        if (controller)
        {
            SDL_GameControllerClose(controller);
            controller = nullptr;
        }
    }
}

static void RefreshControllers()
{
    CloseControllers();
    int controllerIndex = 0;
    const int joystickCount = SDL_NumJoysticks();
    for (int i = 0; i < joystickCount && controllerIndex < 4; ++i)
    {
        if (!SDL_IsGameController(i))
            continue;
        SDL_GameController *controller = SDL_GameControllerOpen(i);
        if (!controller)
        {
            LOG_WARN("INPUT", "Failed to open controller %d: %s", i, SDL_GetError());
            continue;
        }
        g_controllers[controllerIndex++] = controller;
    }
    g_controllersDirty = false;
}

// Settings > Players: what each player has. SDL lists the connected pads in
// slot order (the handheld Joy-Con with the first), and that is the order the
// players take, so the names follow it.
static std::vector<std::string> ControllerNames()
{
    std::vector<std::string> names;
    auto name = [](u32 style) -> std::string {
        const char *key = "emulator_pad_other";
        if (style & HidNpadStyleTag_NpadFullKey)
            key = "emulator_pad_pro";
        else if (style & HidNpadStyleTag_NpadHandheld)
            key = "emulator_pad_handheld";
        else if (style & HidNpadStyleTag_NpadJoyDual)
            key = "emulator_pad_joycon_pair";
        else if (style & HidNpadStyleTag_NpadJoyLeft)
            key = "emulator_pad_joycon_left";
        else if (style & HidNpadStyleTag_NpadJoyRight)
            key = "emulator_pad_joycon_right";
        else if (style & HidNpadStyleTag_NpadGc)
            key = "emulator_pad_gamecube";
        return tr(key);
    };
    const u32 handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld);
    for (int slot = 0; slot < 8 && names.size() < 4; ++slot)
    {
        u32 style = hidGetNpadStyleSet(static_cast<HidNpadIdType>(HidNpadIdType_No1 + slot));
        if (slot == 0 && !style)
            style = handheld;
        if (style)
            names.push_back(name(style));
    }
    if (names.empty() && handheld)
        names.push_back(name(handheld));
    names.resize(4);
    return names;
}

// The system's controller screen, where the players choose who is which.
static bool ShowControllerOrder()
{
    HidLaControllerSupportArg arg;
    hidLaCreateControllerSupportArg(&arg);
    arg.hdr.player_count_min = 0;
    arg.hdr.player_count_max = 4;
    HidLaControllerSupportResultInfo info{};
    return R_SUCCEEDED(hidLaShowControllerSupport(&info, &arg));
}

//==============================================================================
// SDL, audio, ImGui
//==============================================================================

static bool InitSDL()
{
    LOG_INFO("HOME", "Starting initialization...");
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
    else
        LOG_INFO("AUDIO", "SDL_mixer initialized");
    return true;
}

// The game's sample rate changes from the emulation thread.
static void AudioRateCallback(double rate)
{
    g_audio.SetCoreSampleRate(rate);
}

static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

static void AudioFlushCallback()
{
    g_audio.Flush();
}

static bool InitImGui()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImFontConfig fontCfg;
    fontCfg.SizePixels = TicoConfig::FONT_SIZE;
    if (io.Fonts->AddFontFromFileTTF(TicoConfig::FONT_PATH, TicoConfig::FONT_SIZE))
        LOG_INFO("HOME", "Loaded ImGui font from %s", TicoConfig::FONT_PATH);
    else if (!io.Fonts->AddFontDefault(&fontCfg))
    {
        LOG_ERROR("HOME", "Failed to load font from romfs and built-in ImGui fallback");
        return false;
    }
    // the RA alerts' descriptions
    io.Fonts->AddFontFromFileTTF("romfs:/fonts/description.ttf", TicoConfig::FONT_SIZE * 0.75f);
    return true;
}

// Brings up the renderer tico's settings named. With a game on Vulkan,
// paraLLEl-RDP creates the device, so that waits for the core (LoadGame).
static bool InitRenderer(bool forGame)
{
    int w, h;
    GetDisplayResolution(w, h);
    switch (TicoRenderer::Current())
    {
    case TicoRenderer::Backend::Vulkan:
#ifdef TICO_HAVE_VULKAN
        if (forGame)
            return true;
        return TicoVulkan::Init((uint32_t)w, (uint32_t)h, false);
#else
        return false;
#endif
    case TicoRenderer::Backend::OpenGL:
        return TicoGL::Init((uint32_t)w, (uint32_t)h, false);
    case TicoRenderer::Backend::Zink:
        return TicoGL::Init((uint32_t)w, (uint32_t)h, true);
    }
    return false;
}

#ifdef TICO_HAVE_VULKAN
static bool CreateVulkanDeviceForCore()
{
    int w, h;
    GetDisplayResolution(w, h);
    if (!TicoVulkan::Init((uint32_t)w, (uint32_t)h, true))
        return false;
    tico_m64p_vulkan_set_interface(TicoVulkan::CoreInterface());
    g_chain = std::make_unique<TicoShaderChain>();
    if (!g_chain->Init())
    {
        LOG_ERROR("HOME", "Shader chain initialization failed");
        g_chain.reset();
    }
    return true;
}

#endif

static void ShutdownRenderer()
{
#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
    {
        TicoVulkan::WaitIdle();
        g_chain.reset();
        TicoSlang::Shutdown();
        TicoVulkan::Shutdown();
    }
    else
#endif
    {
        TicoGL::Shutdown();
    }
}

//==============================================================================
// Main Loop
//==============================================================================

static void ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            g_running = false;
        if (event.type == SDL_CONTROLLERDEVICEADDED || event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_JOYDEVICEADDED || event.type == SDL_JOYDEVICEREMOVED)
            g_controllersDirty = true;
    }
}

//==============================================================================
// Leaving: tico, the library, or this game again
//==============================================================================

static std::string Quote(const std::string &arg)
{
    return "\"" + arg + "\"";
}

// Starts this NRO again when this process exits, with these arguments.
static void RelaunchSelf(const std::vector<std::string> &args)
{
    std::string line = Quote(g_selfPath);
    for (const std::string &arg : args)
        line += " " + Quote(arg);
    envSetNextLoad(g_selfPath.c_str(), line.c_str());
    LOG_INFO("HOME", "Relaunching: %s", line.c_str());
    g_running = false;
}

static void ChainloadTico()
{
    const char *primaryNro = "sdmc:/switch/tico.nro";
    const char *fallbackNro = "sdmc:/switch/tico/tico.nro";
    const char *targetNro = nullptr;

    struct stat buffer;
    if (stat(primaryNro, &buffer) == 0)
        targetNro = primaryNro;
    else if (stat(fallbackNro, &buffer) == 0)
        targetNro = fallbackNro;

    if (targetNro != nullptr)
    {
        char args[512];
        snprintf(args, sizeof(args), "%s --resume", targetNro);
        envSetNextLoad(targetNro, args);
        LOG_INFO("HOME", "Chainloading back to %s with args: %s", targetNro, args);
    }
    else
    {
        LOG_WARN("HOME", "Chainload target not found! Exiting normally.");
    }
}

//==============================================================================
// Save states
//==============================================================================

static std::string StatePath(int slot)
{
    const std::string dir = TicoConfig::StatesPath();
    TicoConfig::MakeDirs(dir);
    std::string romName = g_core ? g_core->GetGamePath() : std::string();
    const size_t lastSlash = romName.find_last_of("/\\");
    if (lastSlash != std::string::npos)
        romName = romName.substr(lastSlash + 1);
    const size_t lastDot = romName.find_last_of('.');
    if (lastDot != std::string::npos)
        romName = romName.substr(0, lastDot);
    return dir + romName + ".state" + std::to_string(slot);
}

// The frame on screen, as RGBA, for the picture saved with a save state.
static PostedFrame g_shownFrame;
static bool g_haveShownFrame = false;

static bool ReadShownFrame(std::vector<uint8_t> &rgba, uint32_t &width, uint32_t &height)
{
    if (!g_haveShownFrame)
        return false;
#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
        return g_chain && g_chain->ReadOutputRGBA(rgba, width, height);
#endif
    return TicoGL::ReadFrameRGBA(g_shownFrame.slot, rgba, width, height);
}

// Saves the frame on screen beside a save state, shrunk to fit 256x192 (box
// filtered), just big enough for the Save/Load State panel.
static void SaveStatePicture(const std::string &path)
{
    std::vector<uint8_t> rgba;
    uint32_t srcW = 0, srcH = 0;
    if (!ReadShownFrame(rgba, srcW, srcH) || !srcW || !srcH)
        return;
    const float fit = std::min({1.0f, 256.0f / srcW, 192.0f / srcH});
    const unsigned dstW = std::max(1u, (unsigned)(srcW * fit));
    const unsigned dstH = std::max(1u, (unsigned)(srcH * fit));
    std::vector<uint8_t> out(dstW * dstH * 4);
    for (unsigned y = 0; y < dstH; y++)
    {
        const unsigned y0 = y * srcH / dstH, y1 = std::max(y0 + 1, (y + 1) * srcH / dstH);
        for (unsigned x = 0; x < dstW; x++)
        {
            const unsigned x0 = x * srcW / dstW, x1 = std::max(x0 + 1, (x + 1) * srcW / dstW);
            unsigned sum[3] = {0, 0, 0}, n = 0;
            for (unsigned sy = y0; sy < y1; sy++)
                for (unsigned sx = x0; sx < x1; sx++)
                {
                    const uint8_t *px = &rgba[(sy * srcW + sx) * 4];
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    n++;
                }
            uint8_t *dst = &out[(y * dstW + x) * 4];
            dst[0] = sum[0] / n;
            dst[1] = sum[1] / n;
            dst[2] = sum[2] / n;
            dst[3] = 255;
        }
    }
    stbi_write_png(path.c_str(), (int)dstW, (int)dstH, 4, out.data(), (int)dstW * 4);
}

static bool SaveStateNow(int slot)
{
    if (!g_core)
        return false;
    const std::string path = StatePath(slot);
    LOG_INFO("HOME", "state: saving slot %d", slot);
    const bool saved = WithEmulatorRunning([&] { return g_core->SaveState(path); });
    LOG_INFO("HOME", "state: slot %d %s, taking its picture", slot, saved ? "saved" : "not saved");
    if (saved)
        SaveStatePicture(path + ".png");
    LOG_INFO("HOME", "state: slot %d done", slot);
    return saved;
}

static bool LoadStateNow(int slot)
{
    if (!g_core)
        return false;
    const std::string path = StatePath(slot);
    return WithEmulatorRunning([&] { return g_core->LoadState(path); });
}

// The state the game is left in, saved to the auto slot (listed first in Load
// State) whenever the game closes: Exit, Restart, the library, HOME. Once.
static bool g_autoSaved = false;
static void AutoSaveState()
{
    if (!g_core || !g_core->IsRunning() || g_autoSaved)
        return;
    g_autoSaved = true;
    SaveStateNow(OverlayUI::kAutoStateSlot - 1);
}

// Set when a game starts; once its first frame has run, the menu asks whether
// to continue from the auto save, if it has one.
static bool g_offerResume = false;
static void OpenMenu();

static void OfferResume()
{
    g_offerResume = false;
    struct stat st;
    if (!g_core || g_core->IsHardcoreActive() ||
        stat(StatePath(OverlayUI::kAutoStateSlot - 1).c_str(), &st) != 0)
        return;
    // tico's General > Continue Last Game
    const std::string mode = OverlayConfig::ResumeOnLaunch();
    if (mode == "never")
        return;
    if (mode == "always")
    {
        if (LoadStateNow(OverlayUI::kAutoStateSlot - 1))
            OverlayUI::ShowToast(tr("emulator_auto_loaded"));
        return;
    }
    OpenMenu();
    if (g_menuOpen)
        OverlayUI::ShowResumePrompt();
}

//==============================================================================
// Shaders (Vulkan)
//==============================================================================

#ifdef TICO_HAVE_VULKAN

static const char *kBuiltinShaderDir = "romfs:/shaders/";
static const char *kUserShaderDir = "sdmc:/tico/shaders/";

// The built-ins, with the names the menu shows for them.
static const std::pair<const char *, const char *> kBuiltinShaders[] = {
    {"xbrz.slangp", "xBRZ"},
    {"eagle.slangp", "Eagle"},
    {"crt-easymode.slangp", "CRT Easy Mode"},
};

static bool EndsWith(const std::string &s, const char *suffix)
{
    const size_t n = strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

static std::string ShaderPreset()
{
    return OverlayConfig::GetConfigValue("shader_preset", "");
}

static void SetShaderPreset(const std::string &path)
{
    OverlayConfig::SetConfigValue("shader_preset", path);
    OverlayConfig::SaveConfig();
}

static std::string ShaderPresetLabel()
{
    const std::string preset = ShaderPreset();
    if (preset.empty())
        return std::string();
    for (const auto &builtin : kBuiltinShaders)
        if (preset == kBuiltinShaderDir + std::string(builtin.first))
            return builtin.second;
    std::string name = preset;
    const size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    return EndsWith(name, ".slangp") ? name.substr(0, name.size() - 7) : name;
}

// The browser: the user folder lists the built-ins first, every other folder
// its parent; then subfolders and presets, by name.
static std::vector<OverlayUI::ShaderBrowseEntry> BrowseShaders(std::string dir)
{
    using Entry = OverlayUI::ShaderBrowseEntry;
    if (dir.empty() || dir.back() != '/')
        dir += '/';
    std::vector<Entry> entries;
    if (dir == kUserShaderDir)
    {
        entries.push_back({"> " + tr("emulator_builtin_shaders"), kBuiltinShaderDir, true});
        entries.push_back({tr("emulator_none"), "", false});
    }
    else
    {
        std::string parent = kUserShaderDir;
        if (dir != kBuiltinShaderDir)
        {
            const std::string d = dir.substr(0, dir.size() - 1);
            const size_t slash = d.find_last_of('/');
            if (slash != std::string::npos)
                parent = d.substr(0, slash + 1);
        }
        entries.push_back({"..", parent, true});
    }
    if (dir == kBuiltinShaderDir)
    {
        for (const auto &builtin : kBuiltinShaders)
            entries.push_back({builtin.second, dir + builtin.first, false});
        return entries;
    }

    std::vector<Entry> dirs, files;
    if (DIR *d = opendir(dir.c_str()))
    {
        while (struct dirent *e = readdir(d))
        {
            const std::string name = e->d_name;
            if (name.empty() || name[0] == '.')
                continue;
            const std::string path = dir + name;
            bool isDir = e->d_type == DT_DIR;
            if (e->d_type == DT_UNKNOWN)
            {
                struct stat st;
                isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            }
            if (isDir)
                dirs.push_back({name + "/", path + "/", true});
            else if (EndsWith(name, ".slangp"))
                files.push_back({name.substr(0, name.size() - 7), path, false});
        }
        closedir(d);
    }
    auto byName = [](const Entry &a, const Entry &b) {
        return strcasecmp(a.label.c_str(), b.label.c_str()) < 0;
    };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries.insert(entries.end(), dirs.begin(), dirs.end());
    entries.insert(entries.end(), files.begin(), files.end());
    return entries;
}

// Parameter overrides per preset, in mupen64plus.jsonc's shader_parameters.
static nlohmann::json ShaderParameterOverrides()
{
    const std::string text = OverlayConfig::GetConfigJson("shader_parameters");
    nlohmann::json j = text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

static void SaveShaderParameterOverrides(const nlohmann::json &j)
{
    OverlayConfig::SetConfigJson("shader_parameters", j.dump());
    OverlayConfig::SaveConfig();
}

// A preset just loaded: start from its defaults, then the saved overrides.
static void OnShaderLoaded()
{
    if (!g_chain)
        return;
    g_chain->ResetParameters();
    const nlohmann::json overrides = ShaderParameterOverrides();
    const auto it = overrides.find(ShaderPreset());
    if (it == overrides.end() || !it->is_object())
        return;
    for (const auto &param : it->items())
        if (param.value().is_number())
            g_chain->SetParameter(param.key(), param.value().get<float>());
}

static void SetShaderParameter(const std::string &id, float value)
{
    if (!g_chain)
        return;
    g_chain->SetParameter(id, value);
    nlohmann::json overrides = ShaderParameterOverrides();
    nlohmann::json &preset = overrides[ShaderPreset()];
    if (!preset.is_object())
        preset = nlohmann::json::object();
    for (const TicoSlang::Parameter &p : g_chain->Parameters())
    {
        if (p.id != id)
            continue;
        const float step = p.step > 0.0f ? p.step : 0.01f;
        if (std::fabs(value - p.initial) < step * 0.5f)
            preset.erase(id);
        else
            preset[id] = value;
    }
    if (preset.empty())
        overrides.erase(ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

static void ResetShaderParameters()
{
    if (g_chain)
        g_chain->ResetParameters();
    nlohmann::json overrides = ShaderParameterOverrides();
    overrides.erase(ShaderPreset());
    SaveShaderParameterOverrides(overrides);
}

static void RegisterShaderMenu()
{
    OverlayUI::ShaderCallbacks callbacks;
    callbacks.preset_label = [] { return ShaderPresetLabel(); };
    callbacks.browse_start = [] {
        const std::string preset = ShaderPreset();
        const size_t slash = preset.find_last_of('/');
        return slash == std::string::npos ? std::string(kUserShaderDir) : preset.substr(0, slash + 1);
    };
    callbacks.browse = [](const std::string &dir) { return BrowseShaders(dir); };
    callbacks.select = [](const std::string &path) { SetShaderPreset(path); };
    callbacks.parameters = [] {
        std::vector<OverlayUI::ShaderParameter> out;
        if (g_chain)
            for (const TicoSlang::Parameter &p : g_chain->Parameters())
                out.push_back({p.id, p.description, p.value, p.minimum, p.maximum, p.step});
        return out;
    };
    callbacks.set_parameter = [](const std::string &id, float value) { SetShaderParameter(id, value); };
    callbacks.reset_parameters = [] { ResetShaderParameters(); };
    OverlayUI::SetShaderCallbacks(std::move(callbacks));
}

// Loads the preset the settings name once it differs from the active one.
// Compiling can take a while on the Switch, so the frame before it shows a
// toast instead of the screen just freezing.
static void ApplyShaderPreset()
{
    const std::string wanted = ShaderPreset();
    if (!g_chain || wanted == g_activePreset)
        return;
    static std::string announced;
    if (announced != wanted && !wanted.empty())
    {
        announced = wanted;
        OverlayUI::ShowToast(tr("emulator_loading_shader"), OverlayUI::ToastCorner::TopRight);
        return;
    }
    announced.clear();
    std::string error;
    if (g_chain->LoadPreset(wanted, error))
    {
        g_activePreset = wanted;
        OnShaderLoaded();
        return;
    }
    LOG_ERROR("SHADER", "Cannot load %s: %s", wanted.c_str(), error.c_str());
    const std::string firstLine = error.substr(0, error.find('\n'));
    OverlayUI::ShowToast(tr("emulator_shader_failed") + ": " + firstLine.substr(0, 80),
                         OverlayUI::ToastCorner::TopRight);
    // Keep showing (and saving) what actually runs.
    if (g_activePreset == "\x01")
        g_activePreset.clear();
    SetShaderPreset(g_activePreset);
}

#endif // TICO_HAVE_VULKAN

//==============================================================================
// Quick menu
//==============================================================================

// settings.json is the one settings definition: every core option it lists
// reaches the core, with its default when the config file does not set it.
static void ApplySettingsToCore()
{
    if (!g_core)
        return;
    OverlayConfig::ApplyToCore([](const std::string &key, const std::string &value) {
        g_core->SetOption(key, value);
    });
    g_core->ApplyOptions();
}

static std::string TrFormat(const char *key, int value)
{
    SwitchFrontend::OverlayTranslation::TranslationManager::Instance().Init();
    const std::string format = tr(key);
    char text[256];
    snprintf(text, sizeof(text), format.c_str(), value);
    return text;
}

static void OpenMenu()
{
    if (!g_overlayReady || g_menuOpen)
        return;
    // Opening the menu pauses the game, which hardcore only allows so often.
    int waitSeconds = 0;
    if (g_core && !g_core->CanPause(waitSeconds))
    {
        OverlayUI::ShowToast(TrFormat("emulator_hardcore_pause_wait", waitSeconds));
        return;
    }
    g_menuOpen = true;
    g_navHeldPrev = 0;
    g_navRepeatFrames = 0;
    OverlayUI::SetHardcoreMode(g_core && g_core->IsHardcoreActive());
    StopFastForward();
    g_audio.SetPaused(true);
    ImGuiOverlay::SetVisible(true);
}

static void CloseMenu()
{
    if (!g_menuOpen)
        return;
    g_menuOpen = false;
    ImGuiOverlay::SetVisible(false);
    g_audio.SetPaused(false);
}

// The first finger on the touchscreen, for the menu (no controller needed).
static void FeedMenuTouch()
{
    OverlayUI::TouchInput touch{};
    static bool initialized = false;
    if (!initialized)
    {
        hidInitializeTouchScreen();
        initialized = true;
    }
    HidTouchScreenState state{};
    if (hidGetTouchScreenStates(&state, 1) > 0 && state.count > 0)
        touch = {true, static_cast<float>(state.touches[0].x), static_cast<float>(state.touches[0].y)};
    ImGuiOverlay::FeedTouch(touch);
}

// D-pad + left stick, edge plus hold-repeat; Switch A accepts, B goes back.
static void FeedMenu(SDL_GameController *pad)
{
    enum : uint32_t { Up = 1, Down = 2, Left = 4, Right = 8 };
    const Sint16 axisX = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 axisY = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
    uint32_t held = 0;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || axisY < -16000) held |= Up;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || axisY > 16000) held |= Down;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || axisX < -16000) held |= Left;
    if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || axisX > 16000) held |= Right;

    uint32_t fire = held & ~g_navHeldPrev; // new presses fire instantly
    if (held != 0 && held == g_navHeldPrev)
    {
        if (--g_navRepeatFrames <= 0)
        {
            fire |= held;
            g_navRepeatFrames = kNavRepeatFrames;
        }
    }
    else if (fire != 0)
    {
        g_navRepeatFrames = kNavInitialDelayFrames;
    }
    g_navHeldPrev = held;

    // SDL names buttons by position: B is the Switch A (east), A the Switch B.
    static bool acceptHeld = false;
    static bool cancelHeld = false;
    const bool accept = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
    const bool cancel = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
    ImGuiOverlay::FeedNav({
        .up = (fire & Up) != 0,
        .down = (fire & Down) != 0,
        .left = (fire & Left) != 0,
        .right = (fire & Right) != 0,
        .accept = accept && !acceptHeld,
        .cancel = cancel && !cancelHeld,
    });
    acceptHeld = accept;
    cancelHeld = cancel;
}

// Carries out what the menu chose on the last drawn frame.
static void RunMenuAction()
{
    using OverlayUI::Action;
    const Action action = ImGuiOverlay::ConsumeAction();
    if (OverlayUI::ConsumeSettingsChanged())
        ApplySettingsToCore();

    switch (action)
    {
    case Action::None:
        return;
    case Action::Resume:
        CloseMenu();
        return;
    case Action::Exit:
        LOG_INFO("HOME", "Exit requested");
        if (g_standalone)
        {
            // the library itself quits
            g_running = false;
            return;
        }
        CloseMenu();
        AutoSaveState();
        if (g_fromLibrary)
            RelaunchSelf({}); // back to the library
        else
        {
            g_exitToTico = true;
            g_running = false;
        }
        return;
    case Action::ControllerOrder:
        if (!ShowControllerOrder())
            OverlayUI::ShowToast(tr("emulator_controllers_failed"), OverlayUI::ToastCorner::TopRight);
        g_controllersDirty = true; // the players may be in another order now
        return;
    case Action::Reset:
        if (g_core)
            g_core->Reset();
        CloseMenu();
        return;
    case Action::Restart:
        // The game again from disk, as if it were started anew: this process
        // ends (saving the game) and a fresh one loads it.
        if (g_core)
        {
            CloseMenu();
            AutoSaveState();
            std::vector<std::string> args = {g_slugArg, g_core->GetGamePath(), g_titleArg, "--restart"};
            if (g_fromLibrary)
                args.push_back("--library");
            RelaunchSelf(args);
        }
        return;
    default:
        break;
    }

    if (OverlayUI::IsSaveStateAction(action) && g_core)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        const bool saved = SaveStateNow(slot - 1);
        OverlayUI::ShowToast(TrFormat(saved ? "emulator_state_saved" : "emulator_save_failed", slot));
        CloseMenu();
    }
    else if (OverlayUI::IsLoadStateAction(action) && g_core)
    {
        const int slot = OverlayUI::GetStateSlotForAction(action);
        if (g_core->IsHardcoreActive())
            OverlayUI::ShowToast(tr("emulator_hardcore_no_load"));
        else
        {
            const bool loaded = LoadStateNow(slot - 1);
            if (loaded && slot == OverlayUI::kAutoStateSlot)
                OverlayUI::ShowToast(tr("emulator_auto_loaded"));
            else
                OverlayUI::ShowToast(TrFormat(loaded ? "emulator_state_loaded" : "emulator_load_failed", slot));
        }
        CloseMenu();
    }
}

static void UpdateHud(float deltaTime)
{
    g_hudFrames++;
    g_hudSeconds += deltaTime;
    if (g_hudSeconds >= 0.5f)
    {
        g_hudFps = static_cast<float>(g_hudFrames) / g_hudSeconds;
        g_hudFrames = 0;
        g_hudSeconds = 0.0f;
    }
    OverlayUI::HudStats stats;
    stats.fps = g_hudFps;
    stats.fast_forward = g_audio.IsFastForwarding();
    if (g_core)
    {
        stats.rendered_width = g_core->GetFrameWidth();
        stats.rendered_height = g_core->GetFrameHeight();
    }
    OverlayUI::SetHudStats(stats);
}

//==============================================================================
// Input: Switch controllers to N64 pads
//==============================================================================

// Switch buttons by their Nintendo names, as the Controls tab spells them.
enum class SwitchButton
{
    A, B, X, Y, L, R, ZL, ZR, Plus, Minus, StickL, StickR, Up, Down, Left, Right, Count
};

static constexpr uint32_t SwitchBit(SwitchButton button)
{
    return 1u << static_cast<unsigned>(button);
}

static uint32_t SwitchBitFor(const std::string &name)
{
    static const std::pair<const char *, SwitchButton> kNames[] = {
        {"A", SwitchButton::A}, {"B", SwitchButton::B}, {"X", SwitchButton::X},
        {"Y", SwitchButton::Y}, {"L", SwitchButton::L}, {"R", SwitchButton::R},
        {"ZL", SwitchButton::ZL}, {"ZR", SwitchButton::ZR}, {"Plus", SwitchButton::Plus},
        {"Minus", SwitchButton::Minus}, {"StickL", SwitchButton::StickL},
        {"StickR", SwitchButton::StickR}, {"Up", SwitchButton::Up},
        {"Down", SwitchButton::Down}, {"Left", SwitchButton::Left},
        {"Right", SwitchButton::Right},
    };
    for (const auto &entry : kNames)
        if (name == entry.first)
            return SwitchBit(entry.second);
    return 0; // "None"
}

// SDL names buttons by position (Xbox layout): its B is the Switch A, its A
// the Switch B, its Y the Switch X and its X the Switch Y.
static uint32_t SwitchButtonsHeld(SDL_GameController *pad)
{
    struct SdlButton
    {
        SDL_GameControllerButton sdl;
        SwitchButton button;
    };
    static const SdlButton kButtons[] = {
        {SDL_CONTROLLER_BUTTON_B, SwitchButton::A},
        {SDL_CONTROLLER_BUTTON_A, SwitchButton::B},
        {SDL_CONTROLLER_BUTTON_Y, SwitchButton::X},
        {SDL_CONTROLLER_BUTTON_X, SwitchButton::Y},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SwitchButton::L},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SwitchButton::R},
        {SDL_CONTROLLER_BUTTON_START, SwitchButton::Plus},
        {SDL_CONTROLLER_BUTTON_BACK, SwitchButton::Minus},
        {SDL_CONTROLLER_BUTTON_LEFTSTICK, SwitchButton::StickL},
        {SDL_CONTROLLER_BUTTON_RIGHTSTICK, SwitchButton::StickR},
        {SDL_CONTROLLER_BUTTON_DPAD_UP, SwitchButton::Up},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, SwitchButton::Down},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, SwitchButton::Left},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SwitchButton::Right},
    };
    uint32_t held = 0;
    for (const SdlButton &button : kButtons)
        if (SDL_GameControllerGetButton(pad, button.sdl))
            held |= SwitchBit(button.button);
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000)
        held |= SwitchBit(SwitchButton::ZL);
    if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000)
        held |= SwitchBit(SwitchButton::ZR);
    return held;
}

// N64 buttons, with the Switch button each sits on by default (Controls >
// Button mapping). The right stick always works the C buttons too.
struct ButtonMapping
{
    const char *key;
    const char *fallback;
    uint32_t n64;
};
static const ButtonMapping kButtonMappings[] = {
    {"map_a", "A", TICO_N64_A},
    {"map_b", "B", TICO_N64_B},
    {"map_z", "ZL", TICO_N64_Z},
    {"map_z_alt", "ZR", TICO_N64_Z},
    {"map_l", "L", TICO_N64_L},
    {"map_r", "R", TICO_N64_R},
    {"map_start", "Plus", TICO_N64_START},
    {"map_c_up", "None", TICO_N64_C_UP},
    {"map_c_down", "X", TICO_N64_C_DOWN},
    {"map_c_left", "Y", TICO_N64_C_LEFT},
    {"map_c_right", "None", TICO_N64_C_RIGHT},
    {"map_up", "Up", TICO_N64_DPAD_UP},
    {"map_down", "Down", TICO_N64_DPAD_DOWN},
    {"map_left", "Left", TICO_N64_DPAD_LEFT},
    {"map_right", "Right", TICO_N64_DPAD_RIGHT},
};

// The left stick as the N64's: the deadzone cut away (so slow movements
// still register), scaled by the sensitivity, in the N64's -80..80.
static void StickToN64(int16_t x, int16_t y, int8_t &outX, int8_t &outY)
{
    constexpr double kMax = 32768.0;
    const double deadzone =
        std::atoi(OverlayConfig::GetConfigValue("mupen64plus-astick-deadzone", "15").c_str()) * 0.01 * kMax;
    const double sensitivity =
        std::atoi(OverlayConfig::GetConfigValue("mupen64plus-astick-sensitivity", "100").c_str()) / 100.0;
    double radius = std::sqrt((double)x * x + (double)y * y);
    if (radius <= deadzone)
    {
        outX = outY = 0;
        return;
    }
    const double angle = std::atan2((double)y, (double)x);
    radius = (radius - deadzone) * (kMax / (kMax - deadzone));
    radius = std::min(radius * 80.0 / kMax * sensitivity, 127.0);
    outX = (int8_t)std::lround(radius * std::cos(angle));
    outY = (int8_t)-std::lround(radius * std::sin(angle));
}

// Updates fast forward from player 1's hotkey and returns the hotkey's
// Switch button (0 when there is none), which then stays out of the game.
static uint32_t UpdateFastForward(SDL_GameController *pad)
{
    const uint32_t button = SwitchBitFor(OverlayConfig::GetConfigValue("fast_forward_hotkey", "None"));
    const bool down = pad && button && (SwitchButtonsHeld(pad) & button);
    bool active;
    if (OverlayConfig::GetConfigValue("fast_forward_mode", "hold") == "toggle")
    {
        if (down && !g_ffHotkeyHeld)
            g_ffLatched = !g_ffLatched;
        active = g_ffLatched;
    }
    else
    {
        active = down;
    }
    g_ffHotkeyHeld = down;
    if (!active)
        g_ffFrameBudget = 0.0f;
    g_audio.SetFastForward(active);
    return button;
}

static void StopFastForward()
{
    g_ffLatched = false;
    g_ffFrameBudget = 0.0f;
    g_audio.SetFastForward(false);
}

// Game frames to let through for each presented one.
static int FramesThisRefresh()
{
    if (!g_audio.IsFastForwarding())
        return 1;
    const std::string speed = OverlayConfig::GetConfigValue("fast_forward_speed", "200");
    if (speed == "unlimited")
        return 1; // vsync is off instead
    float rate = std::max(1.0f, std::atoi(speed.c_str()) / 100.0f);
    g_ffFrameBudget += rate;
    const int frames = static_cast<int>(g_ffFrameBudget);
    g_ffFrameBudget -= frames;
    return std::max(1, frames);
}

static bool FastForwardUncapped()
{
    return g_audio.IsFastForwarding() &&
           OverlayConfig::GetConfigValue("fast_forward_speed", "200") == "unlimited";
}

static void FeedPads(SDL_GameController *const *controllers, int count)
{
    if (!g_core)
        return;
    // Player 1's fast-forward hotkey: its Switch button is not mapped.
    const uint32_t ffButton = UpdateFastForward(count > 0 ? controllers[0] : nullptr);
    for (int p = 0; p < 4; p++)
    {
        SDL_GameController *pad = p < count ? controllers[p] : nullptr;
        if (!pad)
        {
            g_core->SetPad((unsigned)p, p == 0, 0, 0, 0);
            continue;
        }
        uint32_t held = SwitchButtonsHeld(pad);
        if (p == 0)
            held &= ~ffButton;

        uint32_t buttons = 0;
        for (const ButtonMapping &mapping : kButtonMappings)
        {
            const uint32_t bit = SwitchBitFor(OverlayConfig::GetConfigValue(mapping.key, mapping.fallback));
            if (held & bit)
                buttons |= mapping.n64;
        }
        const int16_t rx = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTX);
        const int16_t ry = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        if (rx < -16384) buttons |= TICO_N64_C_LEFT;
        if (rx > 16384) buttons |= TICO_N64_C_RIGHT;
        if (ry < -16384) buttons |= TICO_N64_C_UP;
        if (ry > 16384) buttons |= TICO_N64_C_DOWN;

        int8_t sx = 0, sy = 0;
        StickToN64(SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX),
                   SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY), sx, sy);
        g_core->SetPad((unsigned)p, true, buttons, sx, sy);
    }
}

static void ReleaseAllPads()
{
    if (!g_core)
        return;
    for (unsigned p = 0; p < 4; p++)
        g_core->SetPad(p, p == 0, 0, 0, 0);
}

//==============================================================================
// Library (standalone launch)
//==============================================================================

static const char *kRomExtensions[] = {".z64", ".n64", ".v64", ".zip", ".7z", ".rar"};

static std::string LowerExtension(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return std::string();
    std::string ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return ext;
}

// The consoles the library lists, each with its own folders.
struct LibraryConsole
{
    const char *slug;
    const char *title;
};
static const LibraryConsole kLibraryConsoles[] = {
    {"n64", "Nintendo 64"},
};

static std::string WithSlash(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if (!path.empty() && path.back() != '/')
        path += '/';
    return path;
}

// tico's ROM bases (general.jsonc): the ROMs path, then the extra bases. A
// console's games are in <base>/<slug>/ under each, as tico scans them.
static std::vector<std::string> TicoRomBases()
{
    std::vector<std::string> bases;
    tico::SettingsStream file("general");
    const nlohmann::json j = file.good() ? nlohmann::json::parse(file, nullptr, false, true) : nlohmann::json();
    std::string roms = j.is_object() ? j.value("roms_path", std::string()) : std::string();
    bases.push_back(WithSlash(roms.empty() ? "sdmc:/tico/roms/" : roms));
    if (j.is_object() && j.contains("rom_base_paths") && j["rom_base_paths"].is_array())
        for (const auto &base : j["rom_base_paths"])
            if (base.is_string() && !base.get<std::string>().empty())
                bases.push_back(WithSlash(base.get<std::string>()));
    return bases;
}

// The module's own folders per console (tico_rom_folders in
// mupen64plus.jsonc), the same list tico's Paths tab edits.
static nlohmann::json ModuleRomFolders()
{
    const std::string text = OverlayConfig::GetConfigJson("tico_rom_folders");
    nlohmann::json j = text.empty() ? nlohmann::json::object() : nlohmann::json::parse(text, nullptr, false);
    return j.is_object() ? j : nlohmann::json::object();
}

static std::vector<std::string> ModuleRomFolders(const std::string &slug)
{
    std::vector<std::string> folders;
    const nlohmann::json all = ModuleRomFolders();
    const auto it = all.find(slug);
    if (it != all.end() && it->is_array())
        for (const auto &entry : *it)
            if (entry.is_string() && !entry.get<std::string>().empty())
                folders.push_back(WithSlash(entry.get<std::string>()));
    return folders;
}

static void SetModuleRomFolders(const std::string &slug, const std::vector<std::string> &folders)
{
    nlohmann::json all = ModuleRomFolders();
    if (folders.empty())
        all.erase(slug);
    else
        all[slug] = folders;
    OverlayConfig::SetConfigJson("tico_rom_folders", all.dump());
    OverlayConfig::SaveConfig();
}

// Every folder a console's games are read from: each base's <base>/<slug>/,
// then the module's own folders.
static std::vector<std::string> RomFoldersFor(const std::string &slug)
{
    std::vector<std::string> folders;
    auto add = [&](const std::string &folder) {
        // a folder on a USB drive is read through the drive's current mount,
        // and left out while the drive is not connected
        const std::string mounted = UsbStorage::Resolve(folder);
        if (!mounted.empty() && std::find(folders.begin(), folders.end(), mounted) == folders.end())
            folders.push_back(mounted);
    };
    for (const std::string &base : TicoRomBases())
        add(base + slug + "/");
    for (const std::string &folder : ModuleRomFolders(slug))
        add(folder);
    return folders;
}

// The console of each listed game, by path: the folder list it was found in.
static std::map<std::string, std::string> g_librarySlugs;

static void ScanRomFolder(const std::string &dir, int depth, std::vector<std::string> &out)
{
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d))
    {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = (dir.back() == '/' ? dir : dir + "/") + name;
        bool isDir = e->d_type == DT_DIR;
        if (e->d_type == DT_UNKNOWN)
        {
            struct stat st;
            isDir = stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isDir)
        {
            if (depth > 0)
                ScanRomFolder(path, depth - 1, out);
            continue;
        }
        const std::string ext = LowerExtension(name);
        for (const char *known : kRomExtensions)
            if (ext == known)
                out.push_back(path);
    }
    closedir(d);
}

static std::vector<OverlayUI::LibraryEntry> ListLibrary()
{
    g_librarySlugs.clear();
    std::vector<OverlayUI::LibraryEntry> entries;
    for (const LibraryConsole &console : kLibraryConsoles)
    {
        std::vector<std::string> roms;
        for (const std::string &folder : RomFoldersFor(console.slug))
            ScanRomFolder(folder, 2, roms);
        std::sort(roms.begin(), roms.end());
        roms.erase(std::unique(roms.begin(), roms.end()), roms.end());

        std::string detail = console.slug;
        std::transform(detail.begin(), detail.end(), detail.begin(),
                       [](unsigned char c) { return (char)std::toupper(c); });
        for (const std::string &path : roms)
        {
            if (!g_librarySlugs.emplace(path, console.slug).second)
                continue; // listed under the first console that has it
            const std::string filename = path.substr(path.find_last_of('/') + 1);
            std::string title = TicoUtils::GetCleanTitle(filename);
            if (title.empty())
                title = filename;
            entries.push_back({title, detail, path});
        }
    }
    std::sort(entries.begin(), entries.end(), [](const auto &a, const auto &b) {
        return strcasecmp(a.title.c_str(), b.title.c_str()) < 0;
    });
    return entries;
}

static void RegisterLibrary()
{
    OverlayUI::LibraryCallbacks library;
    library.list = [] { return ListLibrary(); };
    // a game starts in a fresh process; leaving it comes back here
    library.launch = [](const std::string &path) {
        const auto it = g_librarySlugs.find(path);
        RelaunchSelf({it != g_librarySlugs.end() ? it->second : "n64", path, "", "--library"});
    };
    OverlayUI::SetLibraryCallbacks(std::move(library));

    OverlayUI::LibraryFolderCallbacks folders;
    folders.groups = [] {
        std::vector<OverlayUI::LibraryFolderGroup> groups;
        const std::vector<std::string> bases = TicoRomBases();
        for (const LibraryConsole &console : kLibraryConsoles)
        {
            OverlayUI::LibraryFolderGroup group;
            group.label = console.title;
            for (const std::string &base : bases)
                group.bases.push_back(base + console.slug + "/");
            group.folders = ModuleRomFolders(console.slug);
            groups.push_back(std::move(group));
        }
        return groups;
    };
    folders.set = [](int group, const std::vector<std::string> &paths) {
        if (group >= 0 && group < (int)(sizeof(kLibraryConsoles) / sizeof(kLibraryConsoles[0])))
        {
            std::vector<std::string> normalized;
            for (const std::string &path : paths)
                normalized.push_back(WithSlash(path));
            SetModuleRomFolders(kLibraryConsoles[group].slug, normalized);
        }
    };
    OverlayUI::SetLibraryFolderCallbacks(std::move(folders));
}

//==============================================================================
// Starting a game
//==============================================================================

static bool StartGame(const std::string &slug, const std::string &romArg, const std::string &titleArg)
{
    // tico names a game on a USB drive by the drive's id: find where it is mounted
    std::string romPath = UsbStorage::Resolve(romArg);
    if (romPath.empty())
    {
        LOG_ERROR("HOME", "USB drive for %s is not connected", romArg.c_str());
        romPath = romArg;
    }
    TicoConfig::SetSlug(slug);
    g_slugArg = slug;
    g_titleArg = titleArg;
    LOG_INFO("HOME", "Console slug: %s, ROM: %s, renderer: %s", slug.c_str(), romPath.c_str(),
             TicoRenderer::Name());
    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    TicoConfig::MakeDirs(TicoConfig::StatesPath());
    TicoConfig::MakeDirs(TicoConfig::SystemPath());

    // this game's own settings (Settings > This Game), if it has them, over the core's
    OverlayConfig::SetGame(romPath);
    g_core = std::make_unique<TicoCore>();
    g_core->SetUseGLideN64(!TicoRenderer::IsVulkan());
    g_core->EnsureConfigLoaded();
    OverlayConfig::ApplyToCore([](const std::string &key, const std::string &value) {
        g_core->SetOption(key, value);
    });
    g_core->SetAudioCallbacks(AudioRateCallback, AudioSampleBatchCallback, AudioFlushCallback);

    TicoCoreHooks hooks;
#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
    {
        hooks.createVulkanDevice = CreateVulkanDeviceForCore;
        hooks.presentVulkan = PresentVulkanFromCore;
    }
    else
#endif
    {
        hooks.emuThreadBegin = [] { TicoGL::BeginCoreThread(); };
        hooks.emuThreadEnd = [] { TicoGL::EndCoreThread(); };
        hooks.presentGL = PresentGLFromCore;
        hooks.glFramebuffer = [](unsigned w, unsigned h) { return TicoGL::CoreFramebuffer(w, h); };
        hooks.glProcAddress = [](const char *name) { return TicoGL::GetProcAddress(name); };
    }
    g_core->SetHooks(std::move(hooks));

    const size_t lastSlash = romPath.find_last_of("/\\");
    const std::string filename = lastSlash != std::string::npos ? romPath.substr(lastSlash + 1) : romPath;
    // Prefer the launcher-supplied title; fall back to the rom filename.
    std::string cleanTitle = titleArg.empty() ? TicoUtils::GetCleanTitle(filename) : titleArg;
    if (cleanTitle.empty())
        cleanTitle = filename;
    OverlayUI::SetGameTitle(cleanTitle);
    OverlayUI::SetLibraryMode(false);

    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "Failed to load ROM: %s", romPath.c_str());
        return false;
    }
    ReleaseAllPads();
    if (!g_core->Start())
    {
        LOG_ERROR("HOME", "The emulator did not start");
        return false;
    }
    g_offerResume = true;
    return true;
}

//==============================================================================
// Drawing
//==============================================================================

// The game's on-screen rectangle, from the Display tab: Integer scales the
// frame by 1x, 2x or the largest that fits ("Auto"); Display fits an aspect
// ratio (4:3, 16:9, the core's own "Original") or stretches.
static ImVec4 ComputeGameRect(ImVec2 displaySize)
{
    const int width = g_core ? g_core->GetFrameWidth() : 640;
    const int height = g_core ? g_core->GetFrameHeight() : 480;
    const float aspectRatio = g_core ? g_core->GetAspectRatio() : 4.0f / 3.0f;
    const std::string mode = OverlayConfig::GetConfigValue("display_mode", "Display");
    const std::string size = OverlayConfig::GetConfigValue("display_size", "4:3");

    const float baseW = width > 0 ? static_cast<float>(width) : 640.0f;
    const float baseH = height > 0 ? static_cast<float>(height) : 480.0f;
    float dstWidth = displaySize.x;
    float dstHeight = displaySize.y;
    if (mode == "Integer")
    {
        int scale;
        if (size == "1x")
            scale = 1;
        else if (size == "2x")
            scale = 2;
        else
            scale = std::max(1, std::min(static_cast<int>(displaySize.x / baseW),
                                         static_cast<int>(displaySize.y / baseH)));
        dstWidth = std::min(displaySize.x, baseW * scale);
        dstHeight = std::min(displaySize.y, baseH * scale);
    }
    else if (size != "Stretch")
    {
        float ar = aspectRatio > 0.0f ? aspectRatio : baseW / baseH;
        if (size == "4:3")
            ar = 4.0f / 3.0f;
        else if (size == "16:9")
            ar = 16.0f / 9.0f;
        if (ar > displaySize.x / displaySize.y)
        {
            dstWidth = displaySize.x;
            dstHeight = displaySize.x / ar;
        }
        else
        {
            dstHeight = displaySize.y;
            dstWidth = displaySize.y * ar;
        }
    }
    dstWidth = std::floor(dstWidth);
    dstHeight = std::floor(dstHeight);
    return ImVec4(std::floor((displaySize.x - dstWidth) / 2.0f), std::floor((displaySize.y - dstHeight) / 2.0f),
                  dstWidth, dstHeight);
}

// The frame through the shader chain (Vulkan) or as GLideN64 drew it (GL).
static void DrawGame(GameCommandBuffer cmd, ImDrawList *dl, ImVec2 displaySize, const PostedFrame *frame)
{
    dl->AddRectFilled(ImVec2(0, 0), displaySize, IM_COL32(0, 0, 0, 255));
    if (!g_core || !frame)
        return; // the library: no game, and no stale frame behind it
    const ImVec4 rect = ComputeGameRect(displaySize);
    if (rect.z < 1.0f || rect.w < 1.0f)
        return;
    const ImVec2 p0(rect.x, rect.y), p1(rect.x + rect.z, rect.y + rect.w);

#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
    {
        if (!g_chain || !cmd)
            return;
        const float ar = g_core->GetAspectRatio();
        const ImTextureID tex = g_chain->Process(cmd, (uint32_t)rect.z, (uint32_t)rect.w, ar, g_core->GetFPS());
        if (tex != ImTextureID_Invalid)
            dl->AddImage(tex, p0, p1);
        return;
    }
#else
    (void)cmd;
#endif
    uint32_t w = 0, h = 0;
    const ImTextureID tex = TicoGL::CoreFrameTexture(frame->slot, w, h);
    if (tex != ImTextureID_Invalid)
        dl->AddImage(tex, p0, p1, ImVec2(0, 1), ImVec2(1, 0)); // GL rows run bottom-up
}

static void DrawOSD()
{
    if (!g_core || g_core->GetOSDFrames() <= 0)
        return;
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    const float marginX = 24.0f, marginY = 16.0f, padX = 16.0f, padY = 8.0f, rounding = 14.0f;
    const int frames = g_core->GetOSDFrames();
    const float alpha = frames < 30 ? frames / 30.0f : 1.0f;
    const std::string msg = g_core->GetOSDMessage();
    const ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
    fg->AddRectFilled(ImVec2(marginX, marginY), ImVec2(marginX + textSize.x + padX * 2, marginY + textSize.y + padY * 2),
                      IM_COL32(0, 0, 0, (int)(alpha * 153)), rounding);
    fg->AddText(ImVec2(marginX + padX, marginY + padY), IM_COL32(255, 255, 255, (int)(alpha * 240)), msg.c_str());
    g_core->DecrementOSD();
}

// Draws the frame `frame` (or none) with the overlay and presents it.
// `newFrame` is set when the emulator posted it since the last present.
static void Present(const PostedFrame *frame, bool newFrame)
{
    UpdateScreenMode();

    int logW, logH;
    GetDisplayResolution(logW, logH);
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)logW, (float)logH);
    io.DeltaTime = 1.0f / 60.0f;
    const ImVec2 displaySize((float)logW, (float)logH);

    GameCommandBuffer cmd = nullptr;
#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
    {
        // A skipped frame (swapchain being recreated) still lets the
        // emulator go on.
        cmd = frame ? TicoVulkan::BeginFrame(frame->slot) : TicoVulkan::BeginFrame();
        VkImage image;
        VkImageLayout layout;
        if (cmd && frame && newFrame && g_chain && TicoVulkan::CoreImage(frame->slot, image, layout))
            g_chain->SetSourceImage(image, layout, frame->width, frame->height);
        ApplyShaderPreset();
    }
    else
#endif
    {
        TicoGL::BeginFrame();
    }

    ImGui::NewFrame();
    DrawGame(cmd, ImGui::GetBackgroundDrawList(), displaySize, frame);
    UpdateHud(io.DeltaTime);
    ImGuiOverlay::Draw(g_core.get(), displaySize.x, displaySize.y, io.DeltaTime);
    DrawOSD();
    ImGui::Render();

#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
    {
        if (cmd)
            TicoVulkan::EndFrame(ImGui::GetDrawData());
    }
    else
#endif
    {
        TicoGL::EndFrame(ImGui::GetDrawData(), frame ? (int)frame->slot : -1);
    }
    if (frame)
    {
        g_shownFrame = *frame;
        g_haveShownFrame = true;
    }
    g_presented++;
}

//==============================================================================
// Input and menu, once per presented frame
//==============================================================================

static void HandleInput()
{
    if (g_controllersDirty)
        RefreshControllers();
    SDL_GameController *controllers[4] = {nullptr, nullptr, nullptr, nullptr};
    int count = 0;
    for (SDL_GameController *controller : g_controllers)
        if (controller)
            controllers[count++] = controller;

    RunMenuAction();
    if (!g_running)
        return;

    SDL_GameController *pad = count > 0 ? controllers[0] : nullptr;
    if (pad && g_overlayReady)
    {
        // Guide, or Plus+Minus, opens the menu and closes it again.
        const bool start = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
        const bool select = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK);
        const bool guide = SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE);
        const bool toggle = guide || (start && select);
        // the library stays open while no game runs
        if (toggle && !g_toggleHeld && g_core)
        {
            if (g_menuOpen)
                CloseMenu();
            else
                OpenMenu();
        }
        g_toggleHeld = toggle;
        if (toggle && g_core)
        {
            ReleaseAllPads();
            return;
        }
    }
    if (g_menuOpen)
    {
        ReleaseAllPads();
        if (pad)
            FeedMenu(pad);
        FeedMenuTouch();
        return;
    }
    FeedPads(controllers, count);
}

//==============================================================================
// Profiler
//==============================================================================

// Sampling profiler for the emulation thread: every 5 ms a thread on core 2
// pauses it and records its PC. PCs inside the NRO are bucketed by offset
// (resolve with tools/resolve_profile.py); PCs in the R4300 dynarec cache and
// in other JIT code (paraLLEl-RSP) are counted separately. Opt-in: create
// profile.on in TicoConfig::PROFILE_DIR (it may hold the first frame to
// sample, default 600); 600 presented frames are sampled.
extern "C" void *mupen_jit_rx_addr;
extern "C" void _start();

namespace {
constexpr u32 kProfileBucketShift = 4;
constexpr u32 kProfileBuckets = 0x4000000 >> kProfileBucketShift;
constexpr u64 kProfileIntervalNs = 5000000;
constexpr size_t kProfileStackDepth = 16;
constexpr size_t kProfileMaxStacks = 16384;
constexpr size_t kR4300JitSize = 1u << 25;

void CollectStack(const ThreadContext &ctx, u64 base, std::array<u64, kProfileStackDepth> &out)
{
    out.fill(0);
    size_t n = 0;
    auto push = [&](u64 address) {
        if (n < out.size())
            out[n++] = address >= base ? address - base : 0xFFFFFFFFull;
    };
    push(ctx.pc.x);
    push(ctx.lr);
    const u64 stackLow = ctx.sp;
    const u64 stackHigh = ctx.sp + 0x400000;
    u64 fp = ctx.fp;
    while (n < out.size() && fp >= stackLow && fp + 16 <= stackHigh && (fp & 0xF) == 0)
    {
        const u64 *record = reinterpret_cast<const u64 *>(fp);
        const u64 next = record[0];
        const u64 ret = record[1];
        if (ret == 0)
            break;
        push(ret);
        if (next <= fp)
            break;
        fp = next;
    }
}

struct Profiler
{
    ::Thread Worker{};
    std::atomic<bool> Running{false};
    std::mutex DataMutex;
    u32 *Buckets = nullptr;
    u32 R4300Jit = 0;
    u32 OtherJit = 0;
    u32 Total = 0;
    u32 Failed = 0;
    Handle Target = 0;
    std::vector<std::array<u64, kProfileStackDepth>> Stacks;
};
Profiler Prof;

void ProfilerThread(void *)
{
    const u64 base = (u64)&_start;
    while (Prof.Running)
    {
        svcSleepThread(kProfileIntervalNs);
        if (!Prof.Running)
            break;
        ThreadContext ctx;
        std::array<u64, kProfileStackDepth> stack;
        if (R_FAILED(svcSetThreadActivity(Prof.Target, ThreadActivity_Paused)))
        {
            std::lock_guard<std::mutex> lock(Prof.DataMutex);
            Prof.Failed++;
            continue;
        }
        Result rc = svcGetThreadContext3(&ctx, Prof.Target);
        // the stack is only stable while the thread is paused
        if (R_SUCCEEDED(rc))
            CollectStack(ctx, base, stack);
        svcSetThreadActivity(Prof.Target, ThreadActivity_Runnable);
        // Never acquire the data mutex while the target thread is paused.
        std::lock_guard<std::mutex> lock(Prof.DataMutex);
        if (R_FAILED(rc))
        {
            Prof.Failed++;
            continue;
        }
        Prof.Total++;
        if (Prof.Stacks.size() < kProfileMaxStacks)
            Prof.Stacks.push_back(stack);
        const u64 pc = ctx.pc.x;
        const u64 jit = (u64)mupen_jit_rx_addr;
        const u64 offset = pc - base;
        if (jit && pc >= jit && pc < jit + kR4300JitSize)
            Prof.R4300Jit++;
        else if (pc >= base && (offset >> kProfileBucketShift) < kProfileBuckets)
            Prof.Buckets[offset >> kProfileBucketShift]++;
        else
            Prof.OtherJit++;
    }
}

void WriteProfile()
{
    if (!Prof.Buckets)
        return;
    std::lock_guard<std::mutex> lock(Prof.DataMutex);
    const std::string dir = TicoConfig::PROFILE_DIR;
    FILE *f = fopen((dir + "profile.txt").c_str(), "w");
    if (!f)
        return;
    fprintf(f, "total %u r4300_jit %u other_jit %u failed %u\n", Prof.Total, Prof.R4300Jit, Prof.OtherJit, Prof.Failed);
    fprintf(f, "# interval_ns %llu\n", (unsigned long long)kProfileIntervalNs);
    for (u32 i = 0; i < kProfileBuckets; i++)
        if (Prof.Buckets[i])
            fprintf(f, "%x %u\n", i << kProfileBucketShift, Prof.Buckets[i]);
    fclose(f);

    // one sample per line: hex NRO offsets, innermost first
    FILE *stacks = fopen((dir + "stacks.txt").c_str(), "w");
    if (!stacks)
        return;
    for (const auto &stack : Prof.Stacks)
    {
        for (u64 address : stack)
        {
            if (address == 0)
                break;
            fprintf(stacks, "%llx ", (unsigned long long)address);
        }
        fputc('\n', stacks);
    }
    fclose(stacks);
}

void StartProfiler()
{
    Prof.Target = tico_m64p_emu_thread_handle();
    if (!Prof.Target)
        return;
    Prof.Buckets = (u32 *)calloc(kProfileBuckets, sizeof(u32));
    if (!Prof.Buckets)
        return;
    Prof.Stacks.reserve(kProfileMaxStacks);
    Prof.Running = true;
    const Result created = threadCreate(&Prof.Worker, ProfilerThread, nullptr, nullptr, 0x10000, 0x2C, 2);
    if (R_FAILED(created) || R_FAILED(threadStart(&Prof.Worker)))
    {
        if (R_SUCCEEDED(created))
            threadClose(&Prof.Worker);
        Prof.Running = false;
        LOG_WARN("PROFILE", "profiler thread could not be started");
        return;
    }
    LOG_WARN("PROFILE", "sampling the emulation thread");
    WriteProfile(); // replace an older run's profile right away
}

void StopProfiler()
{
    if (Prof.Running)
    {
        Prof.Running = false;
        threadWaitForExit(&Prof.Worker);
        threadClose(&Prof.Worker);
        LOG_WARN("PROFILE", "wrote profile.txt (%u samples)", Prof.Total);
    }
    WriteProfile();
}
} // namespace

//==============================================================================
// Main
//==============================================================================

// What the process still has mapped beside its heap and code when it leaves:
// hbloader reuses this process for the next NRO and cannot load it over
// memory something still holds (a GPU mapping, a JIT, transfer memory).
static void LogMemoryMap()
{
    static const char *const kTypes[] = {
        "Unmapped", "Io", "Normal", "CodeStatic", "CodeMutable", "Heap", "SharedMem", "WeirdMapped",
        "ModuleCodeStatic", "ModuleCodeMutable", "IpcBuffer0", "MappedMemory", "ThreadLocal",
        "TransferMemIsolated", "TransferMem", "ProcessMem", "Reserved", "IpcBuffer1", "IpcBuffer3",
        "KernelStack", "CodeReadOnly", "CodeWritable", "Coverage", "Insecure"};
    u64 addr = 0;
    for (int i = 0; i < 4096; i++)
    {
        MemoryInfo info = {};
        u32 page = 0;
        if (R_FAILED(svcQueryMemory(&info, &page, addr)))
            break;
        const u32 type = info.type & 0xFF;
        if (type != MemType_Unmapped)
            LOG_INFO("MEMORY", "%010llx +%010llx %-20s attr=%x perm=%x ipc=%u dev=%u", (unsigned long long)info.addr,
                     (unsigned long long)info.size, type < sizeof(kTypes) / sizeof(kTypes[0]) ? kTypes[type] : "?",
                     info.attr, info.perm, info.ipc_refcount, info.device_refcount);
        const u64 next = info.addr + info.size;
        if (next <= addr)
            break;
        addr = next;
    }
}

// Leaves to whatever envSetNextLoad named (tico, or this NRO again), as
// DrasticDS does: straight through libnx's exit, without the C++ static
// destructors and atexit handlers, which would run the plugins' and Mesa's
// teardown again after the frontend already did it.
extern "C" void NX_NORETURN __libnx_exit(int rc);
// paraLLEl-RSP's JIT CPU is a global whose constructor maps 2 MB of JIT code
// memory, on every renderer; only its destructor would unmap it.
extern "C" void parallel_rsp_release_jit(void);
[[noreturn]] static void LeaveProcess()
{
    parallel_rsp_release_jit();
    LogMemoryMap();
    LOG_INFO("HOME", "Clean exit");
    Logger::Instance().CloseLogFile();
    __libnx_exit(0);
}

// Stops what main started besides the emulator, which is gone by now, and
// leaves. Every way out goes through here: hbloader unmaps this NRO as soon
// as the process leaves, and a thread still running then (SDL's audio thread,
// the USB drives') faults in code that is no longer there.
[[noreturn]] static void ShutdownAndLeave()
{
    LOG_INFO("HOME", "cleanup: audio");
    g_audio.Shutdown();
    Mix_CloseAudio();

    CloseControllers();
    LOG_INFO("HOME", "cleanup: renderer");
    ShutdownRenderer();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    LOG_INFO("HOME", "cleanup: SDL");
    SDL_Quit();

    LOG_INFO("HOME", "cleanup: USB drives");
    UsbStorage::Shutdown(); // flush and unmount before tico takes over again
    LOG_INFO("HOME", "cleanup: network");
    curl_global_cleanup();
    socketExit();
    romfsExit();
    appletUnlockExit();
    LeaveProcess();
}

// tico launches with argv[1] = console slug, argv[2] = ROM path,
// argv[3] = title. Restart and the library add --restart / --library.
static void ParseLaunch(int argc, char *argv[], std::string &slug, std::string &rom, std::string &title,
                        bool &restart)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++)
    {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--restart")
            restart = true;
        else if (arg == "--library")
            g_fromLibrary = true;
        else
            args.push_back(arg);
    }
    if (args.size() >= 2 && args[0].find('/') != std::string::npos)
    {
        // tico before {slug} in the launch line: ROM, title
        slug = "n64";
        rom = args[0];
        title = args[1];
    }
    else if (args.size() >= 2)
    {
        slug = args[0];
        rom = args[1];
        title = args.size() >= 3 ? args[2] : "";
    }
    else if (args.size() == 1)
    {
        slug = "n64";
        rom = args[0];
    }
}

int main(int argc, char *argv[])
{
    Logger::Instance().StartNewLogFile();
    g_selfPath = argc > 0 && argv[0] ? argv[0] : "sdmc:/switch/tico-mupen64plus.nro";

    appletLockExit();
    Result romfsRc = romfsInit();
    if (R_FAILED(romfsRc))
        LOG_WARN("HOME", "romfsInit failed: 0x%x", romfsRc);
    if (R_SUCCEEDED(socketInitializeDefault()))
        curl_global_init(CURL_GLOBAL_DEFAULT);
    else
        LOG_ERROR("HOME", "socketInitializeDefault failed");
    // USB drives mount in the background while the rest starts
    UsbStorage::Init();

    // the renderer is picked in tico's settings and holds for this run
    OverlayConfig::ReloadConfig();
    TicoRenderer::Select(TicoRenderer::FromSetting(OverlayConfig::GetConfigValue("tico_renderer", "vk")));

    std::string slug, rom, title;
    bool restart = false;
    ParseLaunch(argc, argv, slug, rom, title, restart);
    g_standalone = rom.empty();
    LOG_INFO("HOME", "mupen64plus starting (%s, %s)", g_standalone ? "library" : rom.c_str(), TicoRenderer::Name());

    if (!InitSDL())
    {
        ChainloadTico();
        ShutdownAndLeave();
    }
    ApplySwitchPerformanceProfile();
    PinCurrentThreadToCore(2, "main/render");
    UpdateScreenMode();

    if (!InitImGui() || !InitRenderer(!g_standalone))
    {
        LOG_ERROR("HOME", "Failed to initialize the %s renderer", TicoRenderer::Name());
        ChainloadTico();
        ShutdownAndLeave();
    }
    g_lastOperationMode = 255;

    if (!g_audio.Init(g_audioDevice))
        LOG_WARN("HOME", "TicoAudio init failed");

    bool started = false;
    if (!g_standalone)
        started = StartGame(slug, rom, title);
    if (!started && !g_standalone)
    {
        // nothing to show: the renderer may not even exist (Vulkan waits
        // for the game), so go back where the player came from
        LOG_ERROR("HOME", "Could not start %s", rom.c_str());
        if (g_fromLibrary)
            RelaunchSelf({});
        else
            ChainloadTico();
        g_core.reset();
        ShutdownAndLeave();
    }
    if (restart)
        g_offerResume = false; // Restart means from the start

    g_overlayReady = ImGuiOverlay::Init();
    // Save/Load State show each slot's picture and when it was saved.
    static std::array<ImTextureID, 6> slotPictures{};
    OverlayUI::SetSlotPreviewCallback([](int slot) {
        OverlayUI::SlotPreview preview;
        if (slot < 1 || slot > (int)slotPictures.size() || !g_core)
            return preview;
        ImTextureID &picture = slotPictures[slot - 1];
        TicoRenderer::DestroyTexture(picture); // the slot may have been saved again
        picture = ImTextureID_Invalid;
        const std::string path = StatePath(slot - 1);
        struct stat st;
        if (stat(path.c_str(), &st) != 0)
            return preview;
        char when[32];
        std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&st.st_mtime));
        preview.saved_at = when;
        int w = 0, h = 0, channels = 0;
        if (unsigned char *rgba = stbi_load((path + ".png").c_str(), &w, &h, &channels, 4))
        {
            picture = TicoRenderer::CreateTextureRGBA(rgba, w, h);
            stbi_image_free(rgba);
        }
        preview.texture = (unsigned long long)picture;
        if (g_core->GetAspectRatio() > 0.1f)
            preview.aspect = g_core->GetAspectRatio();
        return preview;
    });
    // Settings > Players: who is which player, and the system's screen to change it
    OverlayUI::PlayerCallbacks players;
    players.ports = [] { return ControllerNames(); };
    OverlayUI::SetPlayerCallbacks(std::move(players));
    // Cheats from the game's .cht/.cheats file; the menu hides them in hardcore.
    OverlayUI::SetCheatCallbacks(
        [] {
            std::vector<OverlayUI::CheatMenuEntry> entries;
            if (!g_core)
                return entries;
            const auto &cheats = g_core->GetCheats();
            for (size_t i = 0; i < cheats.size(); ++i)
                entries.push_back({cheats[i].name, cheats[i].enabled, true, (int)i, false});
            return entries;
        },
        [](int index) {
            if (!g_core || index < 0)
                return false;
            g_core->ToggleCheat((size_t)index);
            return true;
        });
    OverlayUI::SetSlotOccupiedCallback([](int slot) {
        struct stat st;
        return g_core && slot >= 1 && stat(StatePath(slot - 1).c_str(), &st) == 0;
    });
#ifdef TICO_HAVE_VULKAN
    if (TicoRenderer::IsVulkan())
        RegisterShaderMenu();
#endif
    OverlayUI::ReloadSettings();

    if (g_standalone)
    {
        RegisterLibrary();
        OverlayUI::SetGameTitle("Mupen64Plus");
        OverlayConfig::SetGame(std::string()); // the library has no game settings
        OverlayUI::SetLibraryMode(true);
        OpenMenu();
    }

    // Opt-in profiler: create profile.on in TicoConfig::PROFILE_DIR.
    const std::string profileFlag = std::string(TicoConfig::PROFILE_DIR) + "profile.on";
    uint64_t profileStart = 600;
    bool profiling = false;
    bool profileTaken = false;
    if (FILE *flag = fopen(profileFlag.c_str(), "r"))
    {
        profiling = !g_standalone;
        unsigned long long start = 0;
        if (fscanf(flag, "%llu", &start) == 1)
            profileStart = start;
        fclose(flag);
    }

    // Presentation paces everything: FIFO vsync on the main thread, and the
    // emulator through the handoff. PAL games (50 Hz) take a new frame on
    // only five of every six refreshes.
    bool lastUncapped = false;
    double frameAccum = 0.0;
    bool haveFrame = false;
    PostedFrame shown;
    uint64_t shownSerial = 0;

    while (g_running)
    {
        if (!appletMainLoop())
        {
            LOG_INFO("HOME", "appletMainLoop returned false, exiting");
            g_running = false;
            break;
        }

        const bool uncapped = FastForwardUncapped();
        if (uncapped != lastUncapped)
        {
#ifdef TICO_HAVE_VULKAN
            if (TicoRenderer::IsVulkan())
                TicoVulkan::SetVsync(!uncapped);
            else
#endif
                TicoGL::SetVsync(!uncapped);
            lastUncapped = uncapped;
        }

        ProcessEvents();
        HandleInput();
        if (!g_running)
            break;
        // what the game saved since the last loop reaches the SD card now,
        // not only on a clean exit
        if (g_core)
            g_core->WriteChangedSaves();

        const bool paused = g_menuOpen || !g_core;
        if (paused)
        {
            // the emulator stays blocked on the frame it posted; keep the
            // RetroAchievements session alive and show the last frame
            if (g_core)
                g_core->Idle();
            PostedFrame posted;
            if ((!haveFrame || g_refreshShownFrame) && PeekFrame(posted, std::chrono::milliseconds(0)))
            {
                shown = posted;
                haveFrame = true;
            }
            g_refreshShownFrame = false;
            const bool fresh = haveFrame && shown.serial != shownSerial;
            Present(haveFrame ? &shown : nullptr, fresh);
            shownSerial = shown.serial;
            continue;
        }

        // How many of the game's frames this refresh shows: fast forward lets
        // extra ones through unshown, PAL holds one back now and then.
        const double fps = g_core->GetFPS();
        frameAccum += fps < 55.0 ? fps / 60.0 : 1.0;
        int take = 0;
        while (frameAccum >= 1.0)
        {
            frameAccum -= 1.0;
            take++;
        }
        if (take > 0)
            take = FramesThisRefresh();

        bool took = false;
        for (int i = 0; i < take; i++)
        {
            PostedFrame posted;
            if (!PeekFrame(posted, std::chrono::milliseconds(20)))
                break;
            if (i + 1 < take)
            {
                ReleaseFrame(); // fast forward: run on without showing it
                continue;
            }
            shown = posted;
            haveFrame = took = true;
        }
        // a frame held by the menu comes back here once, not as a new one
        const bool fresh = took && shown.serial != shownSerial;
        Present(haveFrame ? &shown : nullptr, fresh);
        shownSerial = shown.serial;
        // the emulator may render into the next slot now that this one is
        // submitted
        if (took)
            ReleaseFrame();

        if (g_offerResume && fresh)
            OfferResume();

        if (profiling)
        {
            const uint64_t frames = g_presented.load(std::memory_order_relaxed);
            if (Prof.Running && frames >= profileStart + 600)
            {
                StopProfiler();
                profileTaken = true;
            }
            else if (!Prof.Running && !profileTaken && frames >= profileStart)
                StartProfiler();
        }
    }

    LOG_INFO("HOME", "Starting cleanup...");
    // schedule the next program while libnx's environment is still intact
    if (g_exitToTico)
        ChainloadTico();
    StopProfiler();
    LOG_INFO("HOME", "cleanup: auto save");
    AutoSaveState();
    // let the emulator finish its frame and stop
    LOG_INFO("HOME", "cleanup: stopping the emulator");
    ShutdownHandoff();
    g_core.reset();
    LOG_INFO("HOME", "cleanup: emulator stopped");

    OverlayUI::SetSlotOccupiedCallback(nullptr);
    OverlayUI::SetCheatCallbacks(nullptr, nullptr);
    OverlayUI::SetPlayerCallbacks({});
    OverlayUI::SetSlotPreviewCallback(nullptr);
    OverlayUI::SetShaderCallbacks({});
    OverlayUI::SetLibraryCallbacks({});
    OverlayUI::SetLibraryFolderCallbacks({});
    ImGuiOverlay::Shutdown();
    ShutdownAndLeave();
}
