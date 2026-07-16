#include "AngelfraudAPI.h"
#include "Logger.h"
#include "Config.h"
#include "Globals.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <format>
#include <cctype>

#pragma comment(lib, "winhttp.lib")

AngelfraudAPI::AngelfraudAPI() {}

AngelfraudAPI::~AngelfraudAPI() {
    stop();
}

void AngelfraudAPI::start() {
    phpsessid = cfg->angelfraudPhpsessid;

    if (phpsessid.empty()) {
        Logger::logWarning("AngelfraudAPI: No PHPSESSID configured. Auto-extracting...");
        // PHPSESSID extraction happens later in main
    }

    running = true;
    workerThread = std::thread(&AngelfraudAPI::WorkerLoop, this);
    Logger::logInfo("AngelfraudAPI: Started background worker");
}

void AngelfraudAPI::stop() {
    running = false;
    if (workerThread.joinable()) {
        workerThread.join();
    }
    Logger::logInfo("AngelfraudAPI: Stopped");
}

bool AngelfraudAPI::IsCacheValid(const PriceCacheEntry& entry) {
    auto elapsed = std::chrono::steady_clock::now() - entry.fetchedAt;
    return elapsed < std::chrono::seconds(cfg->angelfraudCacheTtl);
}

bool AngelfraudAPI::GetPrice(const std::string& steamid, double& outValue, std::string& outStatus) {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto it = cache.find(steamid);
    if (it != cache.end() && IsCacheValid(it->second)) {
        if (!it->second.isError) {
            outValue = it->second.totalValue;
            outStatus = it->second.status;
            return true;
        }
    }
    return false;
}

bool AngelfraudAPI::FetchPrice(const std::string& steamid, double& outValue, std::string& outStatus) {
    // Check cache first
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        auto it = cache.find(steamid);
        if (it != cache.end() && IsCacheValid(it->second)) {
            if (!it->second.isError) {
                outValue = it->second.totalValue;
                outStatus = it->second.status;
                return true;
            }
        }
    }

    nlohmann::json response;

    // ── Step 1: Try /core with inventory_steamid (browser-equivalent POST) ──
    bool gotInventoryData = false;
    if (HttpPost(steamid, response, "/core", "inventory_steamid")) {
        if (response.contains("total_value") || response.contains("inv_status")) {
            gotInventoryData = true;
            Logger::logInfo("AngelfraudAPI: Got inventory data from /core with inventory_steamid");
        } else {
            Logger::logInfo(std::format("AngelfraudAPI: /core with inventory_steamid returned {} keys, no price data",
                response.size()));
        }
    } else {
        Logger::logInfo("AngelfraudAPI: /core with inventory_steamid failed");
    }

    // ── Step 2: Fallback — try other paths with steam_input ──
    if (!gotInventoryData) {
        static const std::vector<std::string> fallbackPaths = {"/api/prices", "/api/inventory", "/api/price"};
        for (const auto& p : fallbackPaths) {
            if (!HttpPost(steamid, response, p, "steam_input")) {
                Logger::logInfo(std::format("AngelfraudAPI: Fallback path {} failed", p));
                continue;
            }
            if (response.contains("total_value") || response.contains("inv_status")) {
                gotInventoryData = true;
                Logger::logInfo(std::format("AngelfraudAPI: Found inventory data at fallback path {}", p));
                break;
            }
            Logger::logInfo(std::format("AngelfraudAPI: Fallback path {} returned {} keys, no price data",
                p, response.size()));
        }
    }

    // ── Step 3: No data from any path ──
    if (!gotInventoryData) {
        Logger::logWarning(std::format("AngelfraudAPI: No path returned inventory data for {}", steamid));

        PriceCacheEntry errorEntry;
        errorEntry.isError = true;
        errorEntry.status = "no_prices";
        errorEntry.fetchedAt = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[steamid] = errorEntry;

        outStatus = "No price endpoint";
        return false;
    }

    // ── Step 4: Parse response ──
    PriceCacheEntry entry;
    entry.fetchedAt = std::chrono::steady_clock::now();

    if (response.contains("error")) {
        std::string errorMsg = response["error"].get<std::string>();
        Logger::logWarning(std::format("AngelfraudAPI: Error for {}: {}", steamid, errorMsg));
        entry.isError = true;
        entry.status = errorMsg;
        outStatus = errorMsg;
    }
    else if (response.contains("total_value")) {
        std::string totalVal = response["total_value"].get<std::string>();
        // Parse "$123.45" format
        std::string clean;
        for (char c : totalVal) {
            if (isdigit(c) || c == '.') clean += c;
        }
        if (!clean.empty()) {
            entry.totalValue = std::stod(clean);
            // Apply the site's 0.65 multiplier for consistency
            entry.totalValue *= 0.65;
        }
        entry.status = "ok";
        outValue = entry.totalValue;
        outStatus = "ok";

        Logger::logInfo(std::format("AngelfraudAPI: {} price = ${:.2f}", steamid, entry.totalValue));
    }
    else if (response.contains("inv_status")) {
        std::string invStatus = response["inv_status"].get<std::string>();
        entry.status = invStatus;
        entry.isError = (invStatus == "error");

        if (invStatus == "private") outStatus = "PRIVATE";
        else if (invStatus == "empty") outStatus = "ПУСТОЙ";
        else if (invStatus == "banned") outStatus = "ЗАБАНЕН";
        else outStatus = "ошибка";

        Logger::logInfo(std::format("AngelfraudAPI: {} inventory status = {}", steamid, invStatus));
    }
    else {
        // Log unexpected response format for debugging
        Logger::logInfo(std::format("AngelfraudAPI: Unexpected response keys for {}:", steamid));
        for (auto it = response.begin(); it != response.end(); ++it) {
            std::string valStr;
            if (it.value().is_string()) valStr = it.value().get<std::string>();
            else if (it.value().is_number()) valStr = std::to_string(it.value().get<double>());
            else valStr = it.value().type_name();
            Logger::logInfo(std::format("  {}: {}", it.key(), valStr));
            // Recursively check nested objects for price-related fields
            if (it.value().is_object()) {
                for (auto it2 = it.value().begin(); it2 != it.value().end(); ++it2) {
                    std::string sk = it2.key();
                    std::transform(sk.begin(), sk.end(), sk.begin(), ::tolower);
                    if (sk.find("total") != std::string::npos || sk.find("price") != std::string::npos ||
                        sk.find("value") != std::string::npos || sk.find("invent") != std::string::npos ||
                        sk.find("cost") != std::string::npos || sk.find("sum") != std::string::npos) {
                        Logger::logInfo(std::format("  Found potential price field: {}.{} = {}",
                            it.key(), it2.key(), it2.value().dump()));
                    }
                }
            }
            if (it.value().is_array() && !it.value().empty() && it.value()[0].is_object()) {
                for (auto it2 = it.value()[0].begin(); it2 != it.value()[0].end(); ++it2) {
                    std::string sk = it2.key();
                    std::transform(sk.begin(), sk.end(), sk.begin(), ::tolower);
                    if (sk.find("total") != std::string::npos || sk.find("price") != std::string::npos ||
                        sk.find("value") != std::string::npos || sk.find("invent") != std::string::npos ||
                        sk.find("cost") != std::string::npos || sk.find("sum") != std::string::npos) {
                        Logger::logInfo(std::format("  Found potential price field in array: {}[0].{} = {}",
                            it.key(), it2.key(), it2.value().dump()));
                    }
                }
            }
        }
        Logger::logInfo(std::format("AngelfraudAPI: Full dump: {}", response.dump()));

        entry.isError = true;
        entry.status = "unknown";
        outStatus = "Unknown response";
    }

    // Store in cache
    {
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache[steamid] = entry;
    }

    return !entry.isError;
}

void AngelfraudAPI::CleanupCache() {
    std::lock_guard<std::mutex> lock(cacheMutex);
    auto now = std::chrono::steady_clock::now();
    for (auto it = cache.begin(); it != cache.end();) {
        if (now - it->second.fetchedAt > std::chrono::seconds(cfg->angelfraudCacheTtl * 2)) {
            it = cache.erase(it);
        }
        else {
            ++it;
        }
    }
}

// ─── CSRF Token parsing ───────────────────────────────────────────────────────

bool AngelfraudAPI::FetchCsrfToken() {
    std::string html;
    if (!HttpGet("/profile", html)) return false;

    // Try multiple patterns for CSRF token
    struct Pattern { const char* search; size_t skip; };
    Pattern patterns[] = {
        { R"(name="csrf_token"\s+value=")", 0 },
        { R"(csrf_token" value=")", 0 },
        { R"(csrf_token=)", 0 },
        { R"(name="_token"\s+value=")", 0 },
        { R"("csrf-token"\s+content=")", 0 },
    };

    for (auto& p : patterns) {
        size_t pos = html.find(p.search);
        if (pos != std::string::npos) {
            pos += strlen(p.search);
            if (pos < html.size() && html[pos] == '"') pos++; // skip opening quote
            size_t end = html.find("\"", pos);
            if (end != std::string::npos) {
                csrfToken = html.substr(pos, end - pos);
                if (!csrfToken.empty() && csrfToken.find('<') == std::string::npos) {
                    Logger::logInfo(std::format("AngelfraudAPI: CSRF token = {}", csrfToken));
                    return true;
                }
            }
        }
    }

    Logger::logWarning("AngelfraudAPI: CSRF token not found in /profile");
    return false;
}

bool AngelfraudAPI::HttpGet(const std::string& path, std::string& outBody) {
    std::string host = cfg->angelfraudSiteUrl;
    if (host.find("https://") == 0) host = host.substr(8);
    else if (host.find("http://") == 0) host = host.substr(7);
    if (!host.empty() && host.back() == '/') host.pop_back();

    HINTERNET hSession = WinHttpOpen(L"AngelfraudAPI/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    int hostLen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    std::wstring whost(hostLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &whost[0], hostLen);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET",
        std::wstring(path.begin(), path.end()).c_str(),
        NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    std::string cookieHeader = "Cookie: PHPSESSID=" + phpsessid;
    if (!cfClearance.empty()) cookieHeader += "; cf_clearance=" + cfClearance;
    std::wstring wCookie(cookieHeader.begin(), cookieHeader.end());
    WinHttpAddRequestHeaders(hRequest, wCookie.c_str(), wCookie.size(), WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring ua = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0";
    WinHttpAddRequestHeaders(hRequest, ua.c_str(), ua.size(), WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);
    if (!sent) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    BOOL received = WinHttpReceiveResponse(hRequest, NULL);
    if (!received) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    outBody.clear();
    DWORD bytesRead = 0;
    char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        outBody += buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return !outBody.empty();
}

// Helper: URL-encode a string for application/x-www-form-urlencoded
static std::string UrlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else if (c == ' ') {
            out += '+';
        } else {
            out += std::format("%{:02X}", c);
        }
    }
    return out;
}

// ─── Favorites API ────────────────────────────────────────────────────────────

bool AngelfraudAPI::ToggleFavorite(const std::string& steamid, const std::string& name,
    const std::string& avatar, const std::string& value, const std::string& faceitNick)
{
    if (phpsessid.empty()) return false;

    // /api endpoint is a REST-like API, doesn't need CSRF token
    // Build form data (same as site's toggleFav) — URL-encode all values
    std::string body = "steamid=" + UrlEncode(steamid);
    body += "&name=" + UrlEncode(name);
    body += "&avatar=" + UrlEncode(avatar);
    body += "&value=" + UrlEncode(value);
    if (!faceitNick.empty()) {
        body += "&faceit_nick=" + UrlEncode(faceitNick);
    }

    nlohmann::json response;
    if (!HttpPostDirect("/api", body, response)) {
        Logger::logWarning("AngelfraudAPI: ToggleFavorite /api failed");
        return false;
    }

    if (response.contains("status")) {
        std::string status = response["status"];

        // Update favorites cache
        std::lock_guard<std::mutex> lock(favMutex);
        if (status == "added") {
            // Don't manually build — favId would be 0. Clear cache so
            // next GetFavorites->RefreshFavorites gets the real favId.
            favoritesCache.clear();
            favLastFetch = {};
        } else if (status == "removed") {
            favoritesCache.erase(
                std::remove_if(favoritesCache.begin(), favoritesCache.end(),
                    [&](const FavoriteEntry& f) { return f.steamid == steamid; }),
                favoritesCache.end());
        }

        Logger::logInfo(std::format("AngelfraudAPI: ToggleFavorite({}) = {}", steamid, status));
        return true;
    }

    return false;
}

bool AngelfraudAPI::RemoveFavorite(int favId) {
    if (phpsessid.empty()) return false;
    if (csrfToken.empty() && !FetchCsrfToken()) return false;

    std::string getPath = std::format("/favorites?tab=favorites&delete={}&csrf_token={}", favId, csrfToken);
    std::string html;
    if (!HttpGet(getPath, html)) {
        Logger::logWarning(std::format("AngelfraudAPI: RemoveFavorite({}) failed", favId));
        return false;
    }

    // Remove from cache
    std::lock_guard<std::mutex> lock(favMutex);
    favoritesCache.erase(
        std::remove_if(favoritesCache.begin(), favoritesCache.end(),
            [favId](const FavoriteEntry& f) { return f.favId == favId; }),
        favoritesCache.end());

    Logger::logInfo(std::format("AngelfraudAPI: RemoveFavorite({}) done", favId));
    return true;
}

bool AngelfraudAPI::SaveNote(int favId, const std::string& note) {
    if (phpsessid.empty()) return false;
    if (csrfToken.empty() && !FetchCsrfToken()) return false;

    std::string body = std::format("action=update_note&fav_id={}&note={}&csrf_token={}",
        favId, note, csrfToken);

    nlohmann::json response;
    if (!HttpPostDirect("/favorites", body, response)) {
        Logger::logWarning(std::format("AngelfraudAPI: SaveNote({}) failed", favId));
        return false;
    }

    // Update cache
    std::lock_guard<std::mutex> lock(favMutex);
    for (auto& f : favoritesCache) {
        if (f.favId == favId) {
            f.note = note;
            break;
        }
    }

    Logger::logInfo(std::format("AngelfraudAPI: SaveNote({}) saved", favId));
    return true;
}

bool AngelfraudAPI::RefreshFavorites(std::vector<FavoriteEntry>& outFavorites) {
    // Check if cache is fresh (30 second TTL)
    {
        std::lock_guard<std::mutex> lock(favMutex);
        auto elapsed = std::chrono::steady_clock::now() - favLastFetch;
        if (!favoritesCache.empty() && elapsed < std::chrono::seconds(30)) {
            outFavorites = favoritesCache;
            for (auto& f : outFavorites) f.loaded = true;
            return true;
        }
    }

    if (phpsessid.empty()) return false;

    std::string html;
    if (!HttpGet("/favorites?tab=favorites&filter=all", html)) {
        Logger::logWarning("AngelfraudAPI: RefreshFavorites GET failed");
        return false;
    }

    std::lock_guard<std::mutex> lock(favMutex);
    favoritesCache.clear();

    // Parse HTML for favorite cards
    // Each card: <div class="card" id="card-NNNN" ...>
    size_t cardPos = 0;
    int cardCount = 0;
    while (true) {
        // Find card start
        size_t cardStart = html.find(R"(<div class="card" id="card-)", cardPos);
        if (cardStart == std::string::npos) break;
        cardStart = html.find(">", cardStart) + 1;
        size_t cardEnd = html.find(R"(</div>)", cardStart);
        // Find the actual card closure — count nested divs
        int depth = 1;
        size_t searchPos = cardStart;
        while (depth > 0 && searchPos < html.size()) {
            size_t nextOpen = html.find("<div", searchPos);
            size_t nextClose = html.find("</div>", searchPos);
            if (nextClose == std::string::npos) break;
            if (nextOpen < nextClose) {
                depth++;
                searchPos = nextOpen + 5;
            } else {
                depth--;
                searchPos = nextClose + 6;
            }
        }
        cardEnd = searchPos;
        std::string cardHtml = html.substr(cardStart, cardEnd - cardStart);

        // Extract favId from card-XXXX
        size_t idStart = html.find("card-", cardPos);
        size_t idEnd = html.find("\"", idStart);
        std::string idStr = html.substr(idStart + 5, idEnd - idStart - 5);
        int favId = 0;
        try { favId = std::stoi(idStr); } catch (...) {}

        // Extract steamid / name / value / faceit
        FavoriteEntry fe;
        fe.favId = favId;
        fe.loaded = true;

        // SteamID from info-row <b>NNNNNNN</b>
        size_t sidPos = cardHtml.find(R"(<b>)");
        if (sidPos != std::string::npos) {
            sidPos += 3;
            size_t sidEnd = cardHtml.find("</b>", sidPos);
            if (sidEnd != std::string::npos) {
                fe.steamid = cardHtml.substr(sidPos, sidEnd - sidPos);
            }
        }

        // Name from <a ... class="name" ...>
        size_t nmPos = cardHtml.find(R"(class="name")");
        if (nmPos != std::string::npos) {
            nmPos = cardHtml.find(">", nmPos) + 1;
            size_t nmEnd = cardHtml.find("</a>", nmPos);
            if (nmEnd != std::string::npos) {
                fe.name = cardHtml.substr(nmPos, nmEnd - nmPos);
            }
        }

        // Value from badge-value-green or badge-value-red
        size_t valPos = cardHtml.find(R"(badge-value)");
        if (valPos != std::string::npos) {
            valPos = cardHtml.find(">", valPos) + 1;
            size_t valEnd = cardHtml.find("</span>", valPos);
            if (valEnd != std::string::npos) {
                fe.value = cardHtml.substr(valPos, valEnd - valPos);
            }
        }

        // Avatar URL
        size_t avPos = cardHtml.find(R"(<img src=")");
        if (avPos != std::string::npos) {
            avPos += 10;
            size_t avEnd = cardHtml.find("\"", avPos);
            if (avEnd != std::string::npos) {
                fe.avatar = cardHtml.substr(avPos, avEnd - avPos);
            }
        }

        // Note from textarea
        size_t ntPos = cardHtml.find(R"(<textarea)");
        if (ntPos != std::string::npos) {
            ntPos = cardHtml.rfind(">", ntPos);
            if (ntPos != std::string::npos) {
                ntPos++;
                size_t ntEnd = cardHtml.find("</textarea>", ntPos);
                if (ntEnd != std::string::npos) {
                    fe.note = cardHtml.substr(ntPos, ntEnd - ntPos);
                    // HTML decode
                    auto decode = [](std::string s) -> std::string {
                        auto rep = [&](const std::string& from, const std::string& to) {
                            size_t p = 0;
                            while ((p = s.find(from, p)) != std::string::npos) {
                                s.replace(p, from.size(), to);
                                p += to.size();
                            }
                        };
                        rep("&amp;", "&");
                        rep("&lt;", "<");
                        rep("&gt;", ">");
                        rep("&quot;", "\"");
                        rep("&#039;", "'");
                        return s;
                    };
                    fe.note = decode(fe.note);
                }
            }
        }

        favoritesCache.push_back(fe);
        cardCount++;
        cardPos = cardEnd + 6; // skip </div>
    }

    favLastFetch = std::chrono::steady_clock::now();
    Logger::logInfo(std::format("AngelfraudAPI: RefreshFavorites loaded {} favorites", cardCount));

    outFavorites = favoritesCache;
    return true;
}

std::vector<FavoriteEntry> AngelfraudAPI::GetFavorites() {
    std::lock_guard<std::mutex> lock(favMutex);
    return favoritesCache;
}

bool AngelfraudAPI::IsFavorite(const std::string& steamid) {
    std::lock_guard<std::mutex> lock(favMutex);
    for (const auto& f : favoritesCache) {
        if (f.steamid == steamid) return true;
    }
    // If cache is empty, try to load
    if (favoritesCache.empty()) {
        // Can't check under lock, release and try
        // For now just return false — will be loaded on demand
    }
    return false;
}

// ─── Direct HTTP POST (no JSON parsing, returns raw JSON) ────────────────────

bool AngelfraudAPI::HttpPostDirect(const std::string& path, const std::string& body, nlohmann::json& outJson) {
    std::string host = cfg->angelfraudSiteUrl;
    if (host.find("https://") == 0) host = host.substr(8);
    else if (host.find("http://") == 0) host = host.substr(7);
    if (!host.empty() && host.back() == '/') host.pop_back();

    HINTERNET hSession = WinHttpOpen(L"AngelfraudAPI/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    int hostLen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    std::wstring whost(hostLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &whost[0], hostLen);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
        std::wstring(path.begin(), path.end()).c_str(),
        NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    std::string cookieHeader = "Cookie: PHPSESSID=" + phpsessid;
    if (!cfClearance.empty()) cookieHeader += "; cf_clearance=" + cfClearance;
    std::wstring wCookie(cookieHeader.begin(), cookieHeader.end());
    WinHttpAddRequestHeaders(hRequest, wCookie.c_str(), wCookie.size(), WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring contentType = L"Content-Type: application/x-www-form-urlencoded";
    WinHttpAddRequestHeaders(hRequest, contentType.c_str(), contentType.size(), WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring referer = L"Referer: https://angelfraud.steamcommunity.asia/";
    WinHttpAddRequestHeaders(hRequest, referer.c_str(), referer.size(), WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring ua = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0";
    WinHttpAddRequestHeaders(hRequest, ua.c_str(), ua.size(), WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0,
        (LPVOID)body.c_str(), body.size(), body.size(), 0);
    if (!sent) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    BOOL received = WinHttpReceiveResponse(hRequest, NULL);
    if (!received) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    std::string responseData;
    DWORD bytesRead = 0;
    char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        responseData += buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (responseData.empty()) return false;

    try {
        outJson = nlohmann::json::parse(responseData);
        return true;
    } catch (...) {
        return false;
    }
}

void AngelfraudAPI::WorkerLoop() {
    int checkCounter = 0;
    const int MAX_CONCURRENT = 1;   // 1 request at a time (rate limited to 1 per 5s)

    int profileCounter = 0;

    while (running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (!running) break;

        // Don't fetch prices in launcher mode — wait for START
        if (globals::appMode != 1) {
            checkCounter = 0;
            profileCounter = 0;
            continue;
        }

        // ── Periodic profile refresh (every ~30s) ──
        profileCounter++;
        if (profileCounter >= 30 && !phpsessid.empty()) {
            profileCounter = 0;
            ProfileData pd;
            if (FetchAngelfraudProfile(phpsessid, pd)) {
                std::lock_guard<std::mutex> lock(globals::profileMutex);
                globals::profile = pd;
            }
        }

        // Get current match players and check cache in one pass
        std::vector<std::string> pendingIds;  // steamids that need fetching
        {
            std::lock_guard<std::mutex> lock(globals::playersMutex);
            for (auto& player : globals::matchPlayers) {
                if (player.steamid.empty()) continue;
                if (player.valuePending) continue;

                // Try cache first
                double cachedValue = 0;
                std::string cachedStatus;
                if (GetPrice(player.steamid, cachedValue, cachedStatus)) {
                    player.totalValue = cachedValue;
                    player.valueLoaded = (cachedStatus == "ok");
                    player.valueError = (cachedStatus != "ok");
                    player.valueStatus = cachedStatus;
                    player.valuePending = false;
                } else if (!phpsessid.empty()) {
                    // Needs fetching
                    player.valuePending = true;
                    pendingIds.push_back(player.steamid);
                }
            }
        }

        if (pendingIds.empty() || phpsessid.empty()) {
            // Periodic cache cleanup even if nothing to fetch
            checkCounter++;
            if (checkCounter >= 60) { // ~60 seconds
                checkCounter = 0;
                CleanupCache();
            }
            continue;
        }

        Logger::logInfo(std::format("AngelfraudAPI: Fetching {} player(s) with {} worker(s)",
            pendingIds.size(), std::min(MAX_CONCURRENT, (int)pendingIds.size())));

        // ── Parallel fetching with thread pool ──
        std::atomic<size_t> nextIdx{ 0 };
        std::vector<std::thread> workers;
        int numWorkers = std::min(MAX_CONCURRENT, (int)pendingIds.size());

        auto workerFunc = [&]() {
            while (running) {
                size_t idx = nextIdx.fetch_add(1);
                if (idx >= pendingIds.size()) break;

                const std::string& sid = pendingIds[idx];

                double value = 0;
                std::string status;
                bool ok = FetchPrice(sid, value, status);

                if (running) {
                    std::lock_guard<std::mutex> lock(globals::playersMutex);
                    for (auto& p : globals::matchPlayers) {
                        if (p.steamid == sid) {
                            if (ok) {
                                p.totalValue = value;
                                p.valueLoaded = (status == "ok");
                                p.valueError = (status != "ok");
                            } else {
                                p.valueError = true;
                                p.valueLoaded = false;
                            }
                            p.valueStatus = status;
                            p.valuePending = false;
                            break;
                        }
                    }
                }

                // Per-thread rate limiting: 1 request per 5 seconds
                std::this_thread::sleep_for(std::chrono::milliseconds(5000));
            }
        };

        for (int i = 0; i < numWorkers; i++) {
            workers.emplace_back(workerFunc);
        }
        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }

        Logger::logInfo("AngelfraudAPI: Batch fetch complete");

        // Periodic cache cleanup
        checkCounter++;
        if (checkCounter >= 60) { // ~60 seconds
            checkCounter = 0;
            CleanupCache();
        }
    }
}

bool AngelfraudAPI::HttpPost(const std::string& steamid, nlohmann::json& outJson, const std::string& path, const std::string& fieldName) {
    // Strip protocol prefix from the site URL to get just the hostname
    std::string host = cfg->angelfraudSiteUrl;
    if (host.find("https://") == 0) host = host.substr(8);
    else if (host.find("http://") == 0) host = host.substr(7);
    // Strip trailing slash if present
    if (!host.empty() && host.back() == '/') host.pop_back();

    // Build form data: {fieldName}={steamid}  (e.g. steam_input=ID or inventory_steamid=ID)
    std::string body = fieldName + "=" + steamid;

    // Use WinHTTP
    HINTERNET hSession = WinHttpOpen(L"AngelfraudAPI/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        Logger::logWarning("AngelfraudAPI: WinHttpOpen failed");
        return false;
    }

    // Convert host to wide string
    int hostLen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    std::wstring whost(hostLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &whost[0], hostLen);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        Logger::logWarning(std::format("AngelfraudAPI: WinHttpConnect failed for host '{}'", host));
        return false;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST",
        std::wstring(path.begin(), path.end()).c_str(), NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        Logger::logWarning("AngelfraudAPI: WinHttpOpenRequest failed");
        return false;
    }

    // Set cookie header (PHPSESSID + cf_clearance if available)
    std::string cookieHeader = "Cookie: PHPSESSID=" + phpsessid;
    if (!cfClearance.empty()) {
        cookieHeader += "; cf_clearance=" + cfClearance;
    }
    std::wstring wCookieHeader(cookieHeader.begin(), cookieHeader.end());
    WinHttpAddRequestHeaders(hRequest, wCookieHeader.c_str(), wCookieHeader.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    // Set content type
    std::wstring contentType = L"Content-Type: application/x-www-form-urlencoded";
    WinHttpAddRequestHeaders(hRequest, contentType.c_str(), contentType.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    // Set Referer to the site (some CS2 inventory checkers require it)
    std::wstring referer = L"Referer: https://angelfraud.steamcommunity.asia/";
    WinHttpAddRequestHeaders(hRequest, referer.c_str(), referer.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    // Additional browser-like headers
    std::wstring userAgent = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0";
    WinHttpAddRequestHeaders(hRequest, userAgent.c_str(), userAgent.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring accept = L"Accept: application/json, text/plain, */*";
    WinHttpAddRequestHeaders(hRequest, accept.c_str(), accept.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring acceptLang = L"Accept-Language: en-US,en;q=0.5";
    WinHttpAddRequestHeaders(hRequest, acceptLang.c_str(), acceptLang.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    std::wstring origin = L"Origin: https://angelfraud.steamcommunity.asia";
    WinHttpAddRequestHeaders(hRequest, origin.c_str(), origin.size(),
        WINHTTP_ADDREQ_FLAG_ADD);

    // Send request
    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0,
        (LPVOID)body.c_str(), body.size(), body.size(), 0);
    if (!sent) {
        Logger::logWarning("AngelfraudAPI: WinHttpSendRequest failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL received = WinHttpReceiveResponse(hRequest, NULL);
    if (!received) {
        Logger::logWarning("AngelfraudAPI: WinHttpReceiveResponse failed");
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    // Read response
    std::string responseData;
    DWORD bytesRead = 0;
    char buffer[4096];

    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        responseData += buffer;
    }

    // Log full response for debugging
    Logger::logInfo(std::format("AngelfraudAPI: Response ({} bytes): {}",
        responseData.size(),
        responseData.size() > 1500 ? responseData.substr(0, 1500) + "..." : responseData));

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (responseData.empty()) {
        Logger::logWarning("AngelfraudAPI: Empty response");
        return false;
    }

    // Parse JSON
    try {
        outJson = nlohmann::json::parse(responseData);
        return true;
    }
    catch (const std::exception& e) {
        Logger::logWarning(std::format("AngelfraudAPI: JSON parse error: {}", e.what()));
        return false;
    }
}

// ─── Profile fetching ───────────────────────────────────────────────────────

static double ParseMoney(const std::string& s) {
    // Strip $ and whitespace, parse with strtod
    std::string clean;
    for (char c : s) {
        if (c == '$') continue;
        if (c == ' ' || c == ',') continue;
        clean += c;
    }
    if (clean.empty()) return 0.0;
    try { return std::stod(clean); }
    catch (...) { return 0.0; }
}

static std::string ExtractBetween(const std::string& html, const std::string& start, const std::string& end, size_t offset = 0) {
    size_t a = html.find(start, offset);
    if (a == std::string::npos) return "";
    a += start.size();
    size_t b = html.find(end, a);
    if (b == std::string::npos) return "";
    return html.substr(a, b - a);
}

bool FetchAngelfraudProfile(const std::string& phpsessid, ProfileData& out) {
    std::string host = cfg->angelfraudSiteUrl;
    if (host.find("https://") == 0) host = host.substr(8);
    else if (host.find("http://") == 0) host = host.substr(7);
    if (!host.empty() && host.back() == '/') host.pop_back();

    HINTERNET hSession = WinHttpOpen(L"AngelfraudProfile/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) return false;

    int hostLen = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    std::wstring whost(hostLen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &whost[0], hostLen);

    HINTERNET hConnect = WinHttpConnect(hSession, whost.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", L"/profile",
        NULL, NULL, NULL, WINHTTP_FLAG_SECURE);
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    // Cookie
    std::string cookieHeader = "Cookie: PHPSESSID=" + phpsessid;
    std::wstring wCookie(cookieHeader.begin(), cookieHeader.end());
    WinHttpAddRequestHeaders(hRequest, wCookie.c_str(), wCookie.size(), WINHTTP_ADDREQ_FLAG_ADD);

    // Browser headers
    std::wstring ua = L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0";
    WinHttpAddRequestHeaders(hRequest, ua.c_str(), ua.size(), WINHTTP_ADDREQ_FLAG_ADD);

    BOOL sent = WinHttpSendRequest(hRequest, NULL, 0, NULL, 0, 0, 0);
    if (!sent) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    BOOL received = WinHttpReceiveResponse(hRequest, NULL);
    if (!received) { WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return false; }

    std::string html;
    DWORD bytesRead = 0;
    char buffer[4096];
    while (WinHttpReadData(hRequest, buffer, sizeof(buffer) - 1, &bytesRead) && bytesRead > 0) {
        buffer[bytesRead] = '\0';
        html += buffer;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (html.empty()) return false;

    // ── Parse worker nickname ──
    std::string name = ExtractBetween(html, R"(<div class="hero-name">)", "</div>");
    if (!name.empty()) out.nickname = name;

    // ── Parse worker ID ──
    std::string idStr = ExtractBetween(html, "ID:", "<");
    if (!idStr.empty()) {
        // Trim whitespace
        while (!idStr.empty() && idStr[0] == ' ') idStr.erase(0, 1);
        try { out.workerId = std::stoi(idStr); }
        catch (...) {}
    }

    // ── Parse share percentage ──
    // "Повышение процента с 50% до"
    std::string pctStr = ExtractBetween(html, "с ", "%");
    if (!pctStr.empty()) {
        try { out.currentSharePercent = std::stoi(pctStr); }
        catch (...) {}
    }
    std::string pctTarget = ExtractBetween(html, "до", "%");
    if (!pctTarget.empty()) {
        try { out.targetSharePercent = std::stoi(pctTarget); }
        catch (...) {}
    }

    // ── Parse financial stats from f-val + f-lbl pairs ──
    // Order: Грязный профит, Ваша доля, Выплачено, Долг
    auto extractStat = [&](const std::string& label) -> double {
        size_t lblPos = html.find(label);
        if (lblPos == std::string::npos) return 0.0;
        // Find the f-val before this label
        size_t valEnd = html.rfind("</div>", lblPos);
        if (valEnd == std::string::npos) return 0.0;
        size_t valStart = html.rfind(">", valEnd - 1);
        if (valStart == std::string::npos) return 0.0;
        std::string valStr = html.substr(valStart + 1, valEnd - valStart - 1);
        return ParseMoney(valStr);
    };

    out.grossProfit = extractStat("Грязный профит");
    out.yourShare = extractStat("Ваша доля");
    out.paid = extractStat("Выплачено");
    out.debt = extractStat("Долг");
    out.loaded = true;
    out.lastFetch = std::chrono::steady_clock::now();

    Logger::logInfo(std::format("Profile: {} (ID {}), profit=${:.2f}, share=${:.2f}, debt=${:.2f}, {}%->{}%",
        out.nickname, out.workerId, out.grossProfit, out.yourShare, out.debt,
        out.currentSharePercent, out.targetSharePercent));

    return true;
}
