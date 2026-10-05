/// @file TicoCore.cpp
/// @brief mupen64plus for the tico frontend. See TicoCore.h.

#include "TicoCore.h"
#include "TicoConfig.h"
#include "TicoLogger.h"
#include "TicoRenderer.h"
#include "TicoSafeFile.h"
#include "TicoUtils.h"
#include "m64p/tico_m64p.h"
#include "../custom/mupen64plus-next_common.h"

#include <archive.h>
#include <archive_entry.h>
#include <curl/curl.h>
#include <json.hpp>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>
#include <fstream>

#include "rc_client.h"
#include "deps/stb/stb_image.h"

extern "C"
{
#include "api/m64p_types.h"
#ifdef BIT
#undef BIT
#endif
#include "main/rom.h"
}

#define tico_debug_log(...) LOG_CORE(__VA_ARGS__)

// Earlier versions kept in backups/ beside each file: the last few sessions'
// saves, and the state each slot held before it was saved over.
static constexpr int kSaveBackups = 3;
static constexpr int kStateBackups = 1;

/// @brief Switch vibration handles and state
static HidVibrationDeviceHandle s_vibrationHandles[5][2] = {};
static HidVibrationValue s_currentVibration[5][2] = {};
static bool s_vibrationInitialized = false;

// Static instance for callbacks
static TicoCore *s_instance = nullptr;
static const char *RAUserAgent();

//==============================================================================
// Content paths
//==============================================================================

namespace {
std::string ContentRoot(const char *key, const char *defaultRoot)
{
    static nlohmann::json config = [] {
        std::ifstream f("sdmc:/tico/config/cores/mupen64plus.jsonc");
        nlohmann::json j = f.good() ? nlohmann::json::parse(f, nullptr, false, true)
                                    : nlohmann::json::object();
        return j.is_object() ? j : nlohmann::json::object();
    }();

    std::string root = defaultRoot;
    auto it = config.find(key);
    if (it != config.end() && it->is_string() && !it->get<std::string>().empty())
        root = it->get<std::string>();
    if (root.back() != '/')
        root += '/';
    return root;
}

std::string FilenameStem(const std::string &path)
{
    std::string name = path;
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name = name.substr(0, dot);
    return name;
}
} // namespace

namespace TicoConfig {
std::string SystemPath() { return ContentRoot("tico_system_path", "sdmc:/tico/system/") + "n64/"; }
std::string SavesPath() { return ContentRoot("tico_saves_path", "sdmc:/tico/saves/") + CURRENT_SLUG + "/"; }
std::string StatesPath() { return ContentRoot("tico_states_path", "sdmc:/tico/states/") + CURRENT_SLUG + "/"; }

void MakeDirs(const std::string &path)
{
    // A custom root may not exist yet, so create every missing level.
    for (size_t at = path.find('/', path.find(":/") != std::string::npos ? path.find(":/") + 2 : 1);
         at != std::string::npos; at = path.find('/', at + 1))
        mkdir(path.substr(0, at).c_str(), 0777);
}
} // namespace TicoConfig

//==============================================================================
// Cartridge and controller pak saves
//==============================================================================

namespace {
struct SaveComponent
{
    const char *label;
    const char *extension;
    size_t offset;
    size_t size;
};

// The kinds of save this game uses, from the core's ROM database: one
// cartridge save and, when the game supports it, the controller paks.
std::vector<SaveComponent> GameSaveComponents()
{
    size_t eeprom, eepromSize, mempak, mempakSize, sram, sramSize, flash, flashSize;
    tico_m64p_save_layout(&eeprom, &eepromSize, &mempak, &mempakSize, &sram, &sramSize, &flash, &flashSize);

    std::vector<SaveComponent> components;
    switch (ROM_SETTINGS.savetype)
    {
    case SAVETYPE_EEPROM_4K:
        components.push_back({"EEPROM", ".eep", eeprom, 0x200});
        break;
    case SAVETYPE_EEPROM_16K:
        components.push_back({"EEPROM", ".eep", eeprom, eepromSize});
        break;
    case SAVETYPE_SRAM:
        components.push_back({"SRAM", ".sra", sram, sramSize});
        break;
    case SAVETYPE_FLASH_RAM:
        components.push_back({"FlashRAM", ".fla", flash, flashSize});
        break;
    default:
        break;
    }
    if (ROM_SETTINGS.savetype == SAVETYPE_CONTROLLER_PAK || ROM_SETTINGS.mempak)
        components.push_back({"Controller Pak", ".mpk", mempak, mempakSize});
    return components;
}
} // namespace

void TicoCore::LoadSaveData()
{
    size_t total = 0;
    uint8_t *saves = (uint8_t *)tico_m64p_save_memory(&total);
    const std::string base = TicoConfig::SavesPath() + FilenameStem(m_gamePath);
    const std::vector<SaveComponent> components = GameSaveComponents();
    std::vector<bool> loaded(components.size(), false);

    for (size_t i = 0; i < components.size(); i++)
    {
        const SaveComponent &c = components[i];
        std::ifstream file(base + c.extension, std::ios::binary);
        if (!file || c.offset + c.size > total)
            continue;
        file.read((char *)saves + c.offset, (std::streamsize)c.size);
        loaded[i] = file.gcount() > 0;
        if (loaded[i])
            tico_debug_log("Loaded %s from %s%s", c.label, base.c_str(), c.extension);
    }

    // Older builds kept everything in one .srm, laid out like the core's
    // save block: take whatever the native files did not have.
    std::ifstream srm(base + ".srm", std::ios::binary);
    if (!srm)
        return;
    std::vector<uint8_t> legacy(total, 0);
    srm.read((char *)legacy.data(), (std::streamsize)legacy.size());
    const size_t available = (size_t)std::max<std::streamsize>(0, srm.gcount());
    for (size_t i = 0; i < components.size(); i++)
    {
        const SaveComponent &c = components[i];
        if (loaded[i] || c.offset >= available)
            continue;
        memcpy(saves + c.offset, legacy.data() + c.offset, std::min(c.size, available - c.offset));
        tico_debug_log("Imported %s from %s.srm", c.label, base.c_str());
    }
}

void TicoCore::SaveSaveData()
{
    size_t total = 0;
    const uint8_t *saves = (const uint8_t *)tico_m64p_save_memory(&total);
    TicoConfig::MakeDirs(TicoConfig::SavesPath());
    const std::string base = TicoConfig::SavesPath() + FilenameStem(m_gamePath);
    for (const SaveComponent &c : GameSaveComponents())
    {
        if (c.offset + c.size > total)
            continue;
        const std::string path = base + c.extension;
        if (TicoSafeFile::Write(path, saves + c.offset, c.size, kSaveBackups))
            tico_debug_log("Saved %s to %s", c.label, path.c_str());
        else
            tico_debug_log("ERROR: could not save %s to %s", c.label, path.c_str());
    }
}

void TicoCore::FlushSaves()
{
    if (m_gameLoaded)
        SaveSaveData();
}

//==============================================================================
// RetroAchievements
//==============================================================================

// rcheevos addresses the N64 by physical address: RDRAM from 0.
static uint32_t RC_CCONV RAReadMemory(uint32_t address, uint8_t* buffer, uint32_t num_bytes, rc_client_t* client)
{
    (void)client;
    if (!s_instance)
        return 0;
    for (const auto &map : s_instance->m_memoryMaps)
    {
        if (address >= map.start && address + num_bytes <= map.start + map.length)
        {
            memcpy(buffer, map.ptr + (address - map.start), num_bytes);
            return num_bytes;
        }
    }
    return 0;
}

void TicoCore::UpdateMemoryMaps()
{
    unsigned count = 0;
    const tico_m64p_memory_region *regions = tico_m64p_memory_regions(&count);
    m_memoryMaps.clear();
    for (unsigned i = 0; i < count; i++)
        m_memoryMaps.push_back({regions[i].start, regions[i].length, (uint8_t *)regions[i].ptr});
}

static size_t CurlWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Persistent RA worker thread entry point
void TicoCore::RAWorkerEntry(void* arg) {
    TicoCore* self = (TicoCore*)arg;

    while (true) {
        RAJob job;
        {
            std::unique_lock<std::mutex> lock(self->m_raJobMutex);
            self->m_raJobCond.wait(lock, [self]() {
                return !self->m_raJobQueue.empty() || !self->m_raWorkerRunning;
            });

            if (!self->m_raWorkerRunning && self->m_raJobQueue.empty())
                break;

            job = std::move(self->m_raJobQueue.front());
            self->m_raJobQueue.pop_front();
        }

        // Handle badge download jobs specially
        if (job.url == "__badge__") {
            self->DownloadAndCacheBadge(job.post_data);
            continue;
        }

        // Do the HTTP request on this worker thread
        CURL *curl = curl_easy_init();
        std::string readBuffer;
        long http_code = 0;
        std::string errorMsg;
        std::string requestUrl = job.url;

        if (curl) {
            curl_easy_setopt(curl, CURLOPT_URL, job.url.c_str());
            curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
            if (!job.post_data.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, job.post_data.c_str());
            }
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

            CURLcode res = curl_easy_perform(curl);
            if (res == CURLE_OK) {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            } else {
                errorMsg = curl_easy_strerror(res);
                http_code = 500;
            }
            curl_easy_cleanup(curl);
        }

        // The answer is handed to rc_client where it runs (OnFrame / Idle).
        {
            std::lock_guard<std::mutex> lock(self->m_raCallbackMutex);
            self->m_raPendingCallbacks.push_back(
                [job, http_code, readBuffer, errorMsg, requestUrl]() {
                    tico_debug_log("RA: HTTP Request -> %s", requestUrl.c_str());
                    if (!errorMsg.empty()) {
                        tico_debug_log("RA HTTP Error: %s", errorMsg.c_str());
                    }
                    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());

                    rc_api_server_response_t response;
                    memset(&response, 0, sizeof(response));
                    response.body = readBuffer.c_str();
                    response.body_length = readBuffer.size();
                    response.http_status_code = http_code;

                    rc_client_server_callback_t cb = (rc_client_server_callback_t)job.callback;
                    if (cb) {
                        cb(&response, job.callback_data);
                    }
                }
            );
        }
    }
}

void TicoCore::StartRAWorker() {
    m_raWorkerRunning = true;
    memset(&m_raThread, 0, sizeof(m_raThread));
    // core 0 (the emulator is on 1, the frontend on 2), normal priority, 256KB stack
    Result rc = threadCreate(&m_raThread, RAWorkerEntry, this, NULL, 0x40000, 0x2C, 0);
    if (R_SUCCEEDED(rc)) {
        rc = threadStart(&m_raThread);
        if (R_SUCCEEDED(rc)) {
            m_raThreadCreated = true;
            tico_debug_log("RA: Worker thread started (core 0, 256KB stack)");
        } else {
            tico_debug_log("RA: threadStart failed: 0x%x", rc);
            threadClose(&m_raThread);
            m_raWorkerRunning = false;
        }
    } else {
        tico_debug_log("RA: threadCreate failed: 0x%x", rc);
        m_raWorkerRunning = false;
    }
}

void TicoCore::StopRAWorker() {
    if (!m_raThreadCreated) return;

    {
        std::lock_guard<std::mutex> lock(m_raJobMutex);
        m_raWorkerRunning = false;
    }
    m_raJobCond.notify_one();

    threadWaitForExit(&m_raThread);
    threadClose(&m_raThread);
    m_raThreadCreated = false;
    tico_debug_log("RA: Worker thread stopped");
}

#ifndef TICO_APP_VERSION
#define TICO_APP_VERSION "dev"
#endif

// How RetroAchievements identifies this client: the frontend, the core and
// the rcheevos integration.
static const char *RAUserAgent()
{
    static std::string agent;
    if (agent.empty())
    {
        agent = std::string("tico-mupen64plus/") + TICO_APP_VERSION + " (Nintendo Switch) mupen64plus/2.6";
        char clause[64] = "";
        if (rc_client_get_user_agent_clause(nullptr, clause, sizeof(clause)) > 0)
            agent += std::string(" ") + clause;
    }
    return agent.c_str();
}

static void RC_CCONV RAServerCall(const rc_api_request_t* request, rc_client_server_callback_t callback, void* callback_data, rc_client_t* client)
{
    (void)client;
    if (!s_instance) return;

    TicoCore::RAJob job;
    job.url = request->url;
    if (request->post_data) job.post_data = request->post_data;
    job.callback = (void*)callback;
    job.callback_data = callback_data;

    if (s_instance->m_raWorkerRunning) {
        std::lock_guard<std::mutex> lock(s_instance->m_raJobMutex);
        s_instance->m_raJobQueue.push_back(std::move(job));
        s_instance->m_raJobCond.notify_one();
        return;
    }

    // Fallback: synchronous if worker not running
    tico_debug_log("RA: HTTP Request (sync) -> %s", request->url);
    CURL *curl = curl_easy_init();
    std::string readBuffer;
    long http_code = 0;
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, request->url);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
        if (request->post_data) curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->post_data);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
        CURLcode res = curl_easy_perform(curl);
        if (res == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        else { tico_debug_log("RA HTTP Error: %s", curl_easy_strerror(res)); http_code = 500; }
        curl_easy_cleanup(curl);
    }
    tico_debug_log("RA: HTTP Response %ld (size: %zu)", http_code, readBuffer.size());
    rc_api_server_response_t response;
    memset(&response, 0, sizeof(response));
    response.body = readBuffer.c_str();
    response.body_length = readBuffer.size();
    response.http_status_code = http_code;
    if (callback) callback(&response, callback_data);
}

//==============================================================================
// Construction
//==============================================================================

TicoCore::TicoCore()
{
    m_systemDir = TicoConfig::SystemPath();
    m_saveDir = TicoConfig::SavesPath();
    TicoConfig::MakeDirs(m_systemDir);
    TicoConfig::MakeDirs(m_saveDir);
}

TicoCore::~TicoCore()
{
    tico_debug_log("~TicoCore: destroying (gameLoaded=%d, initialized=%d)",
             m_gameLoaded, m_initialized);

    UnloadGame();

    tico_debug_log("~TicoCore: stopping the RetroAchievements worker");
    StopRAWorker();

    if (m_trophySound) {
        Mix_FreeChunk(m_trophySound);
        m_trophySound = nullptr;
    }

    if (m_rcClient) {
        rc_client_destroy(m_rcClient);
        m_rcClient = nullptr;
    }

    if (s_instance == this)
    {
        s_instance = nullptr;
    }

    tico_debug_log("~TicoCore: done");
}

//==============================================================================
// Initialization
//==============================================================================

bool TicoCore::Init()
{
    if (m_initialized)
        return true;

    s_instance = this;

    tico_debug_log("=== TicoCore::Init() ===");

    if (!s_vibrationInitialized)
    {
        memset(s_currentVibration, 0, sizeof(s_currentVibration));
        for(int i = 0; i < 5; i++) {
            for(int j = 0; j < 2; j++) {
                s_currentVibration[i][j].freq_low = 160.0f;
                s_currentVibration[i][j].freq_high = 320.0f;
            }
        }

        hidInitializeVibrationDevices(s_vibrationHandles[0], 2, HidNpadIdType_Handheld, HidNpadStyleTag_NpadHandheld);
        hidInitializeVibrationDevices(s_vibrationHandles[1], 2, HidNpadIdType_No1, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[2], 2, HidNpadIdType_No2, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[3], 2, HidNpadIdType_No3, HidNpadStyleTag_NpadJoyDual);
        hidInitializeVibrationDevices(s_vibrationHandles[4], 2, HidNpadIdType_No4, HidNpadStyleTag_NpadJoyDual);

        s_vibrationInitialized = true;
        tico_debug_log("Vibration devices initialized for P1-P4");
    }

    // Load configuration to ensure options are ready for the ROM
    LoadConfig();

    bool soundEnabled = false;
    std::ifstream audioIn("sdmc:/tico/config/audio.jsonc");
    if (audioIn.is_open()) {
        nlohmann::json j = nlohmann::json::parse(audioIn, nullptr, false, true);
        if (!j.is_discarded() && j.contains("sound_enabled") && j["sound_enabled"].is_boolean())
            soundEnabled = j["sound_enabled"].get<bool>();
    }
    if (soundEnabled) {
        m_trophySound = Mix_LoadWAV("romfs:/assets/trophy.mp3");
        if (m_trophySound) tico_debug_log("RA: Loaded trophy.mp3 successfully.");
        else tico_debug_log("RA: Failed to load trophy.mp3 -> %s", Mix_GetError());
    }

    tico_m64p_callbacks callbacks = {};
    callbacks.log = LogCallback;
    callbacks.get_option = OptionCallback;
    callbacks.audio_rate = AudioRateCallback;
    callbacks.audio_samples = AudioSamplesCallback;
    callbacks.rumble = RumbleCallback;
    callbacks.present_vulkan = PresentVulkanCallback;
    callbacks.present_gl = PresentGLCallback;
    callbacks.gl_default_framebuffer = GLFramebufferCallback;
    callbacks.gl_get_proc_address = GLProcAddressCallback;
    callbacks.emu_thread_begin = EmuThreadBeginCallback;
    callbacks.emu_thread_end = EmuThreadEndCallback;
    if (!tico_m64p_init(&callbacks, m_systemDir.c_str()))
    {
        tico_debug_log("ERROR: the core did not start");
        return false;
    }

    // ------------------------------------------------------------------
    // Setup RetroAchievements Client
    // ------------------------------------------------------------------
    LoadRAConfig();

    m_rcClient = rc_client_create(RAReadMemory, RAServerCall);
    if (m_rcClient) {
        rc_client_set_event_handler(m_rcClient, [](const rc_client_event_t* event, rc_client_t* client) {
            (void)client;
            if (!s_instance) return;
            switch (event->type) {
                case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
                    if (event->achievement) {
                        std::string title = event->achievement->title;
                        std::string desc = event->achievement->description;
                        std::string badge = event->achievement->badge_name;
                        s_instance->PushRANotification(title, desc, badge);
                        if (s_instance->m_trophySound) {
                            Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                        }
                        tico_debug_log("RA: Achievement triggered: %s (badge: %s)", title.c_str(), badge.c_str());
                    }
                    break;
                case RC_CLIENT_EVENT_GAME_COMPLETED:
                    s_instance->PushRANotification("Game Mastered!", "All achievements unlocked!", "ra_icon");
                    if (s_instance->m_trophySound) {
                        Mix_PlayChannel(-1, s_instance->m_trophySound, 0);
                    }
                    break;
                case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
                    if (event->leaderboard) {
                        s_instance->PushRANotification("Leaderboard", event->leaderboard->title, "ra_icon");
                    }
                    break;
                case RC_CLIENT_EVENT_RESET:
                    // rc_client asks for a reset when hardcore turns on mid-game,
                    // so nothing from the softcore session carries over.
                    tico_debug_log("RA: reset requested by rc_client");
                    tico_m64p_reset(false);
                    break;
                case RC_CLIENT_EVENT_SERVER_ERROR:
                    if (event->server_error) {
                        tico_debug_log("RA: Server error: %s", event->server_error->error_message);
                    }
                    break;
                default:
                    break;
            }
        });
        rc_client_set_hardcore_enabled(m_rcClient, m_raHardcore);
        StartRAWorker();
    }

    m_initialized = true;
    return true;
}

//==============================================================================
// tico_m64p callbacks
//==============================================================================

void TicoCore::LogCallback(int level, const char *msg)
{
    char buffer[1024];
    snprintf(buffer, sizeof(buffer), "%s", msg);
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n')
        buffer[len - 1] = '\0';
    switch (level)
    {
    case 3: LOG_ERROR("CORE", "%s", buffer); break;
    case 2: LOG_WARN("CORE", "%s", buffer); break;
    case 1: LOG_INFO("CORE", "%s", buffer); break;
    default: LOG_DEBUG("CORE", "%s", buffer); break;
    }
}

const char *TicoCore::OptionCallback(const char *key)
{
    if (!s_instance || !key)
        return nullptr;
    std::lock_guard<std::mutex> lock(s_instance->m_configMutex);
    auto it = s_instance->m_configOptions.find(key);
    return it != s_instance->m_configOptions.end() ? it->second.c_str() : nullptr;
}

void TicoCore::AudioRateCallback(unsigned rate)
{
    if (s_instance && s_instance->m_audioRateCallback)
        s_instance->m_audioRateCallback((double)rate);
}

void TicoCore::AudioSamplesCallback(const int16_t *frames, size_t count)
{
    if (s_instance && s_instance->m_audioSampleBatchCallback)
        s_instance->m_audioSampleBatchCallback(frames, count);
}

void TicoCore::RumbleCallback(unsigned port, bool on)
{
    if (!s_vibrationInitialized || port >= 4)
        return;
    int target = port + 1;
    if (port == 0 && appletGetOperationMode() == AppletOperationMode_Handheld)
        target = 0;
    HidVibrationValue *v = s_currentVibration[target];
    const float amplitude = on ? 1.0f : 0.0f;
    v[0].amp_low = v[1].amp_low = amplitude;
    v[0].amp_high = v[1].amp_high = amplitude;
    hidSendVibrationValues(s_vibrationHandles[target], v, 2);
}

void TicoCore::PresentVulkanCallback(unsigned width, unsigned height)
{
    if (!s_instance)
        return;
    s_instance->OnFrame();
    if (s_instance->m_hooks.presentVulkan)
        s_instance->m_hooks.presentVulkan(width, height);
}

void TicoCore::PresentGLCallback(unsigned width, unsigned height)
{
    if (!s_instance)
        return;
    s_instance->OnFrame();
    if (s_instance->m_hooks.presentGL)
        s_instance->m_hooks.presentGL(width, height);
}

unsigned TicoCore::GLFramebufferCallback(unsigned width, unsigned height)
{
    return s_instance && s_instance->m_hooks.glFramebuffer ? s_instance->m_hooks.glFramebuffer(width, height) : 0;
}

void *TicoCore::GLProcAddressCallback(const char *name)
{
    return s_instance && s_instance->m_hooks.glProcAddress ? s_instance->m_hooks.glProcAddress(name) : nullptr;
}

void TicoCore::EmuThreadBeginCallback()
{
    if (s_instance && s_instance->m_hooks.emuThreadBegin)
        s_instance->m_hooks.emuThreadBegin();
}

void TicoCore::EmuThreadEndCallback()
{
    if (s_instance && s_instance->m_hooks.emuThreadEnd)
        s_instance->m_hooks.emuThreadEnd();
}

//==============================================================================
// ROM files
//==============================================================================

static bool HasExtension(const std::string &name, const char *ext)
{
    const size_t n = strlen(ext);
    if (name.size() < n)
        return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower((unsigned char)name[name.size() - n + i]) != ext[i])
            return false;
    return true;
}

bool TicoCore::IsArchivePath(const std::string &path)
{
    return HasExtension(path, ".zip") || HasExtension(path, ".7z") || HasExtension(path, ".rar");
}

// A file the core loads: the extensions the module lists for n64.
static bool IsRomName(const std::string &name)
{
    for (const char *ext : {".z64", ".n64", ".v64", ".bin"})
        if (HasExtension(name, ext))
            return true;
    return false;
}

bool TicoCore::ReadRomFile(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
    {
        tico_debug_log("ERROR: Failed to open file: %s", path.c_str());
        return false;
    }
    fseek(fp, 0, SEEK_END);
    const long fileSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fileSize <= 0)
    {
        fclose(fp);
        tico_debug_log("ERROR: File is empty: %s", path.c_str());
        return false;
    }
    out.resize((size_t)fileSize);
    const size_t bytesRead = fread(out.data(), 1, out.size(), fp);
    fclose(fp);
    if (bytesRead != out.size())
    {
        tico_debug_log("ERROR: Short read: %zu of %zu bytes", bytesRead, out.size());
        return false;
    }
    return true;
}

// The first N64 ROM in a .zip, .7z or .rar (libarchive from portlibs).
bool TicoCore::ReadRomFromArchive(const std::string &path, std::vector<uint8_t> &out)
{
    struct archive *ar = archive_read_new();
    archive_read_support_format_zip(ar);
    archive_read_support_format_7zip(ar);
    archive_read_support_format_rar(ar);
    archive_read_support_format_rar5(ar);
    archive_read_support_filter_all(ar);
    if (archive_read_open_filename(ar, path.c_str(), 64 * 1024) != ARCHIVE_OK)
    {
        tico_debug_log("ERROR: Not a readable archive: %s (%s)", path.c_str(),
                       archive_error_string(ar));
        archive_read_free(ar);
        return false;
    }
    constexpr size_t kMaxRom = 64u * 1024u * 1024u;
    bool found = false;
    struct archive_entry *entry = nullptr;
    while (!found && archive_read_next_header(ar, &entry) == ARCHIVE_OK)
    {
        const char *name = archive_entry_pathname(entry);
        if (!name || archive_entry_filetype(entry) != AE_IFREG)
            continue;
        const std::string entryName = name;
        if (!IsRomName(entryName))
            continue;
        out.clear();
        if (archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0)
            out.reserve((size_t)archive_entry_size(entry));
        std::vector<uint8_t> chunk(256 * 1024);
        la_ssize_t read;
        while ((read = archive_read_data(ar, chunk.data(), chunk.size())) > 0 && out.size() <= kMaxRom)
            out.insert(out.end(), chunk.begin(), chunk.begin() + read);
        found = read == 0 && !out.empty() && out.size() <= kMaxRom;
        if (found)
            tico_debug_log("Loaded %s from %s", entryName.c_str(), path.c_str());
    }
    archive_read_free(ar);
    return found;
}

//==============================================================================
// Game Loading
//==============================================================================

bool TicoCore::LoadGame(const std::string &path)
{
    tico_debug_log("=== TicoCore::LoadGame ===");
    tico_debug_log("  path: %s", path.c_str());

    m_gamePath = path;
    if (!Init())
        return false;

    if (IsArchivePath(path))
    {
        if (!ReadRomFromArchive(path, m_romData))
        {
            tico_debug_log("ERROR: No N64 ROM found in %s", path.c_str());
            return false;
        }
    }
    else if (!ReadRomFile(path, m_romData))
    {
        return false;
    }
    tico_debug_log("ROM size: %zu bytes (%.1f MB)", m_romData.size(),
                   m_romData.size() / (1024.0 * 1024.0));

    tico_m64p_set_renderer(m_gliden64 ? TICO_M64P_RENDERER_GLIDEN64 : TICO_M64P_RENDERER_PARALLEL);
    // paraLLEl-RDP needs its device before the plugins start
    if (!m_gliden64 && m_hooks.createVulkanDevice && !m_hooks.createVulkanDevice())
    {
        tico_debug_log("ERROR: no Vulkan device for paraLLEl-RDP");
        return false;
    }
    if (!tico_m64p_load_rom(m_romData.data(), m_romData.size()))
    {
        tico_debug_log("ERROR: the core could not open the ROM");
        return false;
    }

    unsigned width = 640, height = 480;
    tico_m64p_screen_size(&width, &height);
    m_frameWidth = (int)width;
    m_frameHeight = (int)height;
    m_aspectRatio = tico_m64p_aspect_ratio();
    m_fps = tico_m64p_refresh_rate();
    if (m_fps <= 0.0)
        m_fps = 60.0;
    tico_debug_log("ROM \"%s\": %ux%u, %.2f fps, aspect %.3f, %s", tico_m64p_rom_name(), width, height, m_fps,
                   m_aspectRatio, m_gliden64 ? "GLideN64" : "paraLLEl-RDP");

    m_gameLoaded = true;
    LoadSaveData();
    LoadCheats();
    return true;
}

bool TicoCore::Start()
{
    if (!m_gameLoaded || m_started)
        return false;
    m_started = tico_m64p_start();
    if (!m_started)
        return false;

    // RetroAchievements: log in, then identify the game (both answered on
    // the worker; the game is hashed from the ROM data)
    if (m_rcClient && m_raEnabled && !m_raUsername.empty())
    {
        if (!m_raToken.empty())
        {
            tico_debug_log("RA: Existent token found. Auto login as %s...", m_raUsername.c_str());
            rc_client_begin_login_with_token(m_rcClient, m_raUsername.c_str(), m_raToken.c_str(),
                [](int res, const char* err, rc_client_t* c, void* ud) {
                    TicoCore* self = (TicoCore*)ud;
                    if (res == RC_OK) {
                        tico_debug_log("RA login success with token!");
                        if (self->m_gameLoaded && !self->m_gamePath.empty())
                            RAIdentifyGame(c, self);
                    } else if (res == RC_INVALID_CREDENTIALS && !self->m_raPassword.empty()) {
                        tico_debug_log("RA token invalid or expired. Trying password...");
                        RALoginWithPassword(c, self);
                    } else {
                        tico_debug_log("RA login failed -> %s", err ? err : "Unknown");
                        self->PushRANotification("Login Failed", "Check your credentials.", "ra_icon");
                    }
                }, this);
        }
        else if (!m_raPassword.empty())
        {
            tico_debug_log("RA: Auto login using password...");
            RALoginWithPassword(m_rcClient, this);
        }
    }
    return true;
}

void TicoCore::UnloadGame()
{
    if (!m_gameLoaded)
        return;

    tico_debug_log("Stopping the emulator...");
    tico_m64p_stop();
    m_started = false;
    SaveSaveData();
    if (m_rcClient)
    {
        std::lock_guard<std::mutex> lock(m_raMutex);
        rc_client_unload_game(m_rcClient);
    }
    m_memoryMaps.clear();
    m_gameLoaded = false;
}

bool TicoCore::IsRunning() const
{
    return m_started && tico_m64p_running();
}

void TicoCore::Reset()
{
    if (!m_gameLoaded)
        return;
    tico_m64p_reset(false);
    // achievement progress restarts with the game
    if (m_rcClient)
    {
        std::lock_guard<std::mutex> lock(m_raMutex);
        rc_client_reset(m_rcClient);
    }
}

//==============================================================================
// Per-frame work
//==============================================================================

void TicoCore::RunRACallbacks()
{
    std::vector<std::function<void()>> cbs;
    {
        std::lock_guard<std::mutex> lock(m_raCallbackMutex);
        cbs = std::move(m_raPendingCallbacks);
    }
    for (auto &cb : cbs)
        cb();
}

void TicoCore::OnFrame()
{
    if (!m_rcClient)
        return;
    std::lock_guard<std::mutex> lock(m_raMutex);
    if (m_memoryMaps.empty())
        UpdateMemoryMaps();
    RunRACallbacks();
    rc_client_do_frame(m_rcClient);
}

void TicoCore::Idle()
{
    if (!m_rcClient)
        return;
    std::lock_guard<std::mutex> lock(m_raMutex);
    RunRACallbacks();
    rc_client_idle(m_rcClient);
}

//==============================================================================
// Input
//==============================================================================

void TicoCore::SetPad(unsigned port, bool present, uint32_t buttons, int8_t stickX, int8_t stickY)
{
    tico_m64p_set_pad(port, present, buttons, stickX, stickY);
}

//==============================================================================
// Cheats
//==============================================================================
static std::string CheatsBase(const std::string &gamePath)
{
    const std::string dir = "sdmc:/tico/cheats/" + TicoConfig::CURRENT_SLUG + "/";
    TicoConfig::MakeDirs(dir);
    return dir + FilenameStem(gamePath);
}

// One cheat's codes, joined with + or ; in the files
static void SplitCodes(const std::string &text, std::vector<std::string> &out)
{
    std::string code;
    for (const char c : text + ";")
    {
        if (c == '+' || c == ';' || c == ',')
        {
            code = TicoUtils::Trim(code);
            if (!code.empty())
                out.push_back(code);
            code.clear();
        }
        else
            code += c;
    }
}

void TicoCore::LoadCheats()
{
    m_cheats.clear();
    const std::string base = CheatsBase(m_gamePath);

    // RetroArch .cht: cheatN_desc / cheatN_code (enable flags are ignored:
    // every cheat starts off)
    std::ifstream cht(base + ".cht");
    if (cht.is_open())
    {
        std::map<int, Cheat> byIndex;
        std::string line;
        while (std::getline(cht, line))
        {
            const size_t eq = line.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string key = TicoUtils::Trim(line.substr(0, eq));
            std::string value = TicoUtils::Trim(line.substr(eq + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            int index = -1;
            char field[16] = {0};
            if (sscanf(key.c_str(), "cheat%d_%15s", &index, field) != 2 || index < 0)
                continue;
            if (!strcmp(field, "desc"))
                byIndex[index].name = value;
            else if (!strcmp(field, "code"))
                SplitCodes(value, byIndex[index].codes);
        }
        for (auto &entry : byIndex)
        {
            if (entry.second.codes.empty())
                continue;
            if (entry.second.name.empty())
                entry.second.name = "Cheat " + std::to_string(entry.first + 1);
            m_cheats.push_back(entry.second);
        }
    }

    // .cheats: "# Name", then one code (or several joined with +) per line
    std::ifstream simple(base + ".cheats");
    if (simple.is_open())
    {
        std::string line;
        while (std::getline(simple, line))
        {
            const std::string text = TicoUtils::Trim(line);
            if (text.empty() || text[0] == '!')
                continue;
            if (text[0] == '#')
            {
                m_cheats.push_back(Cheat());
                m_cheats.back().name = TicoUtils::Trim(text.substr(1));
                continue;
            }
            if (m_cheats.empty())
            {
                m_cheats.push_back(Cheat());
                m_cheats.back().name = "Cheat";
            }
            SplitCodes(text, m_cheats.back().codes);
        }
    }
    tico_debug_log("CHEATS: %zu for %s", m_cheats.size(), base.c_str());
}

void TicoCore::ApplyCheats()
{
    if (!m_gameLoaded)
        return;
    tico_m64p_cheats_clear();
    unsigned index = 0;
    for (const Cheat &cheat : m_cheats)
    {
        if (!cheat.enabled)
            continue;
        std::string codes;
        for (const std::string &code : cheat.codes)
            codes += code + "\n";
        const std::string name = "cheat_" + std::to_string(index++);
        if (!tico_m64p_cheat_add(name.c_str(), codes.c_str()))
            tico_debug_log("CHEATS: could not apply %s", cheat.name.c_str());
    }
}

void TicoCore::ToggleCheat(size_t index)
{
    if (index >= m_cheats.size() || IsHardcoreActive())
        return;
    m_cheats[index].enabled = !m_cheats[index].enabled;
    ApplyCheats();
}

bool TicoCore::IsHardcoreActive() const
{
    return m_rcClient && rc_client_get_hardcore_enabled(m_rcClient);
}

bool TicoCore::CanPause(int &secondsRemaining)
{
    secondsRemaining = 0;
    if (!m_gameLoaded || !IsHardcoreActive())
        return true;
    uint32_t framesRemaining = 0;
    if (rc_client_can_pause(m_rcClient, &framesRemaining))
        return true;
    const double fps = m_fps > 0.0 ? m_fps : 60.0;
    secondsRemaining = (int)((framesRemaining + fps - 1.0) / fps);
    if (secondsRemaining < 1)
        secondsRemaining = 1;
    return false;
}

//==============================================================================
// Save States
//==============================================================================

// rc_client's achievement progress (hit counts, measured values) for a state
// file, so loading it restores where every achievement stood.
static std::string ProgressPath(const std::string &statePath)
{
    return statePath + ".ra";
}

bool TicoCore::SaveState(const std::string &path)
{
    if (!IsRunning())
        return false;

    std::vector<uint8_t> data(tico_m64p_state_size());
    if (!tico_m64p_save_state(data.data(), data.size()))
    {
        tico_debug_log("ERROR: the core could not save its state");
        return false;
    }

    tico_debug_log("SaveState: the core took the state, writing %s", path.c_str());
    // the slot's previous state stays in backups/ (one level)
    const bool written = TicoSafeFile::Write(path, data.data(), data.size(), kStateBackups);
    if (!written)
    {
        tico_debug_log("ERROR: Failed to write save state: %s", path.c_str());
        return false;
    }
    tico_debug_log("Saved state to %s", path.c_str());

    std::lock_guard<std::mutex> lock(m_raMutex);
    const std::string progressPath = ProgressPath(path);
    const size_t progressSize = m_rcClient ? rc_client_progress_size(m_rcClient) : 0;
    std::vector<uint8_t> progress(progressSize);
    if (progressSize > 0 &&
        rc_client_serialize_progress_sized(m_rcClient, progress.data(), progressSize) == RC_OK)
    {
        if (FILE *pf = fopen(progressPath.c_str(), "wb"))
        {
            fwrite(progress.data(), 1, progressSize, pf);
            fclose(pf);
        }
    }
    else
    {
        // a stale file would restore progress from an older state
        remove(progressPath.c_str());
    }
    return written;
}

bool TicoCore::LoadState(const std::string &path)
{
    if (!IsRunning())
        return false;

    // RetroAchievements hardcore forbids loading states.
    if (IsHardcoreActive())
    {
        tico_debug_log("LoadState: refused, hardcore mode is active");
        return false;
    }

    std::vector<uint8_t> data;
    if (!ReadRomFile(path, data))
    {
        tico_debug_log("LoadState: no state at %s", path.c_str());
        return false;
    }

    if (m_audioFlushCallback)
        m_audioFlushCallback();

    if (!tico_m64p_load_state(data.data(), data.size()))
    {
        tico_debug_log("ERROR: the core could not load %s", path.c_str());
        return false;
    }
    tico_debug_log("Loaded state from %s", path.c_str());

    // Restore achievement progress with the state; a state saved without it
    // resets progress, so nothing from the abandoned timeline counts.
    if (m_rcClient)
    {
        std::vector<uint8_t> progress;
        if (FILE *pf = fopen(ProgressPath(path).c_str(), "rb"))
        {
            fseek(pf, 0, SEEK_END);
            const long progressSize = ftell(pf);
            fseek(pf, 0, SEEK_SET);
            if (progressSize > 0)
            {
                progress.resize((size_t)progressSize);
                if (fread(progress.data(), 1, progress.size(), pf) != progress.size())
                    progress.clear();
            }
            fclose(pf);
        }
        std::lock_guard<std::mutex> lock(m_raMutex);
        if (progress.empty() ||
            rc_client_deserialize_progress_sized(m_rcClient, progress.data(), progress.size()) != RC_OK)
            rc_client_deserialize_progress_sized(m_rcClient, nullptr, 0);
    }
    return true;
}

//==============================================================================
// Configuration
//==============================================================================

void TicoCore::SetOption(const std::string &key, const std::string &value)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_configOptions[key] = value;
}

void TicoCore::ApplyOptions()
{
    if (m_gameLoaded)
        tico_m64p_apply_options();
}

void TicoCore::LoadConfig()
{
    if (m_configLoaded)
        return;
    m_configLoaded = true;

    const char *configPath = "sdmc:/tico/config/cores/mupen64plus.jsonc";
    std::ifstream f(configPath);
    if (!f.good())
    {
        tico_debug_log("No config found at %s. Using defaults.", configPath);
        return;
    }

    nlohmann::json j = nlohmann::json::parse(f, nullptr, false, true);
    if (j.is_discarded())
    {
        tico_debug_log("ERROR: Failed to parse config at %s", configPath);
        return;
    }

    std::lock_guard<std::mutex> lock(m_configMutex);
    for (auto &el : j.items())
    {
        // settings.json's options win: ApplySettingsToCore sets them all
        if (m_configOptions.count(el.key()))
            continue;
        if (el.value().is_string())
            m_configOptions[el.key()] = el.value().get<std::string>();
        else if (el.value().is_boolean())
            m_configOptions[el.key()] = el.value().get<bool>() ? "True" : "False";
        else if (el.value().is_number())
            m_configOptions[el.key()] = std::to_string(el.value().get<int>());
    }
    tico_debug_log("Loaded %zu options from %s", m_configOptions.size(), configPath);
}

void TicoCore::LoadRAConfig()
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    std::ifstream file(accountsPath);
    if (!file.is_open()) {
        tico_debug_log("WARN: accounts.jsonc not found at %s", accountsPath.c_str());
        return;
    }

    tico_debug_log("RA: Found accounts.jsonc at %s", accountsPath.c_str());
    nlohmann::json j = nlohmann::json::parse(file, nullptr, false, true);
    if (!j.is_discarded() && j.is_object()) {
        m_raEnabled = j.value("ra_enabled", false);
        m_raUsername = j.value("ra_username", "");
        m_raToken = j.value("ra_token", "");
        m_raPassword = j.value("ra_password", "");
        m_raHardcore = j.value("ra_hardcore_mode", false);

        // Read alert position
        std::string posStr = j.value("ra_alert_position", "top_right");
        if (posStr == "top_left") m_raAlertPosition = RAAlertPosition::TopLeft;
        else if (posStr == "top_right") m_raAlertPosition = RAAlertPosition::TopRight;
        else if (posStr == "bottom_left") m_raAlertPosition = RAAlertPosition::BottomLeft;
        else if (posStr == "bottom_right") m_raAlertPosition = RAAlertPosition::BottomRight;

        tico_debug_log("RA: Config loaded (Enabled: %d, User: %s, HasToken: %d, HasPassword: %d)",
            m_raEnabled, m_raUsername.c_str(), !m_raToken.empty(), !m_raPassword.empty());
    } else {
        tico_debug_log("WARN: Failed to parse accounts.jsonc for RA settings.");
    }
}

void TicoCore::SaveRAToken(const std::string& token)
{
    std::string accountsPath = "sdmc:/tico/config/accounts.jsonc";
    nlohmann::json j = nlohmann::json::object();

    // Read existing config
    std::ifstream inFile(accountsPath);
    if (inFile.is_open()) {
        auto parsed = nlohmann::json::parse(inFile, nullptr, false, true);
        inFile.close();
        if (!parsed.is_discarded()) j = parsed;
    }

    // Update token
    j["ra_token"] = token;
    m_raToken = token;

    // Write back
    std::ofstream outFile(accountsPath);
    if (outFile.is_open()) {
        outFile << j.dump(4);
        outFile.close();
        tico_debug_log("RA: Token saved to accounts.jsonc");
    } else {
        tico_debug_log("RA: WARNING - Failed to save token to %s", accountsPath.c_str());
    }
}

void TicoCore::RAIdentifyGame(rc_client_t* c, TicoCore* core)
{
    const uint32_t console_id = TicoConfig::GetRcConsoleId();

    tico_debug_log("RA: Identifying game... (Console ID: %u)", console_id);
    // hashed from the loaded ROM, so a zipped game is recognized too
    rc_client_begin_identify_and_load_game(c, console_id, core->m_gamePath.c_str(),
        core->m_romData.empty() ? nullptr : core->m_romData.data(), core->m_romData.size(),
        [](int result, const char* error_message, rc_client_t* client, void* userdata) {
            TicoCore* core = (TicoCore*)userdata;
            if (result == RC_OK) {
                tico_debug_log("RA: Game loaded and identified!");
                const rc_client_game_t* game = rc_client_get_game_info(client);
                if (game && game->title) {
                    core->PushRANotification("RetroAchievements",
                        std::string("Playing: ") + game->title, "ra_icon");
                }
                // Preload all achievement badges in the background
                core->PreloadRABadges();
            } else {
                tico_debug_log("RA: Failed to identify game: %s", error_message ? error_message : "Unknown");
                core->PushRANotification("RetroAchievements",
                    "Rom hash doesn't match or unable to recognize the game, achievements disabled.", "ra_icon");
            }
        }, core);
}

void TicoCore::RALoginWithPassword(rc_client_t* c, TicoCore* core)
{
    if (core->m_raPassword.empty()) {
        tico_debug_log("RA: No password configured. Continuing without RA.");
        return;
    }

    tico_debug_log("RA: Logging in with password...");
    rc_client_begin_login_with_password(c, core->m_raUsername.c_str(), core->m_raPassword.c_str(),
        [](int res, const char* err, rc_client_t* c, void* ud) {
            TicoCore* core = (TicoCore*)ud;
            if (res == RC_OK) {
                const rc_client_user_t* user = rc_client_get_user_info(c);
                if (user && user->token) {
                    tico_debug_log("RA: Password login successful! Saving token...");
                    core->SaveRAToken(user->token);
                } else {
                    tico_debug_log("RA: Password login OK but no token returned");
                }
                RAIdentifyGame(c, core);
            } else {
                tico_debug_log("RA: Password login failed: %s. Continuing without RA.", err ? err : "Unknown");
                core->PushRANotification("RetroAchievements",
                    "Failed to authenticate, check your username/password and try again.", "ra_icon");
            }
        }, core);
}

void TicoCore::PushRANotification(const std::string& title, const std::string& desc,
                                   const std::string& badge)
{
    RANotification n;
    n.title = title;
    n.description = desc;
    n.badge_name = badge;
    n.timer = 0.0f;
    // the texture is looked up when the overlay draws it, on the main thread
    std::lock_guard<std::mutex> lock(m_raNotificationMutex);
    m_raPendingNotifications.push_back(std::move(n));
    tico_debug_log("RA: Notification pushed: %s - %s (badge: %s)",
        title.c_str(), desc.c_str(), badge.c_str());
}

void TicoCore::CollectRANotifications()
{
    std::lock_guard<std::mutex> lock(m_raNotificationMutex);
    for (RANotification &n : m_raPendingNotifications)
    {
        // Cap at 5 visible notifications
        if (m_raNotifications.size() >= 5)
            m_raNotifications.erase(m_raNotifications.begin());
        m_raNotifications.push_back(std::move(n));
    }
    m_raPendingNotifications.clear();
}

ImTextureID TicoCore::GetRABadgeTexture(const std::string& badge_name)
{
    // Check cache first
    auto it = m_raBadgeCache.find(badge_name);
    if (it != m_raBadgeCache.end()) return it->second;

    // Try loading from SD card cache (the worker downloads them there)
    std::string path = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    int w, h, ch;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (data) {
        ImTextureID tex = TicoRenderer::CreateTextureRGBA(data, w, h);
        stbi_image_free(data);
        m_raBadgeCache[badge_name] = tex;
        return tex;
    }
    return ImTextureID_Invalid;
}

void TicoCore::DownloadAndCacheBadge(const std::string& badge_name)
{
    // Check if already cached on disk
    std::string cachePath = "sdmc:/tico/assets/ra/" + badge_name + ".png";
    FILE* check = fopen(cachePath.c_str(), "rb");
    if (check) { fclose(check); return; } // already on disk

    // Download from RA
    std::string url = "https://media.retroachievements.org/Badge/" + badge_name + ".png";
    std::string response;

    CURL* curl = curl_easy_init();
    if (!curl) return;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, RAUserAgent());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || httpCode != 200 || response.empty()) {
        tico_debug_log("RA: Failed to download badge %s (http %ld)", badge_name.c_str(), httpCode);
        return;
    }

    // Ensure directory exists
    mkdir("sdmc:/tico/assets/ra", 0777);

    // Save to disk
    FILE* fp = fopen(cachePath.c_str(), "wb");
    if (fp) {
        fwrite(response.data(), 1, response.size(), fp);
        fclose(fp);
        tico_debug_log("RA: Cached badge %s (%zu bytes)", badge_name.c_str(), response.size());
    }
}

void TicoCore::PreloadRABadges()
{
    if (!m_rcClient) return;

    tico_debug_log("RA: Preloading achievement badges...");

    // Get all achievement lists
    rc_client_achievement_list_t* list = rc_client_create_achievement_list(m_rcClient,
        RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
    if (!list) {
        tico_debug_log("RA: No achievement list to preload");
        return;
    }

    // Collect all unique badge names not on disk yet
    std::vector<std::string> badges;
    for (uint32_t b = 0; b < list->num_buckets; b++) {
        for (uint32_t a = 0; a < list->buckets[b].num_achievements; a++) {
            const rc_client_achievement_t* ach = list->buckets[b].achievements[a];
            if (ach && ach->badge_name[0]) {
                std::string bn = ach->badge_name;
                std::string path = "sdmc:/tico/assets/ra/" + bn + ".png";
                FILE* check = fopen(path.c_str(), "rb");
                if (check) { fclose(check); continue; }
                badges.push_back(bn);
            }
        }
    }
    rc_client_destroy_achievement_list(list);

    if (badges.empty()) {
        tico_debug_log("RA: All badges already cached");
        return;
    }

    tico_debug_log("RA: Need to download %zu badges", badges.size());

    // The worker downloads them: a job with url "__badge__" names a badge.
    for (const auto& badge : badges) {
        {
            std::lock_guard<std::mutex> lock(m_raJobMutex);
            RAJob job;
            job.url = "__badge__";
            job.post_data = badge;
            job.callback = nullptr;
            job.callback_data = nullptr;
            m_raJobQueue.push_back(std::move(job));
        }
        m_raJobCond.notify_one();
    }
}
