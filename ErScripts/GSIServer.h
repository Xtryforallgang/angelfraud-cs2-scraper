#pragma once

#include <httplib/httplib.h>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>

class GSIServer {
private:
    const int port = 23561;
    httplib::Server server;

    void gsiServer();
    void handleJsonPayload(const nlohmann::json& data);
    void parseAllPlayers(const nlohmann::json& data);
    void parsePlayerInfo(const nlohmann::json& data);

    std::string handleSteamID(const nlohmann::json& data);
    int handleBombTime(const nlohmann::json& data);

public:
    GSIServer();
    void run();
    void stop();
};
