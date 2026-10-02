/// @file TicoMain.cpp
/// @brief Entry point for tico-integrated mupen64plus NRO
/// Sets up SDL/EGL/ImGui and runs the main loop

#include "TicoCore.h"
#include "TicoOverlay.h"
#include "TicoConfig.h"
#include "TicoAudio.h"
#include "TicoTranslationManager.h"

#include <SDL.h>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <strings.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <vector>
#include "TicoUtils.h"
#include "TicoLogger.h"

#ifdef __SWITCH__
#include <switch.h>
#ifndef TICO_VULKAN_OVERLAY
#include "glad.h"
#include <EGL/egl.h>
#endif
#include <curl/curl.h>
#endif

#include "imgui.h"
#ifdef TICO_VULKAN_OVERLAY
#include "TicoVulkan.h"
#else
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"
#endif

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

static SDL_Window *g_window = nullptr;
#ifndef TICO_VULKAN_OVERLAY
#ifndef __SWITCH__
static SDL_GLContext g_glContext = nullptr;
#endif
static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;
#endif

static std::unique_ptr<TicoCore> g_core;
static std::unique_ptr<TicoOverlay> g_overlay;

static bool g_running = true;
static bool g_exitToSystem = false;
static TicoAudio g_audio;
static SDL_AudioDeviceID g_audioDevice = 0;
static SDL_GameController *g_controllers[4] = {nullptr, nullptr, nullptr, nullptr};
static bool g_controllersDirty = true;

#ifdef __SWITCH__
static u8 g_lastOperationMode = 255;

static void ApplySwitchPerformanceProfile()
{
    Result rcNormal = apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220007);
    Result rcBoost = apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, 0x92220008);
    if (R_FAILED(rcNormal) || R_FAILED(rcBoost))
    {
        LOG_WARN("HOME", "Switch performance profile failed (normal=0x%x boost=0x%x)", rcNormal, rcBoost);
    }
    else
    {
        LOG_INFO("HOME", "Applied Switch performance profile");
    }
}

static void PinCurrentThreadToCore(int core, const char *label)
{
    if (core < 0 || core > 2)
        return;

    Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
    if (R_FAILED(rc))
    {
        LOG_WARN("HOME", "Failed to pin %s thread to core %d (rc=0x%x)", label, core, rc);
    }
    else
    {
        LOG_INFO("HOME", "Pinned %s thread to core %d", label, core);
    }
}

static bool UpdateScreenMode()
{
    u8 operationMode = appletGetOperationMode();
    if (operationMode == g_lastOperationMode)
        return false;

    if (operationMode == AppletOperationMode_Handheld)
    {
        nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1280, 720);
        LOG_INFO("DISPLAY", "Mode → Handheld (1280x720)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.0f;
        }
    }
    else
    {
        nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
        LOG_INFO("DISPLAY", "Mode → Docked (1920x1080)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.5f;
        }
    }
    g_lastOperationMode = operationMode;
    return true;
}
#endif

//==============================================================================
// SDL/EGL Initialization
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
    int joystickCount = SDL_NumJoysticks();

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

static void GetDisplayResolution(int &w, int &h)
{
#ifdef __SWITCH__
    u8 opMode = appletGetOperationMode();
    if (opMode == AppletOperationMode_Handheld)
    {
        w = 1280;
        h = 720;
    }
    else
    {
        w = 1920;
        h = 1080;
    }
#else
    if (g_window)
        SDL_GetWindowSize(g_window, &w, &h);
    else
    {
        w = 1280;
        h = 720;
    }
#endif
}

bool InitWindow()
{
    LOG_INFO("HOME", "Starting initialization...");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER |
                 SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK) != 0)
    {
        LOG_ERROR("HOME", "SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    LOG_INFO("HOME", "SDL initialized");

#ifdef __SWITCH__
    g_window = nullptr;
    LOG_INFO("HOME", "Switch: skipping SDL window (using native window)");

    UpdateScreenMode();
    int w, h;
    GetDisplayResolution(w, h);
    LOG_INFO("HOME", "Switch Resolution: %dx%d (logical)", w, h);

#ifdef TICO_VULKAN_OVERLAY
    if (!TicoVulkan::CreateInstance())
    {
        LOG_ERROR("VK", "TicoVulkan::CreateInstance failed");
        return false;
    }
    LOG_INFO("VK", "Vulkan instance/surface initialized");
#else
    // Initialize EGL
    g_eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_eglDisplay == EGL_NO_DISPLAY)
    {
        LOG_ERROR("EGL", "eglGetDisplay failed");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(g_eglDisplay, &major, &minor))
    {
        LOG_ERROR("EGL", "eglInitialize failed");
        return false;
    }
    LOG_INFO("EGL", "EGL %d.%d initialized", major, minor);

    EGLConfig config;
    EGLint numConfigs;
    const EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE};

    if (!eglChooseConfig(g_eglDisplay, configAttribs, &config, 1, &numConfigs))
    {
        LOG_ERROR("EGL", "eglChooseConfig failed");
        return false;
    }

    g_eglSurface = eglCreateWindowSurface(g_eglDisplay, config,
                                          nwindowGetDefault(), NULL);
    if (g_eglSurface == EGL_NO_SURFACE)
    {
        LOG_ERROR("EGL", "eglCreateWindowSurface failed");
        return false;
    }

    eglBindAPI(EGL_OPENGL_API);
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE};

    g_eglContext = eglCreateContext(g_eglDisplay, config, EGL_NO_CONTEXT, contextAttribs);
    if (g_eglContext == EGL_NO_CONTEXT)
    {
        LOG_ERROR("EGL", "eglCreateContext failed");
        return false;
    }

    if (!eglMakeCurrent(g_eglDisplay, g_eglSurface, g_eglSurface, g_eglContext))
    {
        LOG_ERROR("EGL", "eglMakeCurrent failed");
        return false;
    }

    if (!gladLoadGLLoader((GLADloadproc)eglGetProcAddress))
    {
        LOG_ERROR("HOME", "gladLoadGLLoader failed");
        return false;
    }

    eglSwapInterval(g_eglDisplay, 0);
    LOG_INFO("EGL", "VSync disabled (eglSwapInterval=0), using manual frame pacing");

    LOG_INFO("HOME", "OpenGL %s initialized", glGetString(GL_VERSION));
#endif

#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    g_window = SDL_CreateWindow("mupen64plus",
                                SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                TicoConfig::WINDOW_WIDTH, TicoConfig::WINDOW_HEIGHT,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (!g_window)
    {
        LOG_ERROR("HOME", "SDL_CreateWindow failed: %s", SDL_GetError());
        return false;
    }

    g_glContext = SDL_GL_CreateContext(g_window);
    if (!g_glContext)
    {
        LOG_ERROR("HOME", "SDL_GL_CreateContext failed: %s", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(g_window, g_glContext);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress))
    {
        LOG_ERROR("HOME", "gladLoadGLLoader failed");
        return false;
    }

    LOG_INFO("HOME", "OpenGL %s initialized", glGetString(GL_VERSION));
#endif

    if (TicoConfig::USE_SDLQUEUEAUDIO)
    {
        SDL_AudioSpec want, have;
        SDL_zero(want);
        want.freq = TicoAudio::SAMPLE_RATE;
        want.format = AUDIO_S16SYS;
        want.channels = TicoAudio::CHANNELS;
        want.samples = 2048;
        want.callback = NULL;

        g_audioDevice = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (g_audioDevice == 0)
        {
            LOG_ERROR("AUDIO", "SDL_OpenAudioDevice failed: %s", SDL_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_QueueAudio initialized. DeviceID: %d, Freq: %d", g_audioDevice, have.freq);
        }
    }
    else
    {
        if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        {
            LOG_ERROR("AUDIO", "Mix_OpenAudio failed: %s", Mix_GetError());
        }
        else
        {
            LOG_INFO("AUDIO", "SDL_mixer initialized");
        }
    }

    return true;
}

static void AudioSampleCallback(int16_t left, int16_t right)
{
    g_audio.PushSample(left, right);
}

static size_t AudioSampleBatchCallback(const int16_t *data, size_t frames)
{
    return g_audio.PushSamples(data, frames);
}

static void AudioFlushCallback()
{
    g_audio.Flush();
    LOG_INFO("AUDIO", "Audio flushed");
}

bool InitImGui()
{
    LOG_INFO("HOME", "InitImGui starting...");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    LOG_INFO("HOME", "ImGui context created");

#ifdef __SWITCH__
#ifdef TICO_VULKAN_OVERLAY
    LOG_INFO("HOME", "ImGui Vulkan backend will initialize after core device negotiation");
#else
    ImGui_ImplSDL2_InitForOpenGL(g_window, nullptr);
    ImGui_ImplOpenGL3_Init("#version 430 core");
#endif
#else
    ImGui_ImplSDL2_InitForOpenGL(g_window, g_glContext);
    ImGui_ImplOpenGL3_Init("#version 330 core");
#endif
    LOG_INFO("HOME", "ImGui backends initialized");

#ifdef __SWITCH__
    ImFontConfig fontCfg;
    fontCfg.SizePixels = TicoConfig::FONT_SIZE;
    if (io.Fonts->AddFontFromFileTTF(TicoConfig::FONT_PATH, TicoConfig::FONT_SIZE))
    {
        LOG_INFO("HOME", "Loaded ImGui font from %s", TicoConfig::FONT_PATH);
    }
    else if (!io.Fonts->AddFontDefault(&fontCfg))
    {
        LOG_ERROR("HOME", "Failed to load font from romfs and built-in ImGui fallback");
        return false;
    }
    else
    {
        LOG_WARN("HOME", "Failed to load %s, using built-in ImGui font", TicoConfig::FONT_PATH);
    }
#else
    if (!io.Fonts->AddFontFromFileTTF("assets/fonts/font.ttf", TicoConfig::FONT_SIZE))
    {
        LOG_ERROR("HOME", "Failed to load ImGui font from assets/fonts/font.ttf");
        return false;
    }
#endif

    LOG_INFO("HOME", "ImGui initialized");
    return true;
}

void CleanupWindow()
{
    CloseControllers();

#ifdef TICO_VULKAN_OVERLAY
    TicoVulkan::ShutdownOverlayRenderer();
    if (ImGui::GetCurrentContext())
        ImGui::DestroyContext();
    TicoVulkan::Shutdown();
#else
    glFinish();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

#ifdef __SWITCH__
    if (g_eglContext != EGL_NO_CONTEXT)
    {
        eglMakeCurrent(g_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(g_eglDisplay, g_eglContext);
    }
    if (g_eglSurface != EGL_NO_SURFACE)
    {
        eglDestroySurface(g_eglDisplay, g_eglSurface);
    }
    if (g_eglDisplay != EGL_NO_DISPLAY)
    {
        eglTerminate(g_eglDisplay);
    }

    eglReleaseThread();
#else
    if (g_glContext)
    {
        SDL_GL_DeleteContext(g_glContext);
    }
#endif
#endif

    if (g_window)
    {
        SDL_DestroyWindow(g_window);
    }

    SDL_Quit();
}

//==============================================================================
// Main Loop
//==============================================================================

void ProcessEvents()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
#ifndef TICO_VULKAN_OVERLAY
        ImGui_ImplSDL2_ProcessEvent(&event);
#endif

        if (event.type == SDL_QUIT)
        {
            LOG_INFO("HOME", "Received SDL_QUIT event");
            g_running = false;
        }

        if (event.type == SDL_CONTROLLERDEVICEADDED ||
            event.type == SDL_CONTROLLERDEVICEREMOVED ||
            event.type == SDL_JOYDEVICEADDED ||
            event.type == SDL_JOYDEVICEREMOVED)
        {
            g_controllersDirty = true;
        }

#ifdef __SWITCH__
        if (event.type == SDL_KEYDOWN && event.key.keysym.scancode == SDL_SCANCODE_ESCAPE)
        {
            LOG_INFO("HOME", "Received Escape key event, requesting exit");
            g_running = false;
        }
#endif
    }
}

void HandleInput()
{
    SDL_GameController *controllers[4] = {nullptr, nullptr, nullptr, nullptr};
    int numControllers = 0;

    if (g_controllersDirty)
    {
        RefreshControllers();
    }

    for (int i = 0; i < 4; ++i)
    {
        if (g_controllers[i])
            controllers[numControllers++] = g_controllers[i];
    }

    if (g_overlay && numControllers > 0 && g_overlay->HandleInput(controllers[0]))
    {
        if (g_overlay->ShouldExitToSystem())
        {
            LOG_INFO("HOME", "ExitToSystem: terminating process");
            remove("imgui.ini");
            g_exitToSystem = true;
            g_running = false;
        }
        if (g_overlay->ShouldReset())
        {
            g_overlay->ClearReset();
            if (g_core)
            {
                g_core->Reset();
            }
        }
        return;
    }

    if (g_core)
    {
        g_core->ClearInputs();

        for (int p = 0; p < numControllers; p++)
        {
            SDL_GameController *controller = controllers[p];
            if (!controller) continue;

            // N64 mapping (alternate mode):
            // Switch A (SDL B) -> JOYPAD_B -> N64 A
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_B,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B));
            // Switch B (SDL A) -> JOYPAD_Y -> N64 B
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_Y,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A));
            // Switch + -> JOYPAD_START -> N64 Start
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_START,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START));
            // Switch DPad -> N64 DPad
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_UP,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_DOWN,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_LEFT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT));
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_RIGHT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
            // Switch Y (SDL X) -> JOYPAD_L -> N64 C-Left
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_X));
            // Switch X (SDL Y) -> JOYPAD_A -> N64 C-Down
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_A,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_Y));
            // Switch L -> JOYPAD_SELECT -> N64 L Trigger
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_SELECT,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER));
            // Switch R -> JOYPAD_R2 -> N64 R Trigger
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_R2,
                                  SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER));
            // Switch ZL + ZR -> JOYPAD_L2 -> N64 Z Trigger
            g_core->SetInputState(p, RETRO_DEVICE_ID_JOYPAD_L2,
                                  SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000 ||
                                  SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000);

            // Left stick -> N64 Analog Stick
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY));
            // Right stick -> N64 C-Buttons
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX));
            g_core->SetAnalogState(p, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y,
                                   SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY));
        }
    }
}

/// Build and stage the ImGui overlay + OSD draw data for the current frame.
/// Shared by the libretro pump path (Render) and the standalone present path
/// (tico_standalone_present, which runs on the emulation thread). Must be
/// called between TicoVulkan::BeginFrame and TicoVulkan::EndFrame, and only
/// ever from the single thread that presents.
void RenderOverlayAndOSD(int w, int h)
{
    TicoVulkan::BeginOverlayFrame();

    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    io.DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();

    ImVec2 displaySize((float)w, (float)h);
    if (g_overlay)
    {
        float ar = g_core ? g_core->GetAspectRatio() : 4.0f / 3.0f;
        int fw = g_core ? g_core->GetFrameWidth() : 640;
        int fh = g_core ? g_core->GetFrameHeight() : 480;
        g_overlay->Render(displaySize, 0, ar, fw, fh, 0, 0);
    }

    if (g_core && g_core->GetOSDFrames() > 0)
    {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float marginX = 24.0f;
        const float marginY = 16.0f;
        const float padX = 16.0f;
        const float padY = 8.0f;
        const float rounding = 14.0f;

        int frames = g_core->GetOSDFrames();
        float alpha = frames < 30 ? frames / 30.0f : 1.0f;
        std::string msg = g_core->GetOSDMessage();
        ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());
        float pillW = textSize.x + padX * 2;
        float pillH = textSize.y + padY * 2;
        ImU32 bgCol = IM_COL32(0, 0, 0, (int)(alpha * 153));
        fg->AddRectFilled(ImVec2(marginX, marginY), ImVec2(marginX + pillW, marginY + pillH), bgCol, rounding);
        ImU32 textCol = IM_COL32(255, 255, 255, (int)(alpha * 240));
        fg->AddText(ImVec2(marginX + padX, marginY + padY), textCol, msg.c_str());
        g_core->DecrementOSD();
    }

    ImGui::Render();
    TicoVulkan::SetOverlayDrawData(ImGui::GetDrawData());
}

void Render()
{
    static int frameCount = 0;
    frameCount++;

    if (frameCount <= 3)
    {
        LOG_DEBUG("RENDER", "Frame %d: Render starting", frameCount);
    }

#ifdef TICO_VULKAN_OVERLAY
#ifdef __SWITCH__
    UpdateScreenMode();
#endif

#ifdef __SWITCH__
    // Frame-time breakdown, reported every 600 frames: splits the loop into
    // begin (fence wait + swapchain acquire), run (retro_run = CPU emulation incl.
    // synchronous RDP waits) and end (submit + FIFO present). Whichever bucket
    // exceeds its share of 16.7ms is the speed bottleneck.
    static uint64_t s_ftAccBegin = 0, s_ftAccRun = 0, s_ftAccEnd = 0;
    static uint32_t s_ftSamples = 0;
    const uint64_t ftT0 = svcGetSystemTick();
#endif
    if (!TicoVulkan::BeginFrame())
        return;
#ifdef __SWITCH__
    const uint64_t ftT1 = svcGetSystemTick();
#endif
    if (frameCount <= 5)
        LOG_DEBUG("RENDER", "Frame %d: Vulkan BeginFrame succeeded", frameCount);

    int w, h;
    GetDisplayResolution(w, h);
    uint32_t swapW = 0, swapH = 0;
    TicoVulkan::GetSwapExtent(swapW, swapH);
    if (swapW != 0 && swapH != 0)
    {
        w = (int)swapW;
        h = (int)swapH;
    }

    if (g_core)
    {
        bool overlayVisible = g_overlay && g_overlay->IsVisible();
        if (!overlayVisible)
        {
            if (frameCount <= 5)
                LOG_DEBUG("RENDER", "Frame %d: Calling RunFrame", frameCount);
            g_core->RunFrame();
            if (frameCount <= 5)
                LOG_DEBUG("RENDER", "Frame %d: RunFrame returned", frameCount);
        }
        else if (frameCount <= 5)
        {
            LOG_DEBUG("RENDER", "Frame %d: Overlay visible, RunFrame skipped", frameCount);
        }
    }

#ifdef __SWITCH__
    const uint64_t ftT2 = svcGetSystemTick();
#endif

    RenderOverlayAndOSD(w, h);
    TicoVulkan::EndFrame();
#ifdef __SWITCH__
    {
        const uint64_t ftT3 = svcGetSystemTick();
        s_ftAccBegin += ftT1 - ftT0;
        s_ftAccRun += ftT2 - ftT1;
        s_ftAccEnd += ftT3 - ftT2;
        if (++s_ftSamples == 600)
        {
            // 19.2 MHz system tick: us = ticks * 10 / 192
            const uint64_t b = (s_ftAccBegin / 600) * 10 / 192;
            const uint64_t r = (s_ftAccRun / 600) * 10 / 192;
            const uint64_t e = (s_ftAccEnd / 600) * 10 / 192;
            LOG_WARN("CORE", "FRAME stats avg-us over 600: begin=%llu run=%llu end=%llu total=%llu (budget 16667)",
                     (unsigned long long)b, (unsigned long long)r,
                     (unsigned long long)e, (unsigned long long)(b + r + e));
            s_ftAccBegin = s_ftAccRun = s_ftAccEnd = 0;
            s_ftSamples = 0;
        }
    }
#endif
    if (frameCount <= 5)
        LOG_DEBUG("RENDER", "Frame %d: Vulkan EndFrame returned", frameCount);
    return;
#else

    ImGui_ImplOpenGL3_NewFrame();

#ifdef __SWITCH__
    UpdateScreenMode();

    ImGuiIO &io = ImGui::GetIO();
    int logW, logH;
    GetDisplayResolution(logW, logH);
    io.DisplaySize = ImVec2((float)logW, (float)logH);
    io.DeltaTime = 1.0f / 60.0f;
#else
    ImGui_ImplSDL2_NewFrame();
#endif
    ImGui::NewFrame();

    int w, h;
    GetDisplayResolution(w, h);
    ImVec2 displaySize((float)w, (float)h);

    if (g_core)
    {
        bool overlayVisible = g_overlay && g_overlay->IsVisible();

        if (!overlayVisible)
        {
            if (frameCount <= 3)
            {
                LOG_DEBUG("RENDER", "Frame %d: Calling RunFrame", frameCount);
            }
            g_core->RunFrame();
            if (frameCount <= 3)
            {
                LOG_DEBUG("RENDER", "Frame %d: RunFrame returned", frameCount);
            }
        }
    }

    glViewport(0, 0, w, h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (g_overlay)
    {
        unsigned int tex = g_core ? g_core->GetFrameTextureID() : 0;
        float ar = g_core ? g_core->GetAspectRatio() : 4.0f / 3.0f;
        int fw = g_core ? g_core->GetFrameWidth() : 640;
        int fh = g_core ? g_core->GetFrameHeight() : 480;
        int fboW = g_core ? g_core->GetFBOWidth() : 0;
        int fboH = g_core ? g_core->GetFBOHeight() : 0;

        g_overlay->Render(displaySize, tex, ar, fw, fh, fboW, fboH);
    }

    if (g_core && g_core->GetOSDFrames() > 0)
    {
        ImDrawList *fg = ImGui::GetForegroundDrawList();
        const float marginX = 24.0f;
        const float marginY = 16.0f;
        const float padX = 16.0f;
        const float padY = 8.0f;
        const float rounding = 14.0f;

        int frames = g_core->GetOSDFrames();
        float alpha = 1.0f;
        if (frames < 30) alpha = frames / 30.0f;

        std::string msg = g_core->GetOSDMessage();
        ImVec2 textSize = ImGui::CalcTextSize(msg.c_str());

        float pillW = textSize.x + padX * 2;
        float pillH = textSize.y + padY * 2;
        float pillX = marginX;
        float pillY = marginY;

        ImU32 bgCol = IM_COL32(0, 0, 0, (int)(alpha * 153));
        fg->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX + pillW, pillY + pillH), bgCol, rounding);

        ImU32 textCol = IM_COL32(255, 255, 255, (int)(alpha * 240));
        fg->AddText(ImVec2(pillX + padX, pillY + padY), textCol, msg.c_str());

        g_core->DecrementOSD();
    }

    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
#endif
#endif
}

//==============================================================================
// Main
//==============================================================================

#ifdef TICO_STANDALONE
// ============================================================================
// Standalone mode (Phase 1 — STANDALONE_PLAN.md)
//
// The emulator free-runs on a dedicated pthread (libretro.c:
// tico_standalone_start_emu, pinned to core 1 by EmuThreadFunction) and hands
// frames over from the N64 VI path: paraLLEl's parallelUpdateScreen() calls
// tico_standalone_present() on the emu thread right after set_image, which
// posts the frame to the main thread (core 2). The main thread presents it and
// handles applet/input/overlay state; FIFO vsync there paces emulation through
// the one-slot handoff. There is no frame pump, no libco coroutine, and no
// manual pacer.
//
// Phase-1 known limits (see plan): overlay shows over the *running* game (no
// auto-pause), runtime display-mode switches are not handled, RA badge
// uploads are deferred (RA disabled during bisection anyway).
// ============================================================================

extern "C" void tico_standalone_start_emu(void);
extern "C" void tico_standalone_stop_emu(void);

static std::atomic<uint64_t> g_standalonePresented{0};

#ifdef __SWITCH__
extern "C" Handle tico_emu_thread_handle;
extern "C" void *mupen_jit_rx_addr;
extern "C" void _start();

// Sampling profiler for the emu thread (same scheme as WatermelonDS): every 5 ms a
// thread on core 2 pauses the emu thread and records its PC. PCs inside the NRO are
// bucketed by offset (resolve with tools/resolve_profile.py); PCs in the R4300
// dynarec cache and in other JIT code (paraLLEl-RSP) are counted separately.
// Opt-in: create sdmc:/switch/mupen64plus/profile.on (may hold the first frame
// to sample, default 600); 600 presented frames are sampled.
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
        if (R_FAILED(svcSetThreadActivity(tico_emu_thread_handle, ThreadActivity_Paused)))
        {
            std::lock_guard<std::mutex> lock(Prof.DataMutex);
            Prof.Failed++;
            continue;
        }
        Result rc = svcGetThreadContext3(&ctx, tico_emu_thread_handle);
        // the stack is only stable while the thread is paused
        if (R_SUCCEEDED(rc))
            CollectStack(ctx, base, stack);
        svcSetThreadActivity(tico_emu_thread_handle, ThreadActivity_Runnable);
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
    const std::string dir = TicoConfig::ROM_FALLBACK_DIR;
    FILE *f = fopen((dir + "profile.txt").c_str(), "w");
    if (!f)
        return;
    fprintf(f, "total %u r4300_jit %u other_jit %u failed %u\n", Prof.Total, Prof.R4300Jit, Prof.OtherJit, Prof.Failed);
    fprintf(f, "# interval_ns %llu\n", (unsigned long long)kProfileIntervalNs);
    for (u32 i = 0; i < kProfileBuckets; i++)
    {
        if (Prof.Buckets[i])
            fprintf(f, "%x %u\n", i << kProfileBucketShift, Prof.Buckets[i]);
    }
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
    if (!tico_emu_thread_handle)
        return;
    Prof.Buckets = (u32 *)calloc(kProfileBuckets, sizeof(u32));
    if (!Prof.Buckets)
    {
        LOG_WARN("PROFILE", "profiler allocation failed");
        return;
    }
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
    LOG_WARN("PROFILE", "sampling emu thread");
    // Replace an older run's profile immediately, even if this run is killed.
    WriteProfile();
}

void StopProfiler()
{
    if (Prof.Running)
    {
        Prof.Running = false;
        threadWaitForExit(&Prof.Worker);
        threadClose(&Prof.Worker);
        LOG_WARN("PROFILE", "wrote profile.txt (%u samples, r4300_jit %u, other_jit %u)",
                 Prof.Total, Prof.R4300Jit, Prof.OtherJit);
    }
    WriteProfile();
}
} // namespace
#endif

// The emu thread hands each scanned-out frame to the main thread (core 2), which
// does the acquire, overlay and present, so none of it costs emulation time on
// core 1. One slot: the emu thread can post a frame only once the main thread
// took the previous one, and the main thread takes a frame only after it
// submitted the one before. So the core never reuses a frame (sync) index whose
// present has not been submitted, and FIFO vsync still paces emulation.
namespace {
struct PresentSlot
{
    std::mutex Mutex;
    std::condition_variable Cond;
    bool Full = false;
    bool Shutdown = false;
    uint32_t FrameIndex = 0;
    unsigned Width = 0, Height = 0;
};
PresentSlot g_presentSlot;
} // namespace

extern "C" uint64_t tico_rsp_task_ns[3];

static inline uint64_t NowUs()
{
    return armTicksToNs(armGetSystemTick()) / 1000;
}

extern "C" void tico_standalone_present(unsigned width, unsigned height)
{
    static uint64_t s_emuUs = 0, s_postUs = 0, s_frames = 0;
    static uint64_t s_lastExit = 0;
    const uint64_t t0 = NowUs();
    if (s_lastExit)
        s_emuUs += t0 - s_lastExit;

    const uint32_t frameIndex = TicoVulkan::AdvanceCoreFrame();
    {
        std::unique_lock<std::mutex> lock(g_presentSlot.Mutex);
        g_presentSlot.Cond.wait(lock, [] { return !g_presentSlot.Full || g_presentSlot.Shutdown; });
        if (!g_presentSlot.Shutdown)
        {
            g_presentSlot.Full = true;
            g_presentSlot.FrameIndex = frameIndex;
            g_presentSlot.Width = width;
            g_presentSlot.Height = height;
        }
    }
    g_presentSlot.Cond.notify_all();

    const uint64_t t1 = NowUs();
    s_postUs += t1 - t0;
    s_lastExit = t1;

    if ((++s_frames % 600) == 0)
    {
        // emu = emulation between VIs (rsp_* = RSP share of it by task type);
        // post = waiting for the main thread to take the frame
        static uint64_t s_lastRsp[3] = {};
        uint64_t rsp[3];
        for (int i = 0; i < 3; i++)
        {
            rsp[i] = (tico_rsp_task_ns[i] - s_lastRsp[i]) / 1000 / 600;
            s_lastRsp[i] = tico_rsp_task_ns[i];
        }
        LOG_WARN("CORE", "STANDALONE emu thread avg-us: emu=%llu (rsp_gfx=%llu rsp_audio=%llu rsp_other=%llu) post=%llu (VI budget 16683)",
                 (unsigned long long)(s_emuUs / 600),
                 (unsigned long long)rsp[0], (unsigned long long)rsp[1], (unsigned long long)rsp[2],
                 (unsigned long long)(s_postUs / 600));
        s_emuUs = s_postUs = 0;
    }
}

static void StopPresentSlot()
{
    {
        std::lock_guard<std::mutex> lock(g_presentSlot.Mutex);
        g_presentSlot.Shutdown = true;
    }
    g_presentSlot.Cond.notify_all();
}

// Main thread: waits up to 10 ms for a frame (the UI tick when nothing is posted).
static void PresentPostedFrame()
{
    static uint64_t s_beginUs = 0, s_overlayUs = 0, s_endUs = 0;

    uint32_t frameIndex = 0;
    unsigned width = 0, height = 0;
    {
        std::unique_lock<std::mutex> lock(g_presentSlot.Mutex);
        if (!g_presentSlot.Cond.wait_for(lock, std::chrono::milliseconds(10),
                                         [] { return g_presentSlot.Full; }))
            return;
        frameIndex = g_presentSlot.FrameIndex;
        width = g_presentSlot.Width;
        height = g_presentSlot.Height;
    }

    const uint64_t t0 = NowUs();
    if (width && height)
        TicoVulkan::SetSourceExtent(width, height);

    const bool begun = TicoVulkan::BeginFrameAt(frameIndex);
    const uint64_t t1 = NowUs();
    uint64_t t2 = t1;
    if (begun)
    {
        int w = 0, h = 0;
        GetDisplayResolution(w, h);
        uint32_t swapW = 0, swapH = 0;
        TicoVulkan::GetSwapExtent(swapW, swapH);
        if (swapW != 0 && swapH != 0)
        {
            w = (int)swapW;
            h = (int)swapH;
        }

        RenderOverlayAndOSD(w, h);
        t2 = NowUs();
        TicoVulkan::EndFrame(); // FIFO present
    }
    const uint64_t t3 = NowUs();

    // Taken only now: the emu thread may post the next frame once this one is submitted.
    {
        std::lock_guard<std::mutex> lock(g_presentSlot.Mutex);
        g_presentSlot.Full = false;
    }
    g_presentSlot.Cond.notify_all();

    s_beginUs += t1 - t0;
    s_overlayUs += t2 - t1;
    s_endUs += t3 - t2;

    const uint64_t presented = ++g_standalonePresented;
    if ((presented % 600) == 0)
    {
        LOG_WARN("CORE", "STANDALONE present #%llu avg-us (core 2): begin=%llu overlay=%llu end=%llu",
                 (unsigned long long)presented,
                 (unsigned long long)(s_beginUs / 600), (unsigned long long)(s_overlayUs / 600),
                 (unsigned long long)(s_endUs / 600));
        s_beginUs = s_overlayUs = s_endUs = 0;
    }
}

static void TicoStandaloneRun()
{
    LOG_INFO("HOME", "Standalone: starting emu thread; main thread presents and runs the UI");
    tico_standalone_start_emu();

#ifdef __SWITCH__
    // Opt-in sampling covers 600 frames after warm-up (loading and JIT compile).
    // profile.on may hold the first frame to sample (default 600).
    const std::string profileFlag = std::string(TicoConfig::ROM_FALLBACK_DIR) + "profile.on";
    uint64_t profileStart = 600;
    bool profiling = false;
    if (FILE *flag = fopen(profileFlag.c_str(), "r"))
    {
        profiling = true;
        unsigned long long start = 0;
        if (fscanf(flag, "%llu", &start) == 1)
            profileStart = start;
        fclose(flag);
    }
    bool profileTaken = false;
    if (profiling)
        LOG_INFO("PROFILE", "profiler armed for frames %llu-%llu",
                 (unsigned long long)profileStart, (unsigned long long)(profileStart + 600));
    else
        LOG_INFO("PROFILE", "profiler off (create %s to enable)", profileFlag.c_str());
#endif

    Uint32 lastTime = SDL_GetTicks();
    while (g_running)
    {
#ifdef __SWITCH__
        if (!appletMainLoop())
        {
            LOG_INFO("HOME", "appletMainLoop returned false, exiting");
            g_running = false;
            break;
        }
#endif
        float deltaTime = (SDL_GetTicks() - lastTime) / 1000.0f;
        lastTime = SDL_GetTicks();

        ProcessEvents();
        HandleInput();
        if (g_overlay)
            g_overlay->Update(deltaTime);

#ifdef __SWITCH__
        if (profiling)
        {
            const uint64_t frames = g_standalonePresented.load(std::memory_order_relaxed);
            if (Prof.Running && frames >= profileStart + 600)
            {
                StopProfiler();
                profileTaken = true;
            }
            else if (!Prof.Running && !profileTaken && frames >= profileStart)
                StartProfiler();
        }
#endif

        // Same audio diagnostics as the pump build (~every 10s).
        {
            static uint64_t lastHeartbeat = NowUs();
            if (NowUs() - lastHeartbeat >= 10000000)
            {
                lastHeartbeat = NowUs();
                LOG_WARN("CORE", "AUDIO heartbeat: consumer_calls=%llu buffered=%zu stalled=%d stalls=%llu underruns=%u primes=%llu",
                         (unsigned long long)g_audio.GetConsumerCalls(),
                         g_audio.GetBufferedSamples(),
                         g_audio.IsSinkStalled() ? 1 : 0,
                         (unsigned long long)g_audio.GetStallCount(),
                         g_audio.GetUnderrunCount(),
                         (unsigned long long)g_audio.GetPrimeCount());
            }
        }

        PresentPostedFrame();
    }

#ifdef __SWITCH__
    StopProfiler();
#endif
    // Release an emu thread waiting to post a frame before stopping it.
    StopPresentSlot();
    LOG_INFO("HOME", "Standalone: stopping emu thread");
    tico_standalone_stop_emu();
}
#endif // TICO_STANDALONE

static bool HasN64Extension(const std::string &name)
{
    size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const char *ext = name.c_str() + dot;
    return strcasecmp(ext, ".z64") == 0 || strcasecmp(ext, ".n64") == 0 || strcasecmp(ext, ".v64") == 0;
}

// Picks the alphabetically first N64 ROM in ROM_FALLBACK_DIR so the choice is stable.
static std::string FindFallbackRom()
{
    const std::string baseDir = TicoConfig::ROM_FALLBACK_DIR;
    mkdir(baseDir.c_str(), 0777);

    DIR *dir = opendir(baseDir.c_str());
    if (!dir)
        return {};

    std::string found;
    while (dirent *entry = readdir(dir))
    {
        std::string name = entry->d_name;
        if (!HasN64Extension(name))
            continue;
        if (found.empty() || name < found)
            found = name;
    }
    closedir(dir);

    return found.empty() ? found : baseDir + found;
}

int main(int argc, char *argv[])
{
    Logger::Instance().StartNewLogFile();

    g_running = true;
    g_controllersDirty = true;

#ifdef __SWITCH__
    LOG_INFO("HOME", "Calling appletLockExit...");
    appletLockExit();
    LOG_INFO("HOME", "Calling romfsInit...");
    Result romfsRc = romfsInit();
    if (R_FAILED(romfsRc))
    {
        LOG_WARN("HOME", "romfsInit failed: 0x%x", romfsRc);
    }
    else
    {
        LOG_INFO("HOME", "romfsInit succeeded");
    }

    if (R_SUCCEEDED(socketInitializeDefault()))
    {
        LOG_INFO("HOME", "socketInitializeDefault succeeded");
        curl_global_init(CURL_GLOBAL_DEFAULT);
    }
    else
    {
        LOG_ERROR("HOME", "socketInitializeDefault failed");
    }

    LOG_INFO("HOME", "Configuring native window...");
    g_lastOperationMode = 255;
    UpdateScreenMode();
    LOG_INFO("HOME", "Switch pre-init complete (romfs, nwindow)");
#endif

    LOG_INFO("HOME", "mupen64plus starting...");

    TicoTranslationManager::Instance().Init();

    LOG_INFO("HOME", "Calling InitWindow...");
    if (!InitWindow())
    {
        LOG_ERROR("HOME", "Failed to initialize window");
        Logger::Instance().CloseLogFile();
        return 1;
    }
    LOG_INFO("HOME", "InitWindow succeeded");

#ifdef __SWITCH__
    ApplySwitchPerformanceProfile();
    PinCurrentThreadToCore(2, "main/render");
#endif

#ifndef TICO_VULKAN_OVERLAY
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
#endif
#endif

    LOG_INFO("HOME", "Calling InitImGui...");
    if (!InitImGui())
    {
        LOG_ERROR("HOME", "Failed to initialize ImGui");
        CleanupWindow();
        Logger::Instance().CloseLogFile();
        return 1;
    }
    LOG_INFO("HOME", "InitImGui succeeded");
#ifdef __SWITCH__
    g_lastOperationMode = 255;
#endif

    LOG_INFO("HOME", "Creating core...");
    g_core = std::make_unique<TicoCore>();

#ifndef TICO_VULKAN_OVERLAY
    LOG_INFO("HOME", "Creating overlay...");
    g_overlay = std::make_unique<TicoOverlay>();
    g_overlay->SetCore(g_core.get());
#endif

    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);

    if (!g_audio.Init(g_audioDevice))
    {
        LOG_WARN("HOME", "TicoAudio init failed");
    }

    LOG_INFO("HOME", "Core and overlay created");

    std::string romPath = TicoConfig::TEST_ROM;
    bool romArgFound = false;

    if (argc > 1 && argv[1] && argv[1][0])
    {
        romPath = argv[1];
        romArgFound = true;
        LOG_INFO("HOME", "ROM path provided via argv: %s", romPath.c_str());
    }

    if (!romArgFound)
    {
        std::string fallbackRom = FindFallbackRom();
        if (!fallbackRom.empty())
        {
            romPath = fallbackRom;
            LOG_INFO("HOME", "No ROM argument provided. Using first ROM in %s: %s",
                     TicoConfig::ROM_FALLBACK_DIR, romPath.c_str());
        }
        else
        {
            LOG_INFO("HOME", "No ROM argument and no ROM in %s. Using default: %s",
                     TicoConfig::ROM_FALLBACK_DIR, romPath.c_str());
        }
    }

    {
        size_t lastSlash = romPath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? romPath.substr(lastSlash + 1) : romPath;

        std::string cleanTitle = TicoUtils::GetCleanTitle(filename);
        if (cleanTitle.empty())
            cleanTitle = filename;

        if (g_overlay)
            g_overlay->SetGameTitle(cleanTitle);
    }

    LOG_INFO("HOME", "Loading ROM: %s", romPath.c_str());
    if (!g_core->LoadGame(romPath))
    {
        LOG_ERROR("HOME", "Failed to load ROM: %s", romPath.c_str());
    }
    else
    {
        g_audio.SetCoreSampleRate(g_core->GetSampleRate());
        LOG_INFO("AUDIO", "Configured audio pipeline for %.0f Hz core output", g_core->GetSampleRate());
    }

#ifdef TICO_VULKAN_OVERLAY
    if (!TicoVulkan::InitOverlayRenderer())
    {
        LOG_WARN("OVERLAY", "Vulkan overlay renderer unavailable; continuing without overlay");
    }
    else
    {
        LOG_INFO("HOME", "Creating overlay...");
        g_overlay = std::make_unique<TicoOverlay>();
        g_overlay->SetCore(g_core.get());

        size_t lastSlash = romPath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? romPath.substr(lastSlash + 1) : romPath;
        std::string cleanTitle = TicoUtils::GetCleanTitle(filename);
        if (cleanTitle.empty())
            cleanTitle = filename;
        g_overlay->SetGameTitle(cleanTitle);
    }
#endif

#ifdef TICO_STANDALONE
    // Standalone: the emulator owns the frame path (present-on-VI, FIFO-paced).
    // This call blocks until exit; the pump loop below is compiled out.
    TicoStandaloneRun();
#else
    Uint32 lastTime = SDL_GetTicks();

#ifdef __SWITCH__
    // Manual frame pacing: 19.2 MHz system tick, target ~16.67ms per frame (60fps)
    static constexpr uint64_t TICKS_PER_SECOND = 19200000ULL;
    static constexpr uint64_t FRAME_TICKS = TICKS_PER_SECOND / 60; // ~320000 ticks
    static constexpr int64_t FRAME_NS = 16666667LL; // 16.67ms in nanoseconds
    uint64_t frameStart = svcGetSystemTick();
#endif

    while (g_running)
    {
#ifdef __SWITCH__
        frameStart = svcGetSystemTick();

        if (!appletMainLoop())
        {
            LOG_INFO("HOME", "appletMainLoop returned false, exiting main loop");
            g_running = false;
            break;
        }
#endif

        float deltaTime = (SDL_GetTicks() - lastTime) / 1000.0f;
        lastTime = SDL_GetTicks();

        if (g_overlay)
        {
            g_overlay->Update(deltaTime);
        }

        ProcessEvents();
        HandleInput();
        Render();

        // Audio heartbeat on a visible log category (AUDIO is disabled in release).
        // consumer_calls flat at 0 => SDL never invokes the mixer callback (setup issue);
        // rising but buffer pinned full => callback thread is starved (core scheduling).
        {
            static uint32_t audioHeartbeatFrames = 0;
            if ((++audioHeartbeatFrames % 600) == 0)
            {
                LOG_WARN("CORE", "AUDIO heartbeat: consumer_calls=%llu buffered=%zu stalled=%d stalls=%llu underruns=%u primes=%llu",
                         (unsigned long long)g_audio.GetConsumerCalls(),
                         g_audio.GetBufferedSamples(),
                         g_audio.IsSinkStalled() ? 1 : 0,
                         (unsigned long long)g_audio.GetStallCount(),
                         g_audio.GetUnderrunCount(),
                         (unsigned long long)g_audio.GetPrimeCount());
            }
        }

#ifdef __SWITCH__
        // Manual frame pacing: wait for remainder of frame time.
        // CRITICAL: Without this, when overlay is visible and RunFrame() is
        // skipped, there is NO frame limiter (VSync is off), causing the loop
        // to spin at max GPU speed and freeze/lock the entire console.
        {
            uint64_t frameEnd = svcGetSystemTick();
            uint64_t elapsed = frameEnd - frameStart;
            if (elapsed < FRAME_TICKS)
            {
                int64_t waitNs = (int64_t)(FRAME_TICKS - elapsed) * 1000000000LL / (int64_t)TICKS_PER_SECOND;
                if (waitNs > 0)
                    svcSleepThread(waitNs);
            }
        }
#endif
    }
#endif // !TICO_STANDALONE

    LOG_INFO("HOME", "Starting cleanup...");
    g_overlay.reset();
    g_core.reset();

    // Close JIT buffer if active
#ifdef __SWITCH__
    {
        extern bool mupen_jit_active;
        extern Jit mupen_jit;
        if (mupen_jit_active)
        {
            jitClose(&mupen_jit);
            mupen_jit_active = false;
        }
    }
#endif

    g_audio.Shutdown();
    if (!TicoConfig::USE_SDLQUEUEAUDIO)
    {
        Mix_CloseAudio();
    }

    CleanupWindow();

#ifdef __SWITCH__
    curl_global_cleanup();
    socketExit();
    romfsExit();
    appletUnlockExit();
#endif

    LOG_INFO("HOME", "Clean exit");
    Logger::Instance().CloseLogFile();

    // For exit-to-system: exit(0) triggers libnx's __libnx_exit() which calls
    // __appExit() (tears down fsdev, fs, time, hid, applet, sm) and then
    // __nx_exit(0, envGetExitFuncPtr()).
    // Normally, Homebrew apps return to their loader (Sphaira/hbmenu) rather than exiting to OS.
    // By setting __nx_applet_exit_mode = 1, we bypass the loader and tell Switch OS to terminate the applet.
#ifdef __SWITCH__
    if (g_exitToSystem)
    {
        LOG_INFO("HOME", "g_exitToSystem is true, forcing applet termination via __nx_applet_exit_mode");
        __nx_applet_exit_mode = 1;
    }
#endif
    exit(0);
}
