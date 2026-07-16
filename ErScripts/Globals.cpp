#include "Globals.h"

namespace globals {
    int width = GetSystemMetrics(0), height = GetSystemMetrics(1), posX = 0, posY = 0;
    bool finish = false;
    bool menuState = true;

    std::string steamid = "", nickname = "";
    int cs2_ping = 0;
    int localPlayerSlotNumber = 0;
    int appMode = 0;                    // start in launcher mode

    std::vector<MatchPlayer> matchPlayers;
    std::mutex playersMutex;

    uint64_t matchId = 0;
    std::string currentMap = "";
    double matchTime = 0.0;
    bool allplayersActive = false;
    bool allplayersEverSeen = false;
    bool serverConnected = false;
    std::chrono::steady_clock::time_point gsiLastUpdate = {};
    std::atomic<bool> favNeedRefresh{ false };

    ProfileData profile;
    std::mutex profileMutex;
}

AngelfraudAPI* g_api = nullptr;
