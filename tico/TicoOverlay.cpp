/// @file TicoOverlay.cpp
/// @brief Overlay UI for tico-integrated mupen64plus (no disc support)

#define IMGUI_DEFINE_MATH_OPERATORS
#include "TicoOverlay.h"
#include "TicoCore.h"
#include "TicoConfig.h"
#include "TicoTranslationManager.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include "TicoUtils.h"
#include <json.hpp>

#ifdef __SWITCH__
#include "glad.h"
#else
#include "glad.h"
#endif

#include <sys/stat.h>
#include <dirent.h>
#include <string>

#ifdef __SWITCH__
#include <switch.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

static std::string GetStatePath(TicoCore *core, int slot)
{
    if (!core) return "";
    std::string romPath = core->GetGamePath();
    std::string romName = romPath;
    size_t lastSlash = romName.find_last_of("/\\");
    if (lastSlash != std::string::npos) romName = romName.substr(lastSlash + 1);
    size_t lastDot = romName.find_last_of(".");
    if (lastDot != std::string::npos) romName = romName.substr(0, lastDot);
    struct stat st = {0};
    if (stat(TicoConfig::STATES_PATH, &st) == -1) mkdir(TicoConfig::STATES_PATH, 0777);
    return std::string(TicoConfig::STATES_PATH) + romName + ".state" + std::to_string(slot);
}

namespace UIStyle {
    inline void DrawTextWithShadow(ImDrawList *dl, ImVec2 pos, ImU32 color, const char *text, float shadowOffset = 1.5f) {
        dl->AddText(ImVec2(pos.x + shadowOffset, pos.y + shadowOffset), IM_COL32(0,0,0,50), text);
        dl->AddText(pos, color, text);
    }
    static void DrawSwitchButton(ImDrawList *dl, ImFont *font, float fontSize, ImVec2 center, float size, const char *symbol, float alpha, bool isDark) {
        ImU32 fillCol = IM_COL32(220, 220, 220, (int)(255 * alpha));
        ImU32 textCol = IM_COL32(40, 40, 40, (int)(255 * alpha));
        dl->AddCircleFilled(center, size * 0.5f, fillCol, 12);
        float symSize = fontSize * 0.75f;
        ImVec2 textSize = font->CalcTextSizeA(symSize, FLT_MAX, 0.0f, symbol);
        dl->AddText(font, symSize, center - (textSize * 0.5f), textCol, symbol);
    }
}

TicoOverlay::TicoOverlay() {
    m_gameTitle = "mupen64plus";
    LoadConfig();
    LoadGeneralConfig();
    LoadAccountData();
    LoadCoreSettings();
#ifdef __SWITCH__
    psmInitialize();
#endif
}

TicoOverlay::~TicoOverlay()
{
    if (m_triangleTexture != 0)
    {
        glDeleteTextures(1, &m_triangleTexture);
        m_triangleTexture = 0;
    }

    if (m_boltTexture != 0)
    {
        glDeleteTextures(1, &m_boltTexture);
        m_boltTexture = 0;
    }

    if (m_avatarTexture != 0)
    {
        glDeleteTextures(1, &m_avatarTexture);
        m_avatarTexture = 0;
    }

#ifdef __SWITCH__
    psmExit();
#endif
}

void TicoOverlay::LoadConfig() {
    const char *configPaths[] = {"sdmc:/tiicu/config/display.jsonc","sdmc:/tico/config/display.jsonc","tico/config/display.jsonc"};
    m_isDarkMode = true; m_showNickname = false;
    FILE *fp = nullptr;
    for (const char *path : configPaths) { fp = fopen(path, "rb"); if (fp) break; }
    if (fp) {
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            std::string content; content.resize(size); fread(&content[0], 1, size, fp);
            auto j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded()) {
                    if (j.contains("dark_mode") && j["dark_mode"].is_boolean()) m_isDarkMode = j["dark_mode"].get<bool>();
                    else if (j.contains("darkMode") && j["darkMode"].is_boolean()) m_isDarkMode = j["darkMode"].get<bool>();
                    if (j.contains("show_nickname") && j["show_nickname"].is_boolean()) m_showNickname = j["show_nickname"].get<bool>();
                    else if (j.contains("showNickname") && j["showNickname"].is_boolean()) m_showNickname = j["showNickname"].get<bool>();
                }
        }
        fclose(fp);
    }
}

void TicoOverlay::LoadGeneralConfig() {
    const char *configPaths[] = {"sdmc:/tiicu/config/general.jsonc","sdmc:/tico/config/general.jsonc","tico/config/general.jsonc"};
    m_hourFormat = "24h";
    FILE *fp = nullptr;
    for (const char *path : configPaths) { fp = fopen(path, "rb"); if (fp) break; }
    if (fp) {
        fseek(fp, 0, SEEK_END); long size = ftell(fp); fseek(fp, 0, SEEK_SET);
        if (size > 0) {
            std::string content; content.resize(size); fread(&content[0], 1, size, fp);
            auto j = nlohmann::json::parse(content, nullptr, false, true);
                if (!j.is_discarded() && j.contains("hour_format") && j["hour_format"].is_string())
                    m_hourFormat = j["hour_format"].get<std::string>();
        }
        fclose(fp);
    }
}

void TicoOverlay::LoadAccountData() {
#ifdef __SWITCH__
    bool customAvatarLoaded = false;
    const char *avatarPaths[] = {"sdmc:/tico/assets/avatar.jpg"};
    for (const char *path : avatarPaths) {
        FILE *fp = fopen(path, "rb"); if (!fp) continue; fclose(fp);
        int width, height, channels;
        unsigned char *data = stbi_load(path, &width, &height, &channels, 4);
        if (data) {
            if (m_avatarTexture != 0) glDeleteTextures(1, &m_avatarTexture);
            glGenTextures(1, &m_avatarTexture); glBindTexture(GL_TEXTURE_2D, m_avatarTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            glBindTexture(GL_TEXTURE_2D, 0); stbi_image_free(data);
            m_nickname = "Player 1"; customAvatarLoaded = true; break;
        }
    }
    if (customAvatarLoaded) return;
    Result rc = accountInitialize(AccountServiceType_Application);
    if (R_FAILED(rc)) return;
    AccountUid uid = {0}; bool found = false;
    if (R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid)) found = true;
    if (!found && R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid)) found = true;
    if (!found) {
        s32 userCount = 0;
        if (R_SUCCEEDED(accountGetUserCount(&userCount)) && userCount > 0) {
            AccountUid uids[ACC_USER_LIST_SIZE]; s32 actualTotal = 0;
            if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &actualTotal)) && actualTotal > 0) { uid = uids[0]; found = true; }
        }
    }
    if (found) {
        AccountProfile profile; AccountProfileBase profileBase;
        if (R_SUCCEEDED(accountGetProfile(&profile, uid))) {
            if (R_SUCCEEDED(accountProfileGet(&profile, NULL, &profileBase))) m_nickname = std::string(profileBase.nickname);
            u32 imageSize = 0;
            if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &imageSize)) && imageSize > 0) {
                unsigned char *jpegBuf = (unsigned char *)malloc(imageSize);
                if (jpegBuf) {
                    u32 actualSize = 0;
                    if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpegBuf, imageSize, &actualSize))) {
                        int width, height, channels;
                        unsigned char *rgba = stbi_load_from_memory(jpegBuf, actualSize, &width, &height, &channels, 4);
                        if (rgba) {
                            if (m_avatarTexture != 0) glDeleteTextures(1, &m_avatarTexture);
                            glGenTextures(1, &m_avatarTexture); glBindTexture(GL_TEXTURE_2D, m_avatarTexture);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                            glBindTexture(GL_TEXTURE_2D, 0); stbi_image_free(rgba);
                        }
                    }
                    free(jpegBuf);
                }
            }
            accountProfileClose(&profile);
        }
    }
    accountExit();
#else
    m_nickname = "Player 1";
#endif
}

void TicoOverlay::EnsureRAIconLoaded() {
    if (!m_core || m_raIconLoadAttempted) return;
    m_raIconLoadAttempted = true;

    if (m_core->m_raIconTexture != 0) return; // already loaded

    const char* svgPath = "romfs:/assets/ra.svg";
    NSVGimage* image = nsvgParseFromFile(svgPath, "px", 96);
    if (image) {
        float sc = 64.0f / image->height;
        int w = (int)(image->width * sc), h = (int)(image->height * sc);
        NSVGrasterizer* rast = nsvgCreateRasterizer();
        if (rast) {
            unsigned char* img = (unsigned char*)malloc(w * h * 4);
            if (img) {
                nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
                unsigned int tex = 0;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
                glBindTexture(GL_TEXTURE_2D, 0);
                m_core->m_raIconTexture = tex;
                free(img);
            }
            nsvgDeleteRasterizer(rast);
        }
        nsvgDelete(image);
    }
}

void TicoOverlay::ResolveNotificationTextures() {
    if (!m_core) return;
    auto& notifications = m_core->m_raNotifications;
    if (notifications.empty()) return;

    for (auto& n : notifications) {
        if (n.textureId != 0) continue;
        if (n.badge_name.empty()) continue;

        if (n.badge_name == "ra_icon") {
            n.textureId = m_core->m_raIconTexture;
        } else {
            // Only check the in-memory GL texture cache — NO disk I/O, NO stbi_load.
            auto it = m_core->m_raBadgeCache.find(n.badge_name);
            if (it != m_core->m_raBadgeCache.end()) {
                n.textureId = it->second;
            }
            // If not in cache yet, leave textureId=0; the badge will appear
            // once ProcessPendingBadgeUploads uploads it (next RunFrame).
        }
    }
}

void TicoOverlay::Update(float deltaTime) {
    // Pre-load RA icon texture BEFORE rendering (GL-safe: outside ImGui frame)
    EnsureRAIconLoaded();

    // Resolve any pending notification badge textures from cache (no GL calls)
    ResolveNotificationTextures();

    if (m_currentMenu != OverlayMenu::None) {
        m_animTimer += deltaTime;
#ifdef __SWITCH__
        m_batteryTimer += deltaTime;
        if (m_batteryTimer >= 3.0f) {
            m_batteryTimer = 0.0f;
            psmGetBatteryChargePercentage(&m_batteryLevel);
            PsmChargerType chargerType; psmGetChargerType(&chargerType);
            m_isCharging = (chargerType != PsmChargerType_Unconnected);
        }
        float target = m_isCharging ? 1.0f : 0.0f;
        float diff = target - m_chargingStateProgress;
        if (std::abs(diff) > 0.001f) {
            m_chargingStateProgress += diff * deltaTime * 8.0f;
            m_chargingStateProgress = std::clamp(m_chargingStateProgress, 0.0f, 1.0f);
        }
#endif
    }
}

void TicoOverlay::Show() {
    if (m_currentMenu == OverlayMenu::None) {
        m_currentMenu = OverlayMenu::QuickMenu;
        m_animTimer = 0.0f; m_quickMenuSelection = 0;
        LoadConfig(); LoadGeneralConfig();
    }
}

void TicoOverlay::Hide() { m_currentMenu = OverlayMenu::None; }

void TicoOverlay::Render(ImVec2 displaySize, unsigned int gameTexture, float aspectRatio,
                         int frameWidth, int frameHeight, int fboWidth, int fboHeight) {
    ImDrawList *bgDrawList = ImGui::GetBackgroundDrawList();
    ImDrawList *fgDrawList = ImGui::GetForegroundDrawList();
    RenderGame(bgDrawList, displaySize, gameTexture, aspectRatio, frameWidth, frameHeight, fboWidth, fboHeight);
    if (m_currentMenu != OverlayMenu::None) {
        RenderOverlayBackground(fgDrawList, displaySize);
        RenderTitleCard(fgDrawList, displaySize);
        switch (m_currentMenu) {
        case OverlayMenu::QuickMenu: RenderQuickMenu(fgDrawList, displaySize); break;
        case OverlayMenu::SaveStates: RenderSaveStatesMenu(fgDrawList, displaySize); break;
        case OverlayMenu::Settings: RenderSettingsMenu(fgDrawList, displaySize); break;
        default: break;
        }
        RenderHelpersBar(fgDrawList, displaySize);
        RenderSocialArea(fgDrawList, displaySize);
        RenderStatusBar(fgDrawList, displaySize);
    }
    
    // RA alerts always render (even during gameplay, not just when menu is open)
    RenderRAAlerts(fgDrawList, displaySize, ImGui::GetIO().DeltaTime);
}

void TicoOverlay::RenderRAAlerts(ImDrawList *dl, ImVec2 displaySize, float deltaTime) {
    if (!m_core) return;
    auto& notifications = m_core->m_raNotifications;
    if (notifications.empty()) return;

    // NOTE: RA icon and badge textures are loaded in Update() / EnsureRAIconLoaded()
    //       BEFORE the ImGui frame. NO GL calls in this render method.

    float scale = ImGui::GetIO().FontGlobalScale;
    ImFont *font = ImGui::GetFont();
    ImFont *descFont = font;
    if (ImGui::GetIO().Fonts->Fonts.Size > 1) {
        descFont = ImGui::GetIO().Fonts->Fonts[1];
    }
    float descFontSize = ImGui::GetFontSize() * 0.65f;
    float titleFontSize = ImGui::GetFontSize() * 0.85f;

    // Alert dimensions
    float alertW = 420.0f * scale; // wider
    float alertH = 100.0f * scale; // taller
    float padding = 12.0f * scale;
    float margin = 16.0f * scale;
    float spacing = 8.0f * scale;
    float cornerRadius = 14.0f * scale;
    float badgeSize = 76.0f * scale; // fits padding perfectly (100 - 24 = 76)
    float badgeRadius = 4.0f * scale; // less roundness per RA spec
    float badgeMargin = 12.0f * scale;

    RAAlertPosition pos = m_core->m_raAlertPosition;
    bool isTop = (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
    bool isRight = (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

    // Update timers and remove expired
    for (auto& n : notifications) {
        n.timer += deltaTime;
    }
    notifications.erase(
        std::remove_if(notifications.begin(), notifications.end(),
            [](const RANotification& n) { return n.timer >= n.duration; }),
        notifications.end());

    // Render each notification
    for (size_t i = 0; i < notifications.size(); i++) {
        auto& n = notifications[i];

        // Badge textures are resolved in ResolveNotificationTextures() during Update().
        // No GL calls here — just skip rendering the badge area if texture isn't ready yet.

        // Calculate slide animation
        float slideProgress;
        if (n.timer < n.slideIn) {
            float t = n.timer / n.slideIn;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else if (n.timer > n.duration - n.slideOut) {
            float t = (n.duration - n.timer) / n.slideOut;
            slideProgress = 1.0f - std::pow(1.0f - t, 3.0f);
        } else {
            slideProgress = 1.0f;
        }

        // Calculate position
        float stackOffset = (float)i * (alertH + spacing);
        float anchorX = isRight ? (displaySize.x - alertW - margin) : margin;
        float anchorY = isTop ? (margin + stackOffset) : (displaySize.y - margin - alertH - stackOffset);
        float slideOffsetY = isTop
            ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
            : (alertH + margin + stackOffset) * (1.0f - slideProgress);

        float drawY = anchorY + slideOffsetY;
        int alpha = (int)(230 * slideProgress);
        if (alpha <= 0) continue;

        ImVec2 rectMin(anchorX, drawY);
        ImVec2 rectMax(anchorX + alertW, drawY + alertH);

        // Background — glassmorphic rounded rectangle
        ImU32 bgColor = m_isDarkMode
            ? IM_COL32(35, 35, 40, alpha)
            : IM_COL32(245, 248, 252, alpha);
        ImU32 borderColor = m_isDarkMode
            ? IM_COL32(70, 70, 80, (int)(180 * slideProgress))
            : IM_COL32(200, 205, 215, (int)(200 * slideProgress));

        dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
        dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

        // Badge image (left side)
        float textX = rectMin.x + padding;
        if (n.textureId != 0) {
            float badgeX = rectMin.x + badgeMargin;
            float badgeY = rectMin.y + (alertH - badgeSize) * 0.5f;

            float drawBadgeSize = badgeSize;
            float drawBadgeX = badgeX;
            float drawBadgeY = badgeY;

            // Make the general RA icon a bit smaller to fit visually better
            if (n.badge_name == "ra_icon") {
                drawBadgeSize = badgeSize * 0.70f;
                drawBadgeX += (badgeSize - drawBadgeSize) * 0.5f;
                drawBadgeY += (badgeSize - drawBadgeSize) * 0.5f;
            }

            ImVec2 bMin(drawBadgeX, drawBadgeY);
            ImVec2 bMax(drawBadgeX + drawBadgeSize, drawBadgeY + drawBadgeSize);
            ImU32 imgCol = IM_COL32(255, 255, 255, alpha);
            dl->AddImageRounded((ImTextureID)(uintptr_t)n.textureId,
                bMin, bMax, ImVec2(0,0), ImVec2(1,1), imgCol, badgeRadius);
            
            textX = badgeX + badgeSize + badgeMargin;
        }

        // Description text
        ImU32 descColor = m_isDarkMode
            ? IM_COL32(185, 185, 195, alpha)
            : IM_COL32(80, 80, 95, alpha);
        float maxDescW = rectMax.x - textX - padding;

        ImU32 titleColor = m_isDarkMode
            ? IM_COL32(255, 255, 255, alpha)
            : IM_COL32(30, 30, 40, alpha);

        std::string desc = n.description;
        float maxDescH = descFontSize * 2.5f; // height for roughly 2 lines
        ImVec2 fullSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        // If content goes through 2 lines, slice and add '...'
        if (fullSize.y > maxDescH) {
            desc += "...";
            while (desc.length() > 4) {
                ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
                if (testSize.y <= maxDescH) break;
                desc.erase(desc.length() - 4, 1);
            }
        }

        std::string titleStr = n.title;
        ImVec2 titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (titleSize.x > maxDescW) {
            titleStr += "...";
            while (titleStr.length() > 4) {
                ImVec2 testSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
                if (testSize.x <= maxDescW) break;
                titleStr.erase(titleStr.length() - 4, 1);
            }
            // Recalculate titleSize for accurate vertical centering
            titleSize = font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        }

        ImVec2 descSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());
        
        float textSpacing = 4.0f * scale;
        float totalTextH = titleSize.y + textSpacing + descSize.y;
        float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
        float descY = titleY + titleSize.y + textSpacing;

        dl->AddText(font, titleFontSize,
            ImVec2(textX + 1.0f, titleY + 1.0f),
            IM_COL32(0, 0, 0, (int)(80 * slideProgress)),
            titleStr.c_str());
        dl->AddText(font, titleFontSize,
            ImVec2(textX, titleY), titleColor, titleStr.c_str());

        dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor, desc.c_str(), nullptr, maxDescW);
    }
}

void TicoOverlay::RenderSocialArea(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    if (ease < 0.01f) return;
    float scale = ImGui::GetIO().FontGlobalScale;
    float AVATAR_SIZE = 72.0f * scale, sideMargin = 32.0f * scale, topMargin = 32.0f * scale, barHeight = 50.0f * scale;
    float startOffset = 200.0f, currentOffset = startOffset * (1.0f - ease);
    ImVec2 avatarCenter(sideMargin + AVATAR_SIZE * 0.5f - currentOffset, topMargin + barHeight * 0.5f);
    float radius = AVATAR_SIZE * 0.5f;
    ImU32 baseCol = m_isDarkMode ? IM_COL32(45,45,45,(int)(255*ease)) : IM_COL32(245,247,250,(int)(200*ease));
    dl->AddCircleFilled(avatarCenter, radius, baseCol);
    if (m_avatarTexture != 0) {
        float imgRadius = radius - 4.0f;
        dl->AddImageRounded((ImTextureID)(intptr_t)m_avatarTexture,
            ImVec2(avatarCenter.x-imgRadius, avatarCenter.y-imgRadius),
            ImVec2(avatarCenter.x+imgRadius, avatarCenter.y+imgRadius),
            ImVec2(0,0), ImVec2(1,1), IM_COL32_WHITE, imgRadius);
        dl->AddCircle(avatarCenter, imgRadius, IM_COL32(255,255,255,60), 0, 1.0f);
    } else {
        dl->AddCircleFilled(avatarCenter, radius - 4.0f, IM_COL32(200,200,210,255));
    }
}

void TicoOverlay::RenderGame(ImDrawList *dl, ImVec2 displaySize, unsigned int texture,
                             float aspectRatio, int width, int height, int fboWidth, int fboHeight) {
    if (texture == 0) return;
    static constexpr int N64_BASE_W = 320, N64_BASE_H = 240;
    float dstWidth = displaySize.x, dstHeight = displaySize.y, offsetX = 0, offsetY = 0;
    if (m_displayMode == Mupen64DisplayMode::Integer) {
        int scale;
        if (m_displaySize == Mupen64DisplaySize::Auto) {
            int scaleX = (int)displaySize.x / N64_BASE_W, scaleY = (int)displaySize.y / N64_BASE_H;
            scale = std::min(scaleX, scaleY); if (scale < 1) scale = 1;
        } else { scale = (int)m_displaySize - 3; if (scale < 1) scale = 1; }
        dstWidth = N64_BASE_W * scale; dstHeight = N64_BASE_H * scale;
        if (dstWidth > displaySize.x) dstWidth = displaySize.x;
        if (dstHeight > displaySize.y) dstHeight = displaySize.y;
    } else {
        switch (m_displaySize) {
        case Mupen64DisplaySize::Stretch: dstWidth = displaySize.x; dstHeight = displaySize.y; break;
        case Mupen64DisplaySize::_4_3: {
            float ar = 4.0f/3.0f, da = displaySize.x/displaySize.y;
            if (ar > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/ar; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*ar; } break;
        }
        case Mupen64DisplaySize::_16_9: {
            float ar = 16.0f/9.0f, da = displaySize.x/displaySize.y;
            if (ar > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/ar; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*ar; } break;
        }
        default: {
            float da = displaySize.x/displaySize.y;
            if (aspectRatio > da) { dstWidth = displaySize.x; dstHeight = displaySize.x/aspectRatio; }
            else { dstHeight = displaySize.y; dstWidth = displaySize.y*aspectRatio; } break;
        }}
    }
    offsetX = (displaySize.x - dstWidth) / 2.0f; offsetY = (displaySize.y - dstHeight) / 2.0f;
    dl->AddRectFilled(ImVec2(0,0), displaySize, IM_COL32(0,0,0,255));
    float u_max = (fboWidth > 0 && width > 0) ? (float)width/fboWidth : 1.0f;
    float v_max = (fboHeight > 0 && height > 0) ? (float)height/fboHeight : 1.0f;
#ifdef __SWITCH__
    dl->AddImage((ImTextureID)(intptr_t)texture, ImVec2(offsetX,offsetY), ImVec2(offsetX+dstWidth,offsetY+dstHeight), ImVec2(0,v_max), ImVec2(u_max,0));
#else
    dl->AddImage((ImTextureID)(intptr_t)texture, ImVec2(offsetX,offsetY), ImVec2(offsetX+dstWidth,offsetY+dstHeight), ImVec2(0,0), ImVec2(u_max,v_max));
#endif
}

void TicoOverlay::RenderOverlayBackground(ImDrawList *dl, ImVec2 displaySize) {
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f);
    int baseAlpha = (int)(200 * ease), maxAlpha = (int)(250 * ease);
    if (baseAlpha > 0) {
        float topH = displaySize.y * 0.20f, botH = displaySize.y * 0.20f;
        ImU32 colMax = IM_COL32(0,0,0,maxAlpha), colBase = IM_COL32(0,0,0,baseAlpha);
        dl->AddRectFilledMultiColor(ImVec2(0,0), ImVec2(displaySize.x,topH), colMax, colMax, colBase, colBase);
        dl->AddRectFilled(ImVec2(0,topH), ImVec2(displaySize.x,displaySize.y-botH), colBase);
        dl->AddRectFilledMultiColor(ImVec2(0,displaySize.y-botH), ImVec2(displaySize.x,displaySize.y), colBase, colBase, colMax, colMax);
    }
}

void TicoOverlay::RenderTitleCard(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    std::string titleStr = m_gameTitle;
    if (m_currentMenu == OverlayMenu::SaveStates) titleStr = m_isSaveMode ? tr("emulator_save_state") : tr("emulator_load_state");
    else if (m_currentMenu == OverlayMenu::Settings) titleStr = tr("emulator_settings");
    titleStr.erase(titleStr.find_last_not_of(" \n\r\t") + 1);
    if (titleStr.length() > 50) titleStr = titleStr.substr(0, 47) + "...";
    float scale = ImGui::GetIO().FontGlobalScale;
    float TITLE_HEIGHT = 72.0f * scale, AVAILABLE_TOP = 110.0f * scale;
    float cardWidth = displaySize.x * 0.4f, cardX = (displaySize.x - cardWidth) * 0.5f, cardY = (AVAILABLE_TOP - TITLE_HEIGHT) * 0.5f;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float startY = -150.0f * scale, currentY = startY + (cardY - startY) * easeOut;
    ImVec2 textSize = ImGui::CalcTextSize(titleStr.c_str());
    float textX = cardX + (cardWidth - textSize.x) * 0.5f, textY = currentY + (TITLE_HEIGHT - textSize.y) * 0.5f;
    UIStyle::DrawTextWithShadow(dl, ImVec2(textX, textY), IM_COL32(200,200,200,255), titleStr.c_str());
}

/// @brief Render an animated menu container with rounded corners
static void RenderMenuContainer(ImDrawList *dl, ImVec2 displaySize, float menuWidth, int numItems, float itemHeight, float animTimer, bool isDark,
                                ImVec2 &menuPos, ImVec2 &menuSize, float &easeOut, float &cornerRadius) {
    float scale = ImGui::GetIO().FontGlobalScale;
    menuSize = ImVec2(menuWidth, numItems * itemHeight);
    float t = std::min(animTimer / 0.4f, 1.0f);
    easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float targetY = (displaySize.y - menuSize.y) / 2.0f, startY = displaySize.y + 100.0f * scale;
    menuPos = ImVec2((displaySize.x - menuSize.x) / 2, startY + (targetY - startY) * easeOut);
    cornerRadius = 16.0f * scale;
    ImU32 containerColor = isDark ? IM_COL32(45,45,45,(int)(255*easeOut)) : IM_COL32(242,245,248,(int)(255*easeOut));
    dl->AddRectFilled(menuPos, ImVec2(menuPos.x + menuSize.x, menuPos.y + menuSize.y), containerColor, cornerRadius);
}

static void RenderMenuItem(ImDrawList *dl, ImVec2 menuPos, ImVec2 menuSize, int i, int numItems, float itemHeight,
                           bool isSelected, float cornerRadius, float easeOut, bool isDark, ImFont *font, float fontSize, const char *text) {
    float scale = ImGui::GetIO().FontGlobalScale;
    float itemY = menuPos.y + i * itemHeight;
    ImVec2 itemMin(menuPos.x, itemY), itemMax(menuPos.x + menuSize.x, itemY + itemHeight);
    if (isSelected) {
        ImDrawFlags corners = 0; float itemRadius = 0.0f;
        if (i == 0) { corners = ImDrawFlags_RoundCornersTop; itemRadius = cornerRadius; }
        else if (i == numItems - 1) { corners = ImDrawFlags_RoundCornersBottom; itemRadius = cornerRadius; }
        ImU32 selCol = isDark ? IM_COL32(60,60,60,(int)(255*easeOut)) : IM_COL32(190,195,205,(int)(255*easeOut));
        dl->AddRectFilled(itemMin, itemMax, selCol, itemRadius, corners);
    }
    ImU32 textColor;
    if (isDark) textColor = isSelected ? IM_COL32(255,255,255,(int)(255*easeOut)) : IM_COL32(200,200,200,(int)(255*easeOut));
    else textColor = isSelected ? IM_COL32(60,60,70,(int)(255*easeOut)) : IM_COL32(90,90,100,(int)(255*easeOut));
    ImVec2 sz = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, text);
    dl->AddText(font, fontSize, ImVec2(itemMin.x + 20.0f * scale, itemMin.y + (itemHeight - sz.y) / 2), textColor, text);
}

void TicoOverlay::RenderQuickMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    std::string items[] = {tr("emulator_save_state"), tr("emulator_load_state"), tr("emulator_settings"), tr("emulator_exit_to_system")};
    const int N = 4; float itemH = 64.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 320.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) RenderMenuItem(dl, menuPos, menuSize, i, N, itemH, m_quickMenuSelection==i, cornerRadius, easeOut, m_isDarkMode, font, fs, items[i].c_str());
}

void TicoOverlay::RenderSaveStatesMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int N = 4; float itemH = 64.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 320.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) {
        bool exists = false;
        if (m_core && m_core->IsGameLoaded()) { struct stat buffer; exists = (stat(GetStatePath(m_core, i).c_str(), &buffer) == 0); }
        char slotText[128];
        snprintf(slotText, sizeof(slotText), tr("emulator_slot").c_str(), i+1, exists ? tr("emulator_in_use").c_str() : tr("emulator_empty").c_str());
        RenderMenuItem(dl, menuPos, menuSize, i, N, itemH, m_saveStateSlot==i, cornerRadius, easeOut, m_isDarkMode, font, fs, slotText);
    }
}

void TicoOverlay::RenderSettingsMenu(ImDrawList *dl, ImVec2 displaySize) {
    float scale = ImGui::GetIO().FontGlobalScale;
    const int N = 2; float itemH = 64.0f * scale;
    ImVec2 menuPos, menuSize; float easeOut, cornerRadius;
    RenderMenuContainer(dl, displaySize, 320.0f*scale, N, itemH, m_animTimer, m_isDarkMode, menuPos, menuSize, easeOut, cornerRadius);
    ImFont *font = ImGui::GetFont(); float fs = ImGui::GetFontSize() * 0.85f;
    for (int i = 0; i < N; i++) {
        bool isSelected = (m_settingsSelection == i);
        float itemY = menuPos.y + i * itemH;
        ImVec2 itemMin(menuPos.x, itemY), itemMax(menuPos.x + menuSize.x, itemY + itemH);
        if (isSelected) {
            ImDrawFlags corners = 0; float itemRadius = 0.0f;
            if (i == 0) { corners = ImDrawFlags_RoundCornersTop; itemRadius = cornerRadius; }
            else if (i == N-1) { corners = ImDrawFlags_RoundCornersBottom; itemRadius = cornerRadius; }
            ImU32 selCol = m_isDarkMode ? IM_COL32(60,60,60,(int)(255*easeOut)) : IM_COL32(190,195,205,(int)(255*easeOut));
            dl->AddRectFilled(itemMin, itemMax, selCol, itemRadius, corners);
        }
        std::string label, value;
        if (i == 0) { label = tr("emulator_display_mode"); value = (m_displayMode == Mupen64DisplayMode::Integer) ? tr("emulator_integer") : tr("emulator_display"); }
        else {
            label = tr("emulator_size");
            if (m_displayMode == Mupen64DisplayMode::Integer) {
                switch (m_displaySize) { case Mupen64DisplaySize::_1x: value="1x"; break; case Mupen64DisplaySize::_2x: value="2x"; break; default: value=tr("emulator_auto"); break; }
            } else {
                switch (m_displaySize) { case Mupen64DisplaySize::Stretch: value=tr("emulator_stretch"); break; case Mupen64DisplaySize::_4_3: value="4:3"; break; case Mupen64DisplaySize::_16_9: value="16:9"; break; default: value=tr("emulator_original"); break; }
            }
        }
        ImU32 textColor;
        if (m_isDarkMode) textColor = isSelected ? IM_COL32(255,255,255,(int)(255*easeOut)) : IM_COL32(200,200,200,(int)(255*easeOut));
        else textColor = isSelected ? IM_COL32(60,60,70,(int)(255*easeOut)) : IM_COL32(90,90,100,(int)(255*easeOut));
        float textX = itemMin.x + 20.0f * scale;
        ImVec2 labelSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, label.c_str());
        dl->AddText(font, fs, ImVec2(textX, itemMin.y + (itemH - labelSize.y)/2), textColor, label.c_str());
        ImVec2 valueSize = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, value.c_str());
        float valueX = itemMax.x - valueSize.x - 40.0f * scale;
        dl->AddText(font, fs, ImVec2(valueX, itemMin.y + (itemH - labelSize.y)/2), textColor, value.c_str());
        if (isSelected) {
            float arrowSize = 12.0f * scale, arrowY = itemMin.y + (itemH - arrowSize)/2;
            float lx = valueX - arrowSize - 12.0f*scale;
            dl->AddTriangleFilled(ImVec2(lx,arrowY+arrowSize/2), ImVec2(lx+arrowSize,arrowY), ImVec2(lx+arrowSize,arrowY+arrowSize), textColor);
            float rx = valueX + valueSize.x + 12.0f*scale;
            dl->AddTriangleFilled(ImVec2(rx+arrowSize,arrowY+arrowSize/2), ImVec2(rx,arrowY), ImVec2(rx,arrowY+arrowSize), textColor);
        }
    }
}

void TicoOverlay::RenderHelpersBar(ImDrawList *dl, ImVec2 displaySize) {
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float easeOut = 1.0f - std::pow(1.0f - t, 3.0f);
    float scale = ImGui::GetIO().FontGlobalScale;
    float BAR_HEIGHT = 48.0f*scale, MARGIN_BOTTOM = 24.0f*scale, PADDING = 16.0f*scale, BUTTON_SIZE = 22.0f*scale, ITEM_SPACING = 12.0f*scale;
    ImFont *font = ImGui::GetFont(); float fontSize = ImGui::GetFontSize() * 0.78f;
    struct Helper { const char *btn; std::string desc; };
    std::vector<Helper> helpers;
    if (m_currentMenu == OverlayMenu::QuickMenu) helpers.push_back({"-", tr("emulator_reset")});
    else if (m_currentMenu == OverlayMenu::Settings) helpers.push_back({"DPAD", tr("emulator_change")});
    helpers.push_back({"B", tr("emulator_back")}); helpers.push_back({"A", tr("emulator_select")});
    float totalWidth = PADDING * 2;
    for (size_t i = 0; i < helpers.size(); i++) {
        totalWidth += BUTTON_SIZE + 8.0f*scale + font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, helpers[i].desc.c_str()).x;
        if (i < helpers.size()-1) totalWidth += ITEM_SPACING;
    }
    float startOffset = 400.0f*scale, currentOffset = startOffset*(1.0f-easeOut);
    float barX = displaySize.x - totalWidth - 20.0f + currentOffset, barY = displaySize.y - MARGIN_BOTTOM - BAR_HEIGHT;
    float cursorX = barX + PADDING, centerY = barY + BAR_HEIGHT * 0.5f;
    for (const auto &h : helpers) {
        UIStyle::DrawSwitchButton(dl, font, fontSize, ImVec2(cursorX+BUTTON_SIZE*0.5f, centerY), BUTTON_SIZE, h.btn, easeOut, true);
        cursorX += BUTTON_SIZE + 8.0f*scale;
        ImVec2 textSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, h.desc.c_str());
        dl->AddText(font, fontSize, ImVec2(cursorX, centerY - textSize.y*0.5f), IM_COL32(200,200,200,(int)(255*easeOut)), h.desc.c_str());
        cursorX += textSize.x + ITEM_SPACING;
    }
}

bool TicoOverlay::HandleInput(SDL_GameController *controller) {
    if (!controller) return false;
    uint32_t now = SDL_GetTicks(); bool debounced = (now - m_lastInputTime) > DEBOUNCE_MS;
    bool start = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
    bool select = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
    bool guide = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_GUIDE);
    bool togglePressed = guide || (start && select);
    if (togglePressed && !m_toggleHeld && debounced) {
        m_toggleHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::None) Show();
        else if (m_currentMenu == OverlayMenu::SaveStates || m_currentMenu == OverlayMenu::Settings) { m_currentMenu = OverlayMenu::QuickMenu; m_animTimer = 0.4f; }
        else Hide();
        return true;
    }
    if (!togglePressed) m_toggleHeld = false;
    if (m_currentMenu == OverlayMenu::None) return false;

    bool up = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_UP);
    bool down = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    bool left = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    bool right = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    bool confirm = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B);
    bool back = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
    Sint16 axisY = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
    Sint16 axisX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
    if (axisY < -16000) up = true; if (axisY > 16000) down = true;
    if (axisX < -16000) left = true; if (axisX > 16000) right = true;

    if (select && !start && debounced && m_currentMenu == OverlayMenu::QuickMenu) { m_shouldReset = true; Hide(); m_lastInputTime = now; return true; }

    if (up && !m_upHeld && debounced) {
        m_upHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) m_quickMenuSelection = (m_quickMenuSelection + 3) % 4;
        else if (m_currentMenu == OverlayMenu::SaveStates) m_saveStateSlot = (m_saveStateSlot + 3) % 4;
        else if (m_currentMenu == OverlayMenu::Settings) m_settingsSelection = (m_settingsSelection + 1) % 2;
    }
    if (!up) m_upHeld = false;
    if (down && !m_downHeld && debounced) {
        m_downHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) m_quickMenuSelection = (m_quickMenuSelection + 1) % 4;
        else if (m_currentMenu == OverlayMenu::SaveStates) m_saveStateSlot = (m_saveStateSlot + 1) % 4;
        else if (m_currentMenu == OverlayMenu::Settings) m_settingsSelection = (m_settingsSelection + 1) % 2;
    }
    if (!down) m_downHeld = false;

    bool dirChanged = false; int dir = 0;
    if (left && !m_leftHeld && debounced) { m_leftHeld = true; dir = -1; dirChanged = true; m_lastInputTime = now; }
    if (!left) m_leftHeld = false;
    if (right && !m_rightHeld && debounced) { m_rightHeld = true; dir = 1; dirChanged = true; m_lastInputTime = now; }
    if (!right) m_rightHeld = false;

    if (dirChanged && m_currentMenu == OverlayMenu::Settings) {
        if (m_settingsSelection == 0) {
            m_displayMode = (m_displayMode == Mupen64DisplayMode::Display) ? Mupen64DisplayMode::Integer : Mupen64DisplayMode::Display;
            m_displaySize = (m_displayMode == Mupen64DisplayMode::Integer) ? Mupen64DisplaySize::Auto : Mupen64DisplaySize::_4_3;
            ApplyScalingSettings(true);
        } else if (m_settingsSelection == 1) {
            if (m_displayMode == Mupen64DisplayMode::Integer) {
                int s = (int)m_displaySize + dir; if (s < 4) s = 6; if (s > 6) s = 4;
                m_displaySize = (Mupen64DisplaySize)s;
            } else {
                int s = (int)m_displaySize + dir; if (s < 0) s = 3; if (s > 3) s = 0;
                m_displaySize = (Mupen64DisplaySize)s;
            }
            ApplyScalingSettings(true);
        }
    }

    if (confirm && !m_confirmHeld && debounced) {
        m_confirmHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) {
            switch (m_quickMenuSelection) {
            case 0: m_isSaveMode = true; m_currentMenu = OverlayMenu::SaveStates; break;
            case 1: m_isSaveMode = false; m_currentMenu = OverlayMenu::SaveStates; break;
            case 2: m_currentMenu = OverlayMenu::Settings; m_settingsSelection = 0; break;
            case 3: m_shouldExitToSystem = true; break;
            }
        } else if (m_currentMenu == OverlayMenu::SaveStates) {
            if (m_core) {
                std::string sp = GetStatePath(m_core, m_saveStateSlot);
                if (m_isSaveMode) { m_core->SaveState(sp); m_currentMenu = OverlayMenu::QuickMenu; }
                else { m_core->LoadState(sp); Hide(); m_animTimer = 0.4f; return true; }
            } else m_currentMenu = OverlayMenu::QuickMenu;
        } else if (m_currentMenu == OverlayMenu::Settings) {
            if (m_settingsSelection == 0) {
                m_displayMode = (m_displayMode == Mupen64DisplayMode::Display) ? Mupen64DisplayMode::Integer : Mupen64DisplayMode::Display;
                m_displaySize = (m_displayMode == Mupen64DisplayMode::Integer) ? Mupen64DisplaySize::Auto : Mupen64DisplaySize::_4_3;
                ApplyScalingSettings(true);
            } else {
                if (m_displayMode == Mupen64DisplayMode::Integer) { int s = (int)m_displaySize; s = (s >= 6) ? 4 : s+1; m_displaySize = (Mupen64DisplaySize)s; }
                else { int s = (int)m_displaySize; s = (s >= 3) ? 0 : s+1; m_displaySize = (Mupen64DisplaySize)s; }
                ApplyScalingSettings(true);
            }
        }
    }
    if (!confirm) m_confirmHeld = false;

    if (back && !m_backHeld && debounced) {
        m_backHeld = true; m_lastInputTime = now;
        if (m_currentMenu == OverlayMenu::QuickMenu) Hide();
        else m_currentMenu = OverlayMenu::QuickMenu;
    }
    if (!back) m_backHeld = false;
    return true;
}

void TicoOverlay::LoadCoreSettings() {
#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/cores/mupen64plus.jsonc";
#else
    std::string configPath = "tico/config/cores/mupen64plus.jsonc";
#endif
    std::ifstream file(configPath);
    if (file.is_open()) {
        auto j = nlohmann::json::parse(file, nullptr, false, true); file.close();
        if (!j.is_discarded()) {
            if (j.contains("display_mode") && j["display_mode"].is_string()) {
                m_displayMode = (j["display_mode"].get<std::string>() == "Integer") ? Mupen64DisplayMode::Integer : Mupen64DisplayMode::Display;
            } else m_displayMode = Mupen64DisplayMode::Display;
            if (j.contains("display_size") && j["display_size"].is_string()) {
                std::string v = j["display_size"].get<std::string>();
                if (v=="Stretch") m_displaySize = Mupen64DisplaySize::Stretch;
                else if (v=="16:9") m_displaySize = Mupen64DisplaySize::_16_9;
                else if (v=="Original") m_displaySize = Mupen64DisplaySize::Original;
                else if (v=="1x") m_displaySize = Mupen64DisplaySize::_1x;
                else if (v=="2x") m_displaySize = Mupen64DisplaySize::_2x;
                else if (v=="Auto") m_displaySize = Mupen64DisplaySize::Auto;
                else m_displaySize = Mupen64DisplaySize::_4_3;
            } else m_displaySize = Mupen64DisplaySize::_4_3;
        } else { m_displayMode = Mupen64DisplayMode::Display; m_displaySize = Mupen64DisplaySize::_4_3; }
    } else { m_displayMode = Mupen64DisplayMode::Display; m_displaySize = Mupen64DisplaySize::_4_3; }
    ApplyScalingSettings(false);
}

void TicoOverlay::SaveCoreSettings() {
#ifdef __SWITCH__
    std::string configPath = "sdmc:/tico/config/cores/mupen64plus.jsonc";
#else
    std::string configPath = "tico/config/cores/mupen64plus.jsonc";
#endif
    nlohmann::json j = nlohmann::json::object();
    { std::ifstream in(configPath); if (in.is_open()) { auto p = nlohmann::json::parse(in, nullptr, false, true); in.close(); if (!p.is_discarded()) j = p; } }
    j["display_mode"] = (m_displayMode == Mupen64DisplayMode::Integer) ? "Integer" : "Display";
    const char *sizeStr = "4:3";
    switch (m_displaySize) {
    case Mupen64DisplaySize::Stretch: sizeStr="Stretch"; break; case Mupen64DisplaySize::_4_3: sizeStr="4:3"; break;
    case Mupen64DisplaySize::_16_9: sizeStr="16:9"; break; case Mupen64DisplaySize::Original: sizeStr="Original"; break;
    case Mupen64DisplaySize::_1x: sizeStr="1x"; break; case Mupen64DisplaySize::_2x: sizeStr="2x"; break;
    case Mupen64DisplaySize::Auto: sizeStr="Auto"; break; default: break;
    }
    j["display_size"] = sizeStr;
    std::ofstream out(configPath); if (out.is_open()) { out << j.dump(4); out.close(); }
}

void TicoOverlay::ApplyScalingSettings(bool save) { if (save) SaveCoreSettings(); }

void TicoOverlay::LoadSVGIcon() {
    const char *svgContent = R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 448 512"><path fill="#FFFFFF" d="M338.8-9.9c11.9 8.6 16.3 24.2 10.9 37.8L271.3 224 416 224c13.5 0 25.5 8.4 30.1 21.1s.7 26.9-9.6 35.5l-288 240c-11.3 9.4-27.4 9.9-39.3 1.3s-16.3-24.2-10.9-37.8L176.7 288 32 288c-13.5 0-25.5-8.4-30.1-21.1s-.7-26.9 9.6-35.5l288-240c11.3-9.4 27.4-9.9 39.3-1.3z"/></svg>)";
    char *input = strdup(svgContent); if (!input) return;
    NSVGimage *image = nsvgParse(input, "px", 96); free(input); if (!image) return;
    float sc = 64.0f / image->height; int w = (int)(image->width * sc), h = (int)(image->height * sc);
    m_boltWidth = w; m_boltHeight = h;
    NSVGrasterizer *rast = nsvgCreateRasterizer();
    if (!rast) { nsvgDelete(image); return; }
    unsigned char *img = (unsigned char *)malloc(w * h * 4);
    if (!img) { nsvgDeleteRasterizer(rast); nsvgDelete(image); return; }
    nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
    if (m_boltTexture != 0) glDeleteTextures(1, &m_boltTexture);
    glGenTextures(1, &m_boltTexture); glBindTexture(GL_TEXTURE_2D, m_boltTexture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img);
    glBindTexture(GL_TEXTURE_2D, 0);
    free(img); nsvgDeleteRasterizer(rast); nsvgDelete(image);
}

void TicoOverlay::RenderStatusBar(ImDrawList *dl, ImVec2 displaySize) {
    if (m_animTimer <= 0.0f) return;
    float t = std::min(m_animTimer / 0.4f, 1.0f);
    float ease = 1.0f - std::pow(1.0f - t, 3.0f), alpha = ease;
    float scale = ImGui::GetIO().FontGlobalScale;
    float BAR_HEIGHT = 50.0f*scale, TOP_MARGIN = 32.0f*scale, SIDE_MARGIN = 32.0f*scale, ITEM_SPACING = 20.0f*scale;
    ImFont *font = ImGui::GetFont(); float fontSize = ImGui::GetFontSize();
    std::time_t now = std::time(nullptr); std::tm *lt = std::localtime(&now);
    char timeStr[16], periodStr[16]; bool is24h = (m_hourFormat == "24h");
    float timeW = 0, periodFontSize = fontSize * 0.55f, periodW = 0;
    if (is24h) { std::strftime(timeStr, sizeof(timeStr), "%H:%M", lt); timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x; periodStr[0]=0; }
    else { std::strftime(timeStr, sizeof(timeStr), "%I:%M", lt); std::strftime(periodStr, sizeof(periodStr), "%p", lt); timeW = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0f, timeStr).x; periodW = font->CalcTextSizeA(periodFontSize, FLT_MAX, 0.0f, periodStr).x; }
    float totalWidth = timeW; if (!is24h) totalWidth += 4.0f + periodW;
    totalWidth += ITEM_SPACING + 34.0f*scale; float PADDING = 20.0f*scale; totalWidth += PADDING*2;
    float offsetY = (1.0f - ease) * -20.0f;
    float barX = displaySize.x - totalWidth - SIDE_MARGIN, barY = TOP_MARGIN + offsetY;
    ImU32 textColor = IM_COL32(200,200,200,(int)(255*alpha));
    float cursorX = barX + PADDING, centerY = barY + BAR_HEIGHT * 0.5f;
    dl->AddText(font, fontSize, ImVec2(cursorX, centerY - fontSize*0.5f), textColor, timeStr); cursorX += timeW;
    if (!is24h) { cursorX += 4.0f*scale; dl->AddText(font, periodFontSize, ImVec2(cursorX, centerY - fontSize*0.5f + (fontSize-periodFontSize)*0.9f), textColor, periodStr); cursorX += periodW; }
    cursorX += ITEM_SPACING;
    float bodyW = 32.0f*scale, bodyH = 20.0f*scale, tipW = 4.0f*scale, tipH = 10.0f*scale;
    ImVec2 batteryPos(cursorX, centerY - bodyH*0.5f);
    ImVec2 bodyMin = batteryPos, bodyMax = bodyMin + ImVec2(bodyW, bodyH);
    dl->AddRect(bodyMin, bodyMax, textColor, 3.0f, 0, 2.0f);
    ImVec2 tipMin(bodyMax.x, batteryPos.y + (bodyH-tipH)*0.5f), tipMax = tipMin + ImVec2(tipW, tipH);
    dl->AddRectFilled(tipMin, tipMax, textColor, 2.0f, ImDrawFlags_RoundCornersRight);
    float pct = std::clamp(m_batteryLevel / 100.0f, 0.0f, 1.0f);
    float pad = 4.0f*scale, fillMaxW = bodyW - pad*2, currentFillW = fillMaxW * pct;
    if (currentFillW < 2.0f*scale && pct > 0) currentFillW = 2.0f*scale;
    if (currentFillW > 0) dl->AddRectFilled(bodyMin + ImVec2(pad,pad), bodyMin + ImVec2(pad+currentFillW, bodyH-pad), textColor, 1.0f);
    if (m_isCharging) {
        if (m_boltTexture == 0) LoadSVGIcon();
        if (m_boltTexture != 0) {
            float iconH = 16.0f*scale, iconW = iconH * ((float)m_boltWidth / (float)m_boltHeight);
            ImVec2 iconPos(tipMax.x + 6.0f*scale, batteryPos.y + (bodyH - iconH)*0.5f);
            float fadeProgress = std::max(0.0f, (m_chargingStateProgress - 0.5f) * 2.0f);
            int alphaBolt = (int)(255 * fadeProgress * ease);
            if (alphaBolt > 0) dl->AddImage((ImTextureID)(intptr_t)m_boltTexture, iconPos, iconPos + ImVec2(iconW,iconH), ImVec2(0,0), ImVec2(1,1), IM_COL32(235,235,235,alphaBolt));
        }
    }
}
