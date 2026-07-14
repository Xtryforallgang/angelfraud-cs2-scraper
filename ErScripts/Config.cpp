#include "Config.h"
#include <filesystem>

void Config::load(const std::string& filename) {
    if (!std::filesystem::exists("configs"))
        std::filesystem::create_directory("configs");

    std::ifstream file("configs\\" + filename + ".json");
    if (!file.is_open()) return;

    nlohmann::json json;
    file >> json;

    /* Overlay */
    read(json["overlay"]["fps-limiter"]["state"], fpsLimiterState);
    read(json["overlay"]["fps-limiter"]["fps"], fpsLimiter);
    read(json["overlay"]["vsync"], vsyncState);
    read(json["overlay"]["menu-bind"], overlayMenuBind);
    readArray(json["overlay"]["pos"], overlayPos);
    readArray(json["overlay"]["size"], overlaySize);

    /* Angelfraud */
    read(json["angelfraud"]["phpsessid"], angelfraudPhpsessid);
    read(json["angelfraud"]["cache-ttl"], angelfraudCacheTtl);
    read(json["angelfraud"]["site-url"], angelfraudSiteUrl);
    read(json["angelfraud"]["auto-extract"], angelfraudAutoExtractCookies);
}

void Config::save(const std::string& filename) const {
    nlohmann::ordered_json json;

    /* Overlay */
    json["overlay"]["fps-limiter"]["state"] = fpsLimiterState;
    json["overlay"]["fps-limiter"]["fps"] = fpsLimiter;
    json["overlay"]["vsync"] = vsyncState;
    json["overlay"]["menu-bind"] = overlayMenuBind;
    json["overlay"]["pos"] = overlayPos;
    json["overlay"]["size"] = overlaySize;

    /* Angelfraud */
    json["angelfraud"]["phpsessid"] = angelfraudPhpsessid;
    json["angelfraud"]["cache-ttl"] = angelfraudCacheTtl;
    json["angelfraud"]["site-url"] = angelfraudSiteUrl;
    json["angelfraud"]["auto-extract"] = angelfraudAutoExtractCookies;

    if (!std::filesystem::exists("configs"))
        std::filesystem::create_directory("configs");

    std::ofstream file("configs\\" + filename + ".json");
    if (file.is_open()) {
        file << json.dump(4);
    }
}

void Config::readArray(const nlohmann::json& src, float(&dest)[2]) {
    try {
        if (!src.is_null() && src.is_array() && src.size() == 2) {
            dest[0] = src[0].get<float>();
            dest[1] = src[1].get<float>();
        }
    }
    catch (const std::exception& e) {
        Logger::logWarning(std::format("Config Array Error: {}", e.what()));
    }
}
