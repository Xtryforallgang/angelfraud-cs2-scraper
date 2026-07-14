#include "GSIServer.h"
#include "Logger.h"
#include "Globals.h"
#include "Config.h"
#include <thread>
#include <algorithm>
#include <format>

GSIServer::GSIServer() {}

void GSIServer::gsiServer() {
    server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
        try {
            nlohmann::json jsonData = nlohmann::json::parse(req.body);
            handleJsonPayload(jsonData);
            res.set_content("OK", "text/plain");
        }
        catch (const nlohmann::json::exception& e) {
            Logger::logWarning(std::format("GSI JSON parse error: {}", e.what()));
            res.status = 400;
            res.set_content("Invalid JSON", "text/plain");
        }
        });

    Logger::logInfo(std::format("GSI server listening on 127.0.0.1:{}", port));
    if (!server.listen("127.0.0.1", port)) {
        Logger::logWarning(std::format("GSI server failed on 127.0.0.1:{}", port));
    }
}

void GSIServer::run() {
    std::thread serverThread(&GSIServer::gsiServer, this);
    serverThread.detach();
}

void GSIServer::stop() {
    Logger::logInfo("Stopping GSI server");
    server.stop();
}

void GSIServer::handleJsonPayload(const nlohmann::json& data) {
    // Track GSI activity timestamp (for overlay diagnostic)
    globals::gsiLastUpdate = std::chrono::steady_clock::now();

    // Get local player info and match state
    parsePlayerInfo(data);

    // Parse allplayers for match participants
    parseAllPlayers(data);

    // Track bomb countdown (for match state awareness)
    if (data.contains("round") && data["round"].contains("bomb")) {
        std::string bombState = data["round"]["bomb"].get<std::string>();
        if (bombState == "planted" && data.contains("phase_countdowns")
            && data["phase_countdowns"].contains("phase_ends_in")) {
            globals::matchTime = data["phase_countdowns"]["phase_ends_in"].get<double>();
        }
    }
}

void GSIServer::parsePlayerInfo(const nlohmann::json& data) {
    // Get local steamid
    if (data.contains("provider") && data["provider"].contains("steamid")) {
        globals::steamid = data["provider"]["steamid"].get<std::string>();
    }

    // Get player name from player object
    if (data.contains("player") && data["player"].contains("name")) {
        globals::nickname = data["player"]["name"].get<std::string>();
    }

    // Get map name
    if (data.contains("map") && data["map"].contains("name")) {
        globals::currentMap = data["map"]["name"].get<std::string>();
    }

    // Get player slot number
    if (data.contains("player") && data["player"].contains("observer_slot")) {
        globals::localPlayerSlotNumber = data["player"]["observer_slot"].get<int>();
    }
}

void GSIServer::parseAllPlayers(const nlohmann::json& data) {
    // If no allplayers data — player left the match, clear the table
    if (!data.contains("allplayers")) {
        std::lock_guard<std::mutex> lock(globals::playersMutex);
        if (!globals::matchPlayers.empty()) {
            globals::matchPlayers.clear();
            Logger::logInfo("GSI: Match ended, player list cleared");
        }
        return;
    }

    const auto& allplayers = data["allplayers"];
    if (!allplayers.is_object() || allplayers.empty()) {
        std::lock_guard<std::mutex> lock(globals::playersMutex);
        if (!globals::matchPlayers.empty()) {
            globals::matchPlayers.clear();
            Logger::logInfo("GSI: Match ended, player list cleared");
        }
        return;
    }

    std::vector<MatchPlayer> newPlayers;
    bool isNewMatch = false;

    // Track current match's player count
    static int prevPlayerCount = 0;

    for (auto it = allplayers.begin(); it != allplayers.end(); ++it) {
        const std::string& steamid = it.key();
        const auto& playerData = it.value();

        MatchPlayer mp;
        mp.steamid = steamid;

        // Get player name
        if (playerData.contains("name")) {
            mp.name = playerData["name"].get<std::string>();
        }

        // Get observer slot (used to identify which player is which)
        if (playerData.contains("observer_slot")) {
            mp.observerSlot = playerData["observer_slot"].get<int>();
        }

        // Get avatar
        if (playerData.contains("avatar")) {
            mp.avatar = playerData["avatar"].get<std::string>();
        }

        // Check if this is the local player
        mp.isLocalPlayer = (steamid == globals::steamid) ||
            (playerData.contains("observer_slot") &&
                playerData["observer_slot"].get<int>() == globals::localPlayerSlotNumber);

        // Preserve existing price data if player was already tracked
        {
            std::lock_guard<std::mutex> lock(globals::playersMutex);
            for (const auto& existing : globals::matchPlayers) {
                if (existing.steamid == steamid) {
                    mp.totalValue = existing.totalValue;
                    mp.valueLoaded = existing.valueLoaded;
                    mp.valueError = existing.valueError;
                    mp.valuePending = existing.valuePending;
                    mp.isFavorite = existing.isFavorite;
                    mp.valueStatus = existing.valueStatus;
                    mp.lastFetch = existing.lastFetch;
                    break;
                }
            }
        }

        newPlayers.push_back(std::move(mp));
    }

    // Detect match state changes
    int currentCount = (int)newPlayers.size();
    if (currentCount >= 2) {
        if (prevPlayerCount == 0 && currentCount > 0) {
            // New match started (was empty, now has players)
            isNewMatch = true;
            Logger::logInfo(std::format("GSI: New match detected! {} players", currentCount));
        }
        // Also detect if map changed
        static std::string lastMap;
        if (!globals::currentMap.empty() && globals::currentMap != lastMap) {
            if (!lastMap.empty()) isNewMatch = true;
            lastMap = globals::currentMap;
        }
    }
    prevPlayerCount = currentCount;

    // If new match, clear previous price data
    if (isNewMatch) {
        for (auto& p : newPlayers) {
            p.totalValue = 0;
            p.valueLoaded = false;
            p.valueError = false;
            p.valuePending = false;
        }
        Logger::logInfo("GSI: Price data cleared for new match");
    }

    // Update globals
    {
        std::lock_guard<std::mutex> lock(globals::playersMutex);
        globals::matchPlayers = std::move(newPlayers);
    }
}
