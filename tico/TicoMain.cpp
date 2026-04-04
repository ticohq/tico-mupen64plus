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
#include "TicoUtils.h"
#include "TicoLogger.h"

#ifdef __SWITCH__
#include <switch.h>
#include "glad.h"
#include <EGL/egl.h>
#include <curl/curl.h>
#endif

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

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
#ifndef __SWITCH__
static SDL_GLContext g_glContext = nullptr;
#endif
static EGLDisplay g_eglDisplay = EGL_NO_DISPLAY;
static EGLContext g_eglContext = EGL_NO_CONTEXT;
static EGLSurface g_eglSurface = EGL_NO_SURFACE;

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
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
        LOG_INFO("DISPLAY", "Mode → Handheld (1280×720 crop)");
        if (ImGui::GetCurrentContext()) {
            ImGui::GetIO().FontGlobalScale = 1.0f;
        }
    }
    else
    {
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
        LOG_INFO("DISPLAY", "Mode → Docked (1920×1080)");
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
    ImGui_ImplSDL2_InitForOpenGL(g_window, nullptr);
    ImGui_ImplOpenGL3_Init("#version 430 core");
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
        ImGui_ImplSDL2_ProcessEvent(&event);

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

void Render()
{
    static int frameCount = 0;
    frameCount++;

    if (frameCount <= 3)
    {
        LOG_DEBUG("RENDER", "Frame %d: Render starting", frameCount);
    }

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
}

//==============================================================================
// Main
//==============================================================================

int main(int argc, char *argv[])
{
    Logger::Instance().ResetLogFile();

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

    LOG_INFO("HOME", "Calling nwindowSetDimensions...");
    nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
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

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
#ifdef __SWITCH__
    eglSwapBuffers(g_eglDisplay, g_eglSurface);
#else
    SDL_GL_SwapWindow(g_window);
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

    LOG_INFO("HOME", "Creating overlay...");
    g_overlay = std::make_unique<TicoOverlay>();
    g_overlay->SetCore(g_core.get());

    g_core->SetAudioCallbacks(AudioSampleCallback, AudioSampleBatchCallback, AudioFlushCallback);

    if (!g_audio.Init(g_audioDevice))
    {
        LOG_WARN("HOME", "TicoAudio init failed");
    }

    LOG_INFO("HOME", "Core and overlay created");

    std::string romPath = TicoConfig::TEST_ROM;
    bool romArgFound = false;

    if (argc > 1 && argv[1])
    {
        romPath = argv[1];
        romArgFound = true;
        LOG_INFO("HOME", "ROM path provided via argv: %s", romPath.c_str());
    }

    if (!romArgFound)
    {
        LOG_INFO("HOME", "No ROM argument provided. Using default: %s", romPath.c_str());
    }

    {
        size_t lastSlash = romPath.find_last_of("/\\");
        std::string filename = (lastSlash != std::string::npos) ? romPath.substr(lastSlash + 1) : romPath;

        std::string cleanTitle = TicoUtils::GetCleanTitle(filename);
        if (cleanTitle.empty())
            cleanTitle = filename;

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
