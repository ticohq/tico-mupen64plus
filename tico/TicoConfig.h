/// @file TicoConfig.h
/// @brief Minimal hardcoded configuration for tico overlay (mupen64plus)
#pragma once

#include <string>

namespace TicoConfig {
    constexpr const char* TEST_ROM = "sdmc:/tico/roms/n64/mario.z64";
    // without a launch argument, the first N64 ROM found here is loaded
    constexpr const char* ROM_FALLBACK_DIR = "sdmc:/switch/mupen64plus/";

    constexpr const char* FONT_PATH = "romfs:/fonts/font.ttf";
    constexpr const char* SYSTEM_PATH = "sdmc:/tico/system/n64/";
    constexpr const char* SAVES_PATH = "sdmc:/tico/saves/n64/";
    constexpr const char* STATES_PATH = "sdmc:/tico/states/n64/";

    constexpr int WINDOW_WIDTH = 1280;
    constexpr int WINDOW_HEIGHT = 720;
    constexpr float FONT_SIZE = 32.0f;

    /// @brief Use callback/ring-buffer path (supports resampling)
    constexpr bool USE_SDLQUEUEAUDIO = false;
}

/// @brief UI action identifiers for the helpers bar
enum UIActions {
    ACTION_CONFIRM,
    ACTION_BACK,
    ACTION_DETAILS,
    ACTION_MENU,
    ACTION_EDIT,
    ACTION_DELETE
};
