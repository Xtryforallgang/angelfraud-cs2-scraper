#pragma once

#include "Logger.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <memory>
#include <string>

class Config {
public:
    void load(const std::string& filename);
    void save(const std::string& filename) const;

    /* Overlay */
    bool fpsLimiterState{ true };
    int fpsLimiter{ 350 };
    bool vsyncState{ false };
    int overlayMenuBind{ VK_INSERT };

    /* Angelfraud */
    std::string angelfraudPhpsessid{ "" };
    int angelfraudCacheTtl{ 300 };        // seconds
    std::string angelfraudSiteUrl{ "angelfraud.steamcommunity.asia" };
    bool angelfraudAutoExtractCookies{ true };

    /* Window state */
    float overlayPos[2]{ 100.0f, 100.0f };
    float overlaySize[2]{ 400.0f, 300.0f };

private:
    template <typename T>
    void read(const nlohmann::json& src, T& dest) {
        try {
            if (!src.is_null()) {
                dest = src.get<T>();
            }
        }
        catch (const std::exception& e) {
            Logger::logWarning(std::format("Config Error Code: {}", e.what()));
        }
    }

    void readArray(const nlohmann::json& src, float(&dest)[2]);
};

inline std::unique_ptr<Config> cfg = std::make_unique<Config>();
