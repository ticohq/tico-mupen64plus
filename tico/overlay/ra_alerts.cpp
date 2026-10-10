// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#include "overlay/ra_alerts.h"

#include "TicoCore.h"
#include "TicoRenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

// Nanosvg rasterizes the generic RA icon; badges come decoded from the core.
#define NANOSVG_IMPLEMENTATION
#include "deps/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "deps/nanosvg/nanosvgrast.h"

namespace SwitchFrontend::RAAlerts {

void Render(TicoCore *core, ImDrawList *dl, ImVec2 displaySize, float deltaTime) {
  if (!core)
    return;
  // achievements trigger on the emulation thread; they queue there
  core->CollectRANotifications();
  auto &notifications = core->m_raNotifications;
  if (notifications.empty())
    return;

  // Lazy-load RA icon from SVG if not loaded yet
  if (core->m_raIconTexture == ImTextureID_Invalid) {
    // Load ra.svg as texture using nanosvg (available in this TU)
    const char *svgPath = "romfs:/assets/ra.svg";
    NSVGimage *image = nsvgParseFromFile(svgPath, "px", 96);
    if (image) {
      float sc = 64.0f / image->height;
      int w = (int)(image->width * sc), h = (int)(image->height * sc);
      NSVGrasterizer *rast = nsvgCreateRasterizer();
      if (rast) {
        unsigned char *img = (unsigned char *)malloc(w * h * 4);
        if (img) {
          nsvgRasterize(rast, image, 0, 0, sc, img, w, h, w * 4);
          core->m_raIconTexture = TicoRenderer::CreateTextureRGBA(img, w, h);
          free(img);
        }
        nsvgDeleteRasterizer(rast);
      }
      nsvgDelete(image);
    }
  }

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
  float badgeSize = 76.0f * scale;  // fits padding perfectly (100 - 24 = 76)
  float badgeRadius = 4.0f * scale; // less roundness per RA spec
  float badgeMargin = 12.0f * scale;

  RAAlertPosition pos = core->m_raAlertPosition;
  bool isTop =
      (pos == RAAlertPosition::TopLeft || pos == RAAlertPosition::TopRight);
  bool isRight =
      (pos == RAAlertPosition::TopRight || pos == RAAlertPosition::BottomRight);

  // Update timers and remove expired
  for (auto &n : notifications) {
    n.timer += deltaTime;
  }
  notifications.erase(std::remove_if(notifications.begin(), notifications.end(),
                                     [](const RANotification &n) {
                                       return n.timer >= n.duration;
                                     }),
                      notifications.end());

  // Render each notification
  for (size_t i = 0; i < notifications.size(); i++) {
    auto &n = notifications[i];

    // Lazy-resolve badge texture (may have been downloaded after notification
    // was pushed)
    if (n.textureId == ImTextureID_Invalid && !n.badge_name.empty()) {
      if (n.badge_name == "ra_icon") {
        n.textureId = core->m_raIconTexture;
      } else {
        n.textureId = core->GetRABadgeTexture(n.badge_name);
        if (n.textureId == ImTextureID_Invalid) { // Not fetched by tico: placeholder
          n.badge_name = "ra_icon";
          n.textureId = core->m_raIconTexture;
        }
      }
    }

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
    float anchorY = isTop ? (margin + stackOffset)
                          : (displaySize.y - margin - alertH - stackOffset);
    float slideOffsetY =
        isTop ? -(alertH + margin + stackOffset) * (1.0f - slideProgress)
              : (alertH + margin + stackOffset) * (1.0f - slideProgress);

    float drawY = anchorY + slideOffsetY;
    int alpha = (int)(230 * slideProgress);
    if (alpha <= 0)
      continue;

    ImVec2 rectMin(anchorX, drawY);
    ImVec2 rectMax(anchorX + alertW, drawY + alertH);

    // Background — glassmorphic rounded rectangle
    ImU32 bgColor = IM_COL32(35, 35, 40, alpha);
    ImU32 borderColor =
        IM_COL32(70, 70, 80, (int)(180 * slideProgress));

    dl->AddRectFilled(rectMin, rectMax, bgColor, cornerRadius);
    dl->AddRect(rectMin, rectMax, borderColor, cornerRadius, 0, 1.5f * scale);

    // Badge image (left side)
    float textX = rectMin.x + padding;
    if (n.textureId != ImTextureID_Invalid) {
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
      dl->AddImageRounded(n.textureId, bMin, bMax,
                          ImVec2(0, 0), ImVec2(1, 1), imgCol, badgeRadius);

      textX = badgeX + badgeSize + badgeMargin;
    }

    // Description text
    ImU32 descColor = IM_COL32(185, 185, 195, alpha);
    float maxDescW = rectMax.x - textX - padding;

    ImU32 titleColor = IM_COL32(255, 255, 255, alpha);

    std::string desc = n.description;
    float maxDescH = descFontSize * 2.5f; // height for roughly 2 lines
    ImVec2 fullSize =
        descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());

    // If content goes through 2 lines, slice and add '...'
    if (fullSize.y > maxDescH) {
      desc += "...";
      while (desc.length() > 4) {
        ImVec2 testSize = descFont->CalcTextSizeA(descFontSize, FLT_MAX,
                                                  maxDescW, desc.c_str());
        if (testSize.y <= maxDescH)
          break;
        desc.erase(desc.length() - 4, 1);
      }
    }

    std::string titleStr = n.title;
    ImVec2 titleSize =
        font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
    if (titleSize.x > maxDescW) {
      titleStr += "...";
      while (titleStr.length() > 4) {
        ImVec2 testSize =
            font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
        if (testSize.x <= maxDescW)
          break;
        titleStr.erase(titleStr.length() - 4, 1);
      }
      // Recalculate titleSize for accurate vertical centering
      titleSize =
          font->CalcTextSizeA(titleFontSize, FLT_MAX, 0.0f, titleStr.c_str());
    }

    ImVec2 descSize =
        descFont->CalcTextSizeA(descFontSize, FLT_MAX, maxDescW, desc.c_str());

    float textSpacing = 4.0f * scale;
    float totalTextH = titleSize.y + textSpacing + descSize.y;
    float titleY = rectMin.y + (alertH - totalTextH) * 0.5f;
    float descY = titleY + titleSize.y + textSpacing;

    dl->AddText(font, titleFontSize, ImVec2(textX + 1.0f, titleY + 1.0f),
                IM_COL32(0, 0, 0, (int)(80 * slideProgress)), titleStr.c_str());
    dl->AddText(font, titleFontSize, ImVec2(textX, titleY), titleColor,
                titleStr.c_str());

    dl->AddText(descFont, descFontSize, ImVec2(textX, descY), descColor,
                desc.c_str(), nullptr, maxDescW);
  }
}

} // namespace SwitchFrontend::RAAlerts
