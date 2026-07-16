#pragma once

#include <string>
#include <vector>

// Extracts PHPSESSID cookie from browsers for the panel API
class CookieExtractor {
public:
    // Returns the PHPSESSID value, empty string if not found
    static std::string Extract();

    // Try from Firefox specifically
    static std::string ExtractFromFirefox();

    // Try from Chrome/Edge via DPAPI (best-effort, older Chrome versions)
    static std::string ExtractFromChrome();

    // Try from Steam's built-in CEF browser (Steam > View > Internet / overlay browser)
    // Universal — works without Chrome or Firefox
    static std::string ExtractFromSteam();

    // Extract from Firefox using binary scanning of cookies.sqlite
    // Works because Firefox stores cookie values as plaintext in SQLite
    static std::string ExtractFirefoxCookies(const std::string& profilePath);

    // Returns cf_clearance cookie if found during extraction
    static std::string GetCfClearance() { return extractedCfClearance; }

private:
    // Search for a value between two known strings in a binary buffer
    // Returns the text found between startMarker and endMarker
    static std::string ExtractBetween(const char* data, size_t dataSize,
        const std::string& startMarker, const std::string& endMarker,
        size_t maxDistance = 512);

    // Find a browser's profile path
    static std::string GetFirefoxProfilePath();
    static std::vector<std::string> GetChromeCookiePaths();

    // Cloudflare Turnstile clearance cookie (populated during extraction)
    inline static std::string extractedCfClearance;
};
