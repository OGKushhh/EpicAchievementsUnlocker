#pragma once

namespace PipeServer {
    void Start();   // Call after achievements are loaded
    void Stop();    // Call from ScreamAPI::destroy()
    void NotifyUnlock(const char* achievementId);  // Call after each unlock
    void SendUpdatedList();  // Call from queryPlayerAchievementsComplete when refresh is complete
}
