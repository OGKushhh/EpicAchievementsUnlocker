#pragma once
#include "pch.h"
#include "Overlay_types.h"
#include <mutex>

namespace AchievementManager {

void init();
void initWithOverlay(void* hModule);
void findAchievement(const char* achievementID, std::function<void(Overlay_Achievement&)> callback);
void unlockAchievement(Overlay_Achievement* achievement);
void refresh();   // manually refresh definitions and player achievements
std::mutex& GetAchievementsMutex();

}