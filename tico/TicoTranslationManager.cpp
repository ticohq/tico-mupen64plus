/// @file TicoTranslationManager.cpp
/// @brief Implementation of i18n translation loading

#include "TicoTranslationManager.h"
#include <json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

TicoTranslationManager& TicoTranslationManager::Instance() {
    static TicoTranslationManager instance;
    return instance;
}

bool TicoTranslationManager::Init() {
    std::string language = "English";

    std::ifstream cfgFile("sdmc:/tico/config/general.jsonc");
    if (cfgFile.is_open()) {
        json cfg = json::parse(cfgFile, nullptr, false, true);
        if (!cfg.is_discarded() && cfg.contains("language") && cfg["language"].is_string()) {
            language = cfg["language"].get<std::string>();
        }
        cfgFile.close();
    }

    if (m_currentLanguage == language && !m_translations.empty()) {
        return true;
    }

    m_translations.clear();
    m_currentLanguage = language;

    std::string filename = "";
    if (language == "English") filename = "en.json";
    else if (language == "Portuguese") filename = "pt.json";
    else if (language == "Espanol") filename = "es.json";
    else if (language == "Japanese") filename = "ja.json";
    else if (language == "French") filename = "fr.json";
    else if (language == "Chinese") filename = "zh.json";
    else filename = "en.json";

    std::string langPath = "romfs:/lang/" + filename;
    std::ifstream file(langPath);
    if (!file.is_open()) {
        return false;
    }

    json j = json::parse(file, nullptr, false);
    if (!j.is_discarded()) {
        for (auto& el : j.items()) {
            if (el.value().is_string()) {
                m_translations[el.key()] = el.value().get<std::string>();
            }
        }
    }

    return true;
}

std::string TicoTranslationManager::GetString(const std::string& key) const {
    auto it = m_translations.find(key);
    if (it != m_translations.end()) {
        return it->second;
    }
    return key;
}

std::string tr(const std::string& key) {
    return TicoTranslationManager::Instance().GetString(key);
}
