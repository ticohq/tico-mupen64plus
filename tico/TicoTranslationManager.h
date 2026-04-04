/// @file TicoTranslationManager.h
/// @brief Simple i18n system loading JSON language files from romfs
#pragma once

#include <string>
#include <unordered_map>

/// @brief Singleton translation manager for UI string localization
class TicoTranslationManager {
public:
    static TicoTranslationManager& Instance();

    /// @brief Load language file based on general.jsonc config
    bool Init();

    /// @brief Get translated string by key, returns key if not found
    std::string GetString(const std::string& key) const;

private:
    TicoTranslationManager() = default;

    std::string m_currentLanguage;
    std::unordered_map<std::string, std::string> m_translations;
};

/// @brief Global shorthand for TicoTranslationManager::GetString
std::string tr(const std::string& key);
