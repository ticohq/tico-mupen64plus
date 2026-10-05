// Copyright 2026 Dan | ticoverse.com
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "imgui.h"

class TicoCore;

// RetroAchievements toasts (session start, unlocks, leaderboards), drawn over
// the game and the menu alike. The core owns the queue and the badge textures.
namespace SwitchFrontend::RAAlerts {

// Advances and draws the queued toasts, uploading the generic RA icon
// (romfs:/assets/ra.svg) the first time one needs it.
void Render(TicoCore* core, ImDrawList* dl, ImVec2 display_size, float delta_time);

} // namespace SwitchFrontend::RAAlerts
