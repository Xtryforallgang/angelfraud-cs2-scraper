#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <chrono>
#include <atomic>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

struct MatchPlayer {
    std::string steamid;
    std::string name;
    std::string avatar;         // steam avatar URL
    int observerSlot = -1;
    bool isLocalPlayer = false;

    // Price data
    double totalValue = 0.0;
    bool valueLoaded = false;   // true when we got a response
    bool valueError = false;    // true if API returned error
    bool valuePending = false;  // true while waiting for API
    std::string valueStatus;    // "private", "empty", "banned", error msg
    std::chrono::steady_clock::time_point lastFetch;

    // Favorites
    bool isFavorite = false;    // cached from server
};

struct FavoriteEntry {
    int favId = 0;
    std::string steamid;
    std::string name;
    std::string avatar;
    std::string value;          // "$45.03" or "PRIVATE"
    std::string faceitNick;
    std::string note;           // saved note
    bool loaded = false;
};

struct ProfileData {
    std::string nickname;
    int workerId = 0;
    double grossProfit = 0.0;       // Грязный профит
    double yourShare = 0.0;         // Ваша доля
    double paid = 0.0;              // Выплачено
    double debt = 0.0;              // Долг
    int currentSharePercent = 50;   // текущий %
    int targetSharePercent = 60;    // целевой %
    bool loaded = false;
    std::chrono::steady_clock::time_point lastFetch;
};

namespace globals {
    extern int width, height, posX, posY;
    extern bool finish;
    extern bool menuState;

    extern std::string steamid, nickname;
    extern int cs2_ping;
    extern int localPlayerSlotNumber;   // needed to identify self in allplayers
    extern int appMode;                 // 0 = launcher, 1 = overlay running

    extern std::vector<MatchPlayer> matchPlayers;
    extern std::mutex playersMutex;

    extern uint64_t matchId;            // to detect new match
    extern std::string currentMap;
    extern double matchTime;            // round time
    extern bool allplayersActive;       // true when allplayers data is being received currently
    extern bool allplayersEverSeen;     // true if allplayers was ever received this session
    extern bool serverConnected;        // true when provider + map data indicates we're on a server
    extern std::chrono::steady_clock::time_point gsiLastUpdate;  // last GSI payload received
    extern std::atomic<bool> favNeedRefresh;                     // signal favorites window to refresh

    // Profile data from panel API /profile
    extern ProfileData profile;
    extern std::mutex profileMutex;
}

class AngelfraudAPI;
extern AngelfraudAPI* g_api;
