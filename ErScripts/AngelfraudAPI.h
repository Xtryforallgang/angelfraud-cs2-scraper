#pragma once

#include <string>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <nlohmann/json.hpp>
#include "Globals.h"

struct PriceCacheEntry {
    double totalValue = 0.0;
    std::string status;                     // "ok", "private", "empty", "error", "banned"
    std::chrono::steady_clock::time_point fetchedAt;
    bool isError = false;
};

class AngelfraudAPI {
public:
    AngelfraudAPI();
    ~AngelfraudAPI();

    void start();
    void stop();

    // Fetch price for a SteamID (blocks, returns false on error)
    bool FetchPrice(const std::string& steamid, double& outValue, std::string& outStatus);

    // Fetch price with cache check
    bool GetPrice(const std::string& steamid, double& outValue, std::string& outStatus);

    // Clear expired cache entries
    void CleanupCache();

    // Get/set PHPSESSID
    void SetSessionId(const std::string& sid) { phpsessid = sid; }
    std::string GetSessionId() const { return phpsessid; }

    // Get/set cf_clearance (Cloudflare Turnstile token)
    void SetCfClearance(const std::string& cc) { cfClearance = cc; }
    std::string GetCfClearance() const { return cfClearance; }

    // ── Favorites API ──
    bool ToggleFavorite(const std::string& steamid, const std::string& name,
        const std::string& avatar, const std::string& value, const std::string& faceitNick);
    bool RemoveFavorite(int favId);
    bool SaveNote(int favId, const std::string& note);
    bool RefreshFavorites(std::vector<FavoriteEntry>& outFavorites);
    bool IsFavorite(const std::string& steamid);
    std::vector<FavoriteEntry> GetFavorites();

    // Background worker: periodically fetch prices for all match players
    void WorkerLoop();

private:
    std::string csrfToken;  // cached CSRF token
    std::vector<FavoriteEntry> favoritesCache;
    std::mutex favMutex;
    std::chrono::steady_clock::time_point favLastFetch;

    bool FetchCsrfToken();
    bool HttpGet(const std::string& path, std::string& outBody);
    bool HttpPostDirect(const std::string& path, const std::string& body, nlohmann::json& outJson);
    std::string phpsessid;
    std::string cfClearance;  // Cloudflare Turnstile clearance token
    std::unordered_map<std::string, PriceCacheEntry> cache;
    std::mutex cacheMutex;

    std::atomic<bool> running{ false };
    std::thread workerThread;

    // Make HTTP POST request to angelfraud (path = API endpoint, e.g. "/core")
    // fieldName is the form field name: "steam_input" for profile, "inventory_steamid" for inventory
    bool HttpPost(const std::string& steamid, nlohmann::json& outJson, const std::string& path = "/core", const std::string& fieldName = "steam_input");

    // Check if cache entry is still valid
    bool IsCacheValid(const PriceCacheEntry& entry);
};

// Free function: fetch profile data from panel API /profile
// Returns false on network error; out.loaded is set true only on successful parse
bool FetchAngelfraudProfile(const std::string& phpsessid, ProfileData& out);
