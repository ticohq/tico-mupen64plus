// Copyright 2026 Azahar Emulator Project
// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/imgui_overlay.h"
#include "../TicoSession.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "imgui.h"
#define STB_IMAGE_IMPLEMENTATION
#include "deps/stb/stb_image.h"
// nanosvg is implemented in ra_alerts.cpp
#include "deps/nanosvg/nanosvg.h"
#include "deps/nanosvg/nanosvgrast.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "TicoLogger.h"
#include "TicoRenderer.h"
#include "overlay/ra_alerts.h"
#include "overlay/tico_config.h"

namespace SwitchFrontend::ImGuiOverlay {
namespace {

constexpr std::array<const char*, 6> kAvatarPaths = {{
    "sdmc:/tico/assets/avatar.jpg",
    "sdmc:/tico/assets/avatar.jpeg",
    "sdmc:/tico/assets/avatar.png",
    "romfs:/assets/avatar.jpg",
    "romfs:/assets/avatar.jpeg",
    "romfs:/assets/avatar.png",
}};
// Selection border strips, as tico-nx ships them: index 0 and "original" (the
// last) use the cyan/violet strip, every other tint has its own. The slugs
// mirror tico-nx's TintPalette::GetBorderGradientFile.
constexpr const char* kBorderDir = "romfs:/assets/border/";
constexpr std::array<const char*, 13> kBorderSlugs = {{
    "default", "aqua", "violet", "sunset", "lime", "rose", "gold", "ice", "ember", "mint",
    "lagoon", "cobalt", "original",
}};

bool s_initialized = false;
bool s_visible = false;
bool s_psm_initialized = false;
OverlayUI::NavInput s_nav{};
OverlayUI::Action s_action = OverlayUI::Action::None;
ImTextureID s_avatar_texture = ImTextureID_Invalid;
ImTextureID s_border_texture = ImTextureID_Invalid;
// the quick menu sidebar's icons: Settings, Restart, Exit Game
std::array<ImTextureID, 3> s_side_icons = {ImTextureID_Invalid, ImTextureID_Invalid, ImTextureID_Invalid};

// Uploads an RGBA image decoded by stb_image and frees it. Returns the
// texture, or ImTextureID_Invalid when there was no image.
ImTextureID Upload(unsigned char* rgba, int width, int height, const char* source) {
    if (!rgba) {
        return ImTextureID_Invalid;
    }
    const ImTextureID texture = TicoRenderer::CreateTextureRGBA(rgba, width, height);
    stbi_image_free(rgba);
    if (texture != ImTextureID_Invalid) {
        LOG_INFO("OVERLAY", "loaded %s (%dx%d)", source, width, height);
    }
    return texture;
}

void LoadBorder() {
    const int tint = TicoConfig::BorderTint();
    const int original = static_cast<int>(kBorderSlugs.size()) - 1;
    std::string path = kBorderDir;
    if (tint <= 0 || tint >= original) {
        path += "border_gradient.png";
    } else {
        path += std::string("border_gradient_") + kBorderSlugs[static_cast<std::size_t>(tint)] +
                ".png";
    }
    int width = 0;
    int height = 0;
    int channels = 0;
    s_border_texture =
        Upload(stbi_load(path.c_str(), &width, &height, &channels, 4), width, height, path.c_str());
    if (s_border_texture == ImTextureID_Invalid) {
        LOG_WARN("OVERLAY", "no selection border strip at %s", path.c_str());
    }
    OverlayUI::SetBorderTextureId(static_cast<unsigned long long>(s_border_texture));
}

#ifdef __SWITCH__
bool LoadAvatarFromAccount() {
    if (R_FAILED(accountInitialize(AccountServiceType_Application))) {
        return false;
    }

    AccountUid uid{};
    bool found = tico::CurrentSession().AccountId(uid); // Who tico says plays
    if (!found) found = R_SUCCEEDED(accountGetPreselectedUser(&uid)) && accountUidIsValid(&uid);
    if (!found) {
        found = R_SUCCEEDED(accountGetLastOpenedUser(&uid)) && accountUidIsValid(&uid);
    }
    if (!found) {
        AccountUid uids[ACC_USER_LIST_SIZE]{};
        s32 total = 0;
        if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &total)) && total > 0) {
            uid = uids[0];
            found = accountUidIsValid(&uid);
        }
    }

    AccountProfile profile{};
    if (found && R_SUCCEEDED(accountGetProfile(&profile, uid))) {
        AccountProfileBase profile_base{};
        if (R_SUCCEEDED(accountProfileGet(&profile, nullptr, &profile_base)) &&
            profile_base.nickname[0] != '\0') {
            OverlayUI::SetNickname(profile_base.nickname);
        }
        u32 image_size = 0;
        if (R_SUCCEEDED(accountProfileGetImageSize(&profile, &image_size)) && image_size > 0) {
            std::vector<unsigned char> jpeg(image_size);
            u32 actual = 0;
            if (R_SUCCEEDED(accountProfileLoadImage(&profile, jpeg.data(), image_size, &actual)) &&
                actual > 0) {
                int width = 0;
                int height = 0;
                int channels = 0;
                s_avatar_texture =
                    Upload(stbi_load_from_memory(jpeg.data(), static_cast<int>(actual), &width,
                                                 &height, &channels, 4),
                           width, height, "switch-account-avatar");
            }
        }
        accountProfileClose(&profile);
    }
    accountExit();
    return s_avatar_texture != ImTextureID_Invalid;
}
#endif

void LoadAvatar() {
    for (const char* path : tico::AvatarCandidates(kAvatarPaths)) {
        int width = 0;
        int height = 0;
        int channels = 0;
        s_avatar_texture = Upload(stbi_load(path, &width, &height, &channels, 4), width, height, path);
        if (s_avatar_texture != ImTextureID_Invalid) {
            break;
        }
    }
#ifdef __SWITCH__
    if (s_avatar_texture == ImTextureID_Invalid && !LoadAvatarFromAccount()) {
        LOG_INFO("OVERLAY", "no avatar image found");
    }
#endif
    OverlayUI::SetAvatarTextureId(static_cast<unsigned long long>(s_avatar_texture));
}

} // namespace

// A white SVG icon rasterized to a size x size texture.
ImTextureID LoadSvgIcon(const std::string& path, int size) {
    NSVGimage* image = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!image) {
        LOG_WARN("OVERLAY", "no icon at %s", path.c_str());
        return ImTextureID_Invalid;
    }
    ImTextureID texture = ImTextureID_Invalid;
    if (NSVGrasterizer* rast = nsvgCreateRasterizer()) {
        std::vector<unsigned char> rgba(static_cast<std::size_t>(size) * size * 4, 0);
        const float longest = std::max(image->width, image->height);
        const float scale = longest > 0.0f ? size / longest : 1.0f;
        // centre the shorter side
        const float dx = (size - (image->width * scale)) * 0.5f;
        const float dy = (size - (image->height * scale)) * 0.5f;
        nsvgRasterize(rast, image, dx, dy, scale, rgba.data(), size, size, size * 4);
        nsvgDeleteRasterizer(rast);
        texture = TicoRenderer::CreateTextureRGBA(rgba.data(), size, size);
    }
    nsvgDelete(image);
    return texture;
}

void LoadSidebarIcons() {
#ifdef __SWITCH__
    const std::string dir = "romfs:/assets/icons/";
#else
    const std::string dir = "tico/assets/icons/";
#endif
    const char* names[] = {"gear.svg", "rotate-left.svg", "right-from-bracket.svg"};
    for (std::size_t i = 0; i < s_side_icons.size(); ++i) {
        s_side_icons[i] = LoadSvgIcon(dir + names[i], 64);
    }
    OverlayUI::SetSidebarIconTextures(static_cast<unsigned long long>(s_side_icons[0]),
                                      static_cast<unsigned long long>(s_side_icons[1]),
                                      static_cast<unsigned long long>(s_side_icons[2]));
}

bool Init() {
    if (s_initialized) {
        return true;
    }
#ifdef __SWITCH__
    if (!s_psm_initialized && R_SUCCEEDED(psmInitialize())) {
        s_psm_initialized = true;
    }
#endif
    LoadAvatar();
    LoadBorder();
    LoadSidebarIcons();
    s_visible = false;
    s_action = OverlayUI::Action::None;
    s_initialized = true;
    LOG_INFO("OVERLAY", "tico overlay initialized");
    return true;
}

void Shutdown() {
    if (!s_initialized) {
        return;
    }
    for (ImTextureID* texture : {&s_avatar_texture, &s_border_texture, &s_side_icons[0],
                                 &s_side_icons[1], &s_side_icons[2]}) {
        if (*texture != ImTextureID_Invalid) {
            TicoRenderer::DestroyTexture(*texture);
            *texture = ImTextureID_Invalid;
        }
    }
    OverlayUI::SetSidebarIconTextures(0, 0, 0);
    OverlayUI::SetAvatarTextureId(0);
    OverlayUI::SetBorderTextureId(0);
    OverlayUI::SetVisible(false);
    OverlayUI::ShowToast(std::string{});
#ifdef __SWITCH__
    if (s_psm_initialized) {
        psmExit();
        s_psm_initialized = false;
    }
#endif
    s_initialized = false;
}

void SetVisible(bool visible) {
    s_visible = visible;
    s_nav = {};
    OverlayUI::SetVisible(visible);
}

bool IsVisible() {
    return s_visible;
}

void FeedNav(const OverlayUI::NavInput& nav) {
    s_nav.up |= nav.up;
    s_nav.down |= nav.down;
    s_nav.left |= nav.left;
    s_nav.right |= nav.right;
    s_nav.accept |= nav.accept;
    s_nav.cancel |= nav.cancel;
}

void FeedTouch(const OverlayUI::TouchInput& touch) {
    OverlayUI::FeedTouch(touch);
}

void Draw(TicoCore* core, float width, float height, float delta_time) {
    if (!s_initialized) {
        return;
    }
    OverlayUI::FeedNav(s_nav);
    s_nav = {};
    const OverlayUI::Action action =
        OverlayUI::Render(static_cast<int>(width), static_cast<int>(height));
    RAAlerts::Render(core, ImGui::GetForegroundDrawList(), ImVec2(width, height), delta_time);
    if (action != OverlayUI::Action::None) {
        s_action = action;
    }
}

OverlayUI::Action ConsumeAction() {
    const OverlayUI::Action action = s_action;
    s_action = OverlayUI::Action::None;
    return action;
}

} // namespace SwitchFrontend::ImGuiOverlay
