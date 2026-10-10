/// @file TicoCore.h
/// @brief mupen64plus for the tico frontend: the game, its saves, states,
/// cheats and RetroAchievements, on top of tico_m64p (no libretro).
///
/// The emulator runs on its own thread once Start() is called. Frames reach
/// the frontend through the hooks it sets (SetHooks), on that thread. Save
/// states need the emulation thread to reach a safe point, so the frontend
/// must let it run (not hold its frames) while SaveState/LoadState wait.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>

#include "imgui.h"

#include <switch.h>
#include <SDL_mixer.h>

struct rc_client_t;

struct TicoMemoryMap {
    uint32_t start;
    uint32_t length;
    uint8_t* ptr;
};

/// @brief Alert position for RA notifications
enum class RAAlertPosition {
    TopLeft = 0,
    TopRight,
    BottomLeft,
    BottomRight
};

/// @brief RA notification for the overlay
struct RANotification {
    std::string title;
    std::string description;
    std::string badge_name;     // badge identifier or "ra_icon" for session start
    ImTextureID textureId = ImTextureID_Invalid; // badge texture
    float timer = 0.0f;
    float duration = 4.0f; // total display time
    float slideIn = 0.4f;  // slide-in duration
    float slideOut = 0.4f; // slide-out duration
};

/// What the frontend does with the emulator's frames, on the emulation
/// thread. Exactly one of the present hooks is used, by the renderer.
struct TicoCoreHooks {
    std::function<void()> emuThreadBegin;
    std::function<void()> emuThreadEnd;
    std::function<void(unsigned width, unsigned height)> presentVulkan;
    std::function<void(unsigned width, unsigned height)> presentGL;
    std::function<unsigned(unsigned width, unsigned height)> glFramebuffer;
    std::function<void*(const char *name)> glProcAddress;
    /// paraLLEl-RDP: create the Vulkan device and hand back its interface
    std::function<bool()> createVulkanDevice;
};

class TicoCore
{
public:
    TicoCore();
    ~TicoCore();

    void SetHooks(TicoCoreHooks hooks) { m_hooks = std::move(hooks); }

    /// GLideN64 (OpenGL or Zink) or paraLLEl-RDP (Vulkan), before LoadGame.
    void SetUseGLideN64(bool gliden64) { m_gliden64 = gliden64; }
    bool UsesGLideN64() const { return m_gliden64; }

    /// @brief Load a game ROM (.z64/.n64/.v64, or one inside a .zip/.7z/.rar).
    /// The emulator does not run until Start().
    bool LoadGame(const std::string &path);
    bool Start();

    /// @brief Stop the emulator and write the game's saves.
    void UnloadGame();

    void Reset();

    bool IsGameLoaded() const { return m_gameLoaded; }
    bool IsRunning() const;

    /// @brief OSD notification accessors
    const std::string& GetOSDMessage() const { return m_osdMessage; }
    int GetOSDFrames() const { return m_osdFrames; }
    void DecrementOSD() { if (m_osdFrames > 0) m_osdFrames--; }
    void ShowOSD(const std::string &msg, int frames) { m_osdMessage = msg; m_osdFrames = frames; }

    /// @brief Video/Audio info
    float GetAspectRatio() const { return m_aspectRatio; }
    int GetFrameWidth() const { return m_frameWidth; }
    int GetFrameHeight() const { return m_frameHeight; }
    void SetFrameSize(int width, int height) { m_frameWidth = width; m_frameHeight = height; }
    double GetFPS() const { return m_fps; }

    /// @brief Get current game path
    std::string GetGamePath() const { return m_gamePath; }

    /// @brief An N64 controller, already mapped (TICO_N64_* buttons, stick
    /// in -80..80). From the main thread.
    void SetPad(unsigned port, bool present, uint32_t buttons, int8_t stickX, int8_t stickY);

    /// @brief Cheats for the game, from sdmc:/tico/cheats/n64/<game>.cht
    /// (RetroArch's format, as in libretro's cheat database) or .cheats
    /// ("# Name" then its codes): GameShark "XXXXXXXX YYYY" codes. Every cheat
    /// starts off; toggles last the session. Toggle with the emulation thread
    /// held (the quick menu is open).
    struct Cheat {
        std::string name;
        std::vector<std::string> codes;
        bool enabled = false;
    };
    const std::vector<Cheat> &GetCheats() const { return m_cheats; }
    void ToggleCheat(size_t index);

    /// @brief Save states
    /// The state goes to `path`, rc_client's achievement progress beside it
    /// (`path` + ".ra"). Loading is refused while hardcore is active. Both
    /// block until the emulation thread has taken the state.
    bool SaveState(const std::string &path);
    bool LoadState(const std::string &path);

    /// @brief Main thread: write the cartridge and pak saves the game changed
    /// since they were last written, so a crash or a power-off loses nothing
    /// the game saved. The emulation thread notices the change (OnFrame); this
    /// is cheap when there is none, so call it every loop. Unloading writes
    /// whatever is left.
    void WriteChangedSaves();

    /// True while rc_client runs the session in hardcore mode. Loading states
    /// (and rewind, cheats, slow motion) must stay unavailable then.
    bool IsHardcoreActive() const;

    /// Hardcore rate-limits pausing so it can't be used to slow the game
    /// down. False while a pause isn't allowed yet; `secondsRemaining` then
    /// says how long until it is. Always true outside hardcore.
    bool CanPause(int &secondsRemaining);

    /// Keeps the RetroAchievements session alive while emulation is paused
    /// (the quick menu is open): pings, server callbacks, badge uploads.
    /// Main thread, with the emulation thread held.
    void Idle();

    /// Once per emulated frame, on the emulation thread: changed saves,
    /// achievements and the RetroAchievements server's answers.
    void OnFrame();

    /// @brief Core options. LoadConfig reads mupen64plus.jsonc (once);
    /// SetOption changes an option the core reads (mupen64plus-*).
    void EnsureConfigLoaded() { LoadConfig(); }
    void SetOption(const std::string &key, const std::string &value);
    /// Re-read the options that may change while the game runs.
    void ApplyOptions();

    /// @brief Audio from the emulation thread, at the game's own rate.
    typedef void (*AudioRateCallback_t)(double rate);
    typedef size_t (*AudioSampleBatchCallback_t)(const int16_t *data, size_t frames);
    typedef void (*AudioFlushCallback_t)();
    void SetAudioCallbacks(AudioRateCallback_t rateCb, AudioSampleBatchCallback_t batchCb,
                           AudioFlushCallback_t flushCb = nullptr)
    {
        m_audioRateCallback = rateCb;
        m_audioSampleBatchCallback = batchCb;
        m_audioFlushCallback = flushCb;
    }

private:
    bool Init();
    void LoadSaveData();
    void WriteSaveComponents(const uint8_t *saves, size_t total);
    void CheckSavesChanged();

    /// The core's save block as the files hold it (main thread), as the
    /// emulation thread last saw it, and a changed copy it handed over.
    std::vector<uint8_t> m_savesWritten;
    std::vector<uint8_t> m_savesSeen;
    std::vector<uint8_t> m_savesPending;
    bool m_savesChanged = false;
    std::mutex m_savesMutex;
    unsigned m_saveCheckFrames = 0;
    void LoadCheats();
    void ApplyCheats();
    void UpdateMemoryMaps();

    std::vector<Cheat> m_cheats;

    /// The loaded ROM, unpacked; kept for RetroAchievements hashing.
    std::vector<uint8_t> m_romData;
    static bool IsArchivePath(const std::string &path);
    static bool ReadRomFile(const std::string &path, std::vector<uint8_t> &out);
    static bool ReadRomFromArchive(const std::string &path, std::vector<uint8_t> &out);

    /// @name tico_m64p callbacks
    static void LogCallback(int level, const char *msg);
    static const char *OptionCallback(const char *key);
    static void AudioRateCallback(unsigned rate);
    static void AudioSamplesCallback(const int16_t *frames, size_t count);
    static void RumbleCallback(unsigned port, bool on);
    static void PresentVulkanCallback(unsigned width, unsigned height);
    static void PresentGLCallback(unsigned width, unsigned height);
    static unsigned GLFramebufferCallback(unsigned width, unsigned height);
    static void *GLProcAddressCallback(const char *name);
    static void EmuThreadBeginCallback();
    static void EmuThreadEndCallback();

    TicoCoreHooks m_hooks;
    bool m_gliden64 = false;
    bool m_initialized = false;
    bool m_gameLoaded = false;
    bool m_started = false;

    int m_frameWidth = 640;
    int m_frameHeight = 480;
    float m_aspectRatio = 4.0f / 3.0f;
    double m_fps = 60.0;

    AudioRateCallback_t m_audioRateCallback = nullptr;
    AudioSampleBatchCallback_t m_audioSampleBatchCallback = nullptr;
    AudioFlushCallback_t m_audioFlushCallback = nullptr;

    std::string m_systemDir;
    std::string m_saveDir;
    std::string m_gamePath;

    void LoadConfig();
    std::map<std::string, std::string> m_configOptions;
    std::mutex m_configMutex;
    bool m_configLoaded = false;

    std::string m_osdMessage = "";
    int m_osdFrames = 0;

    // RetroAchievements Client
    rc_client_t* m_rcClient = nullptr;
    bool m_raEnabled = false;
    std::string m_raUsername = "";
    std::string m_raToken = "";
    std::string m_raPassword = "";
    bool m_raHardcore = false;
    // rc_client and its callbacks: the emulation thread per frame, the main
    // thread while paused
    std::mutex m_raMutex;
    void LoadRAConfig();
    void SaveRAToken(const std::string& token);
    static void RAIdentifyGame(rc_client_t* c, TicoCore* core);
    void RunRACallbacks();

    Mix_Chunk* m_trophySound = nullptr;
    static void RALoginWithPassword(rc_client_t* c, TicoCore* core);

public:
    // RA notifications for the overlay (main thread), and those queued from
    // other threads until it collects them
    std::vector<RANotification> m_raNotifications;
    std::vector<RANotification> m_raPendingNotifications;
    std::mutex m_raNotificationMutex;
    void CollectRANotifications();
    RAAlertPosition m_raAlertPosition = RAAlertPosition::TopRight;
    void PushRANotification(const std::string& title, const std::string& desc,
                           const std::string& badge = "");

    // RA badge cache (badge_name -> texture), main thread
    std::map<std::string, ImTextureID> m_raBadgeCache;
    ImTextureID m_raIconTexture = ImTextureID_Invalid; // ra.svg icon
    ImTextureID GetRABadgeTexture(const std::string& badge_name);

public:
    // RA Worker Thread (persistent, proper libnx lifecycle)
    struct RAJob {
        std::string url;
        std::string post_data;
        void* callback;       // rc_client_server_callback_t (cast in .cpp)
        void* callback_data;
    };
    std::mutex m_raJobMutex;
    std::condition_variable m_raJobCond;
    std::deque<RAJob> m_raJobQueue;
    bool m_raWorkerRunning = false;

    std::mutex m_raCallbackMutex;
    std::vector<std::function<void()>> m_raPendingCallbacks;

    Thread m_raThread;
    bool m_raThreadCreated = false;
    void StartRAWorker();
    void StopRAWorker();
    static void RAWorkerEntry(void* arg);

    std::vector<TicoMemoryMap> m_memoryMaps;
};
