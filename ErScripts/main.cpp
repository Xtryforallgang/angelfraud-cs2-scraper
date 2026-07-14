#include "Overlay.h"
#include "GSIServer.h"
#include "Config.h"
#include "Globals.h"
#include "Logger.h"
#include "SteamTools.h"
#include "AngelfraudAPI.h"
#include "CookieExtractor.h"

// Windows subsystem — no console window
#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:mainCRTStartup")

#include <iostream>
#include <thread>
#include <format>
#include <filesystem>

namespace fs = std::filesystem;

#define APP_NAME "Angelfraud Scraper"

// ─── GSI Config ─────────────────────────────────────────────────────────────

const std::string GSI_CONFIG_CONTENT = R"("GSI Config for CS2 Inventory Overlay"
{
    "uri" "http://127.0.0.1:23561"
    "timeout" "5.0"
    "buffer"  "0.1"
    "throttle" "0.5"
    "heartbeat" "30.0"
    "data"
    {
        "provider" "1"
        "map" "1"
        "round" "1"
        "player_id" "1"
        "player_state" "1"
        "player_weapons" "1"
        "player_match_stats" "1"
        "allplayers_id" "1"
        "allplayers_match_stats" "1"
        "allplayers_state" "1"
        "allplayers_weapons" "1"
        "allplayers_position" "1"
        "phase_countdowns" "1"
        "bomb" "1"
    }
})";

// ─── CS2 Path Finding ───────────────────────────────────────────────────────

struct Cs2Paths {
    std::wstring installPath;
    std::wstring gsiConfigPath;
    HWND windowHandle = nullptr;
    int width = 0, height = 0, posX = 0, posY = 0;
};

Cs2Paths FindCs2() {
    Cs2Paths result;

    SteamTools::GameConfig cs2Config{
        .possibleFolders = { L"Counter-Strike Global Offensive" }
    };

    auto cs2Path = SteamTools::getAppInstallPath("730", cs2Config);
    if (!cs2Path) {
        Logger::logWarning("Could not find CS2 installation path");
        return result;
    }

    result.installPath = *cs2Path;
    result.gsiConfigPath = *cs2Path + L"/game/csgo/cfg/gamestate_integration_erscripts.cfg";

    // Find CS2 window
    result.windowHandle = FindWindowA(nullptr, "Counter-Strike 2");
    if (result.windowHandle) {
        RECT rect;
        if (GetClientRect(result.windowHandle, &rect)) {
            result.width = rect.right - rect.left;
            result.height = rect.bottom - rect.top;
            globals::width = result.width;
            globals::height = result.height;
            POINT pt = { 0, 0 };
            if (ClientToScreen(result.windowHandle, &pt)) {
                result.posX = globals::posX = pt.x;
                result.posY = globals::posY = pt.y;
            }
        }
    }

    return result;
}

bool EnsureGSIConfig(const std::wstring& gsiConfigPath) {
    if (!fs::exists(gsiConfigPath)) {
        std::ofstream file(gsiConfigPath);
        if (file.is_open()) {
            file << GSI_CONFIG_CONTENT;
            file.close();
            Logger::logSuccess(std::format(L"Created GSI config: {}", gsiConfigPath));
            return true;
        }
        else {
            Logger::logWarning(std::format(L"Failed to create GSI config: {}", gsiConfigPath));
            return false;
        }
    }
    Logger::logInfo("GSI config exists");
    return true;
}

// ─── Main ───────────────────────────────────────────────────────────────────

int main() {
    Logger::EnableANSIColors();

    // Single instance check
    CreateMutexA(0, FALSE, "Local\\csinventory_overlay");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxA(NULL, "CS2 Inventory Overlay is already running!", 0, MB_OK);
        return -1;
    }

    SetConsoleTitleA(APP_NAME);

    std::cout << "[>] CS2 Inventory Overlay" << std::endl;
    std::cout << "[>] Shows inventory prices of CS2 match players" << std::endl;
    std::cout << "[>] Panel API" << std::endl;
    std::cout << std::endl;

    // Load config
    cfg->load("default");

    // Find CS2
    Logger::logInfo("Locating CS2...");
    Cs2Paths cs2 = FindCs2();

    if (cs2.installPath.empty()) {
        Logger::logWarning("CS2 not found. Waiting for CS2 to start...");
        // Poll for CS2
        while (cs2.installPath.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            cs2 = FindCs2();
        }
        // CS2 just started — config will be written below BEFORE it fully loads
    }
    Logger::logSuccess(std::format(L"CS2 path: {}", cs2.installPath));

    // ── Write GSI config as early as possible ──
    // This MUST happen BEFORE waiting for the CS2 window, because if CS2 is
    // already running it has already booted without GSI integration.
    // If the config was just created, notify the user to restart CS2.
    bool gsiConfigExisted = fs::exists(cs2.gsiConfigPath);
    EnsureGSIConfig(cs2.gsiConfigPath);
    if (!gsiConfigExisted && cs2.windowHandle) {
        // CS2 was already running when we wrote the config — it won't pick it up
        Logger::logWarning("GSI config was missing — written now.");
        Logger::logWarning("CS2 is already running. RESTART CS2 for GSI to take effect.");
        Logger::logWarning("After restarting, players will load automatically in matches.");
    }

    if (!cs2.windowHandle) {
        Logger::logInfo("Waiting for CS2 window...");
        while (!(cs2.windowHandle = FindWindowA(nullptr, "Counter-Strike 2"))) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        RECT rect;
        if (GetClientRect(cs2.windowHandle, &rect)) {
            cs2.width = globals::width = rect.right - rect.left;
            cs2.height = globals::height = rect.bottom - rect.top;
            POINT pt = { 0, 0 };
            if (ClientToScreen(cs2.windowHandle, &pt)) {
                cs2.posX = globals::posX = pt.x;
                cs2.posY = globals::posY = pt.y;
            }
        }
    }
    Logger::logSuccess("CS2 window found");

    // Create a global AngelfraudAPI instance
    auto angelfraud = std::make_shared<AngelfraudAPI>();
    g_api = angelfraud.get();

    // Start overlay
    Overlay overlay;
    overlay.run();

    // Start GSI server
    GSIServer gsi;
    gsi.run();

    // Auto-extract PHPSESSID from browser (always try for a fresh session)
    if (cfg->angelfraudAutoExtractCookies) {
        Logger::logInfo("Attempting to auto-extract PHPSESSID from browser...");
        std::string phpsessid = CookieExtractor::Extract();
        if (!phpsessid.empty()) {
            Logger::logSuccess("PHPSESSID extracted successfully!");
            cfg->angelfraudPhpsessid = phpsessid;
            cfg->save("default");
        }
        else if (cfg->angelfraudPhpsessid.empty()) {
            Logger::logWarning("Could not auto-extract PHPSESSID.");
            Logger::logWarning("To set it manually, edit configs/default.json and add angelfraud.phpsessid");
        }
        else {
            Logger::logInfo("Using previously saved PHPSESSID (extraction failed, config value may be stale)");
        }
    }

    // Set PHPSESSID and cf_clearance, then start price fetcher
    angelfraud->SetSessionId(cfg->angelfraudPhpsessid);
    angelfraud->SetCfClearance(CookieExtractor::GetCfClearance());
    overlay.SetPhpsessid(cfg->angelfraudPhpsessid);
    angelfraud->start();

    // Initial profile fetch
    if (!cfg->angelfraudPhpsessid.empty()) {
        Logger::logInfo("Fetching profile data...");
        ProfileData pd;
        if (FetchAngelfraudProfile(cfg->angelfraudPhpsessid, pd)) {
            std::lock_guard<std::mutex> lock(globals::profileMutex);
            globals::profile = pd;
        }
    }

    Logger::logInfo("Ready! Click START in the launcher, or press END to exit.");

    // Main loop — launcher mode then overlay mode
    while (!globals::finish) {
        if (GetAsyncKeyState(VK_END) & 0x8000) {
            globals::finish = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // Cleanup
    angelfraud->stop();
    gsi.stop();

    Logger::logInfo("Shutdown complete");
    return 0;
}
