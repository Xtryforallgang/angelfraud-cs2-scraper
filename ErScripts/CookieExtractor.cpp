#include "CookieExtractor.h"
#include "Logger.h"
#include "Config.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#include <dpapi.h>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cstring>
#include <filesystem>

#pragma comment(lib, "crypt32.lib")
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

// BCrypt types from the Windows SDK — provide fallback defines for older SDKs
#ifndef BCRYPT_KEY_DATA_BLOB_VERSION
#define BCRYPT_KEY_DATA_BLOB_VERSION 1
#endif
#ifndef BCRYPT_AUTH_TAG_LENGTH
#define BCRYPT_AUTH_TAG_LENGTH L"AuthTagLength"
#endif
// BCrypt authenticated mode info struct (Win8+)
typedef struct _BCRYPT_AUTHENTICATED_MODE_MODE_INFO {
    ULONG dwVersion;
    ULONG cbNonce;
    ULONG cbAuthData;
    ULONG cbTag;
    PBYTE pbNonce;
    PBYTE pbAuthData;
    PBYTE pbTag;
} BCRYPT_AUTHENTICATED_MODE_MODE_INFO;

namespace fs = std::filesystem;

// ─── Firefox extraction ──────────────────────────────────────────────────────

std::string CookieExtractor::GetFirefoxProfilePath() {
    char* appData = nullptr;
    size_t len = 0;
    if (_dupenv_s(&appData, &len, "APPDATA") != 0 || !appData) {
        return "";
    }
    std::string base(appData);
    free(appData);

    // Firefox-based browsers and their profile directories
    std::vector<std::string> browserPaths = {
        base + "\\Mozilla\\Firefox\\Profiles",
        base + "\\Zen\\Profiles",
    };

    for (const auto& profilesDir : browserPaths) {
        if (!fs::exists(profilesDir)) continue;

        // Enumerate all subdirectories, looking for cookies.sqlite
        for (const auto& entry : fs::directory_iterator(profilesDir)) {
            if (!entry.is_directory()) continue;

            std::string profilePath = entry.path().string();
            std::string cookieFile = profilePath + "\\cookies.sqlite";

            if (fs::exists(cookieFile)) {
                Logger::logInfo(std::format("CookieExtractor: Found profile at {}", profilePath));
                return profilePath;
            }
        }
    }

    return "";
}

std::string CookieExtractor::ExtractFirefoxCookies(const std::string& profilePath) {
    std::string cookieFile = profilePath + "\\cookies.sqlite";
    if (!fs::exists(cookieFile)) {
        cookieFile = profilePath + "\\cookies.sqlite-wal";
        if (!fs::exists(cookieFile)) return "";
    }

    Logger::logInfo(std::format("CookieExtractor: Scanning {} for cookies", cookieFile));

    // Read entire file into memory
    std::ifstream file(cookieFile, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return "";
    size_t size = (size_t)file.tellg();
    file.seekg(0);
    std::vector<char> data(size);
    if (!file.read(data.data(), size)) {
        file.close();
        return "";
    }
    file.close();

    // Identify the angelfraud domain (without protocol)
    std::string domain = cfg->angelfraudSiteUrl;
    if (domain.find("https://") == 0) domain = domain.substr(8);
    else if (domain.find("http://") == 0) domain = domain.substr(7);
    // Also try with subdomain variants
    std::vector<std::string> domainVariants = { domain };
    if (domain.find("www.") == 0)
        domainVariants.push_back(domain.substr(4));
    else
        domainVariants.push_back("www." + domain);
    // Also try just the main hostname
    size_t dotPos = domain.find('.');
    if (dotPos != std::string::npos) {
        std::string mainDomain = domain.substr(dotPos + 1); // e.g. "steamcommunity.asia"
        domainVariants.push_back(mainDomain);
    }

    // Step 1: Find any domain variant in the file (case-insensitive)
    // to confirm the cookies database has angelfraud data
    bool domainFound = false;
    for (const auto& dv : domainVariants) {
        std::string lowerDv;
        for (char c : dv) lowerDv += (char)tolower((unsigned char)c);
        for (size_t i = 0; i + lowerDv.size() <= size; i++) {
            bool match = true;
            for (size_t j = 0; j < lowerDv.size(); j++) {
                if (tolower((unsigned char)data[i + j]) != lowerDv[j]) { match = false; break; }
            }
            if (match) { domainFound = true; break; }
        }
        if (domainFound) { Logger::logInfo("CookieExtractor: Found angelfraud domain in cookies"); break; }
    }

    if (!domainFound) {
        Logger::logWarning("CookieExtractor: angelfraud domain not found in cookies DB");
        Logger::logInfo("Make sure you are logged in at the panel website in your browser");
        return "";
    }

    // Step 2 & 3: Find PHPSESSID and cf_clearance in a single pass
    // For each domain occurrence, search backward for both cookie names
    std::string foundPhpsessid;
    std::string foundCfClearance;

    for (const auto& dv : domainVariants) {
        for (size_t i = 0; i + dv.size() <= size; i++) {
            // Check if domain matches (case-insensitive)
            bool domainMatch = true;
            for (size_t j = 0; j < dv.size(); j++) {
                if (tolower((unsigned char)data[i + j]) != tolower((unsigned char)dv[j])) {
                    domainMatch = false;
                    break;
                }
            }
            if (!domainMatch) continue;

            // Found domain — search backward for cookies within this SQLite cell
            size_t searchBack = (i > 512) ? i - 512 : 0;
            for (size_t bp = searchBack; bp < i; bp++) {
                // Check if this position has "phpsessid" (9 chars)
                bool isPhpsessid = true;
                for (size_t j = 0; j < 9; j++) {
                    if (tolower((unsigned char)data[bp + j]) != "phpsessid"[j]) {
                        isPhpsessid = false;
                        break;
                    }
                }

                // Check if this position has "cf_clearance" (12 chars) — only if not found yet
                bool isCfClearance = false;
                if (!isPhpsessid && foundCfClearance.empty()) {
                    isCfClearance = true;
                    for (size_t j = 0; j < 12; j++) {
                        if (tolower((unsigned char)data[bp + j]) != "cf_clearance"[j]) {
                            isCfClearance = false;
                            break;
                        }
                    }
                }

                if (!isPhpsessid && !isCfClearance) continue;

                // Determine marker characteristics
                size_t markerLen = isPhpsessid ? 9 : 12;
                size_t maxValLen = isPhpsessid ? 40 : 128;
                size_t minValLen = isPhpsessid ? 26 : 30;

                // Skip binary bytes (non-alphanumeric) — SQLite stores varint type
                // and length prefix bytes between fields
                size_t valueStart = bp + markerLen;
                while (valueStart < i && !isalnum((unsigned char)data[valueStart])) {
                    valueStart++;
                }

                // Extract the token (alphanumeric + hyphens/underscores/dots for cf_clearance)
                size_t scan = valueStart;
                size_t valLen = 0;
                auto isCookieChar = [](unsigned char c, bool allowExtra) -> bool {
                    if (isalnum(c)) return true;
                    if (c == '-' || c == '_') return true;
                    if (allowExtra && (c == '.')) return true;
                    return false;
                };

                while (scan < i && isCookieChar((unsigned char)data[scan], isCfClearance)) {
                    valLen++;
                    scan++;
                    if (valLen > maxValLen) break;
                }

                if (valLen < minValLen || valLen > maxValLen) continue;

                std::string candidate(data.data() + valueStart, valLen);

                if (isPhpsessid) {
                    // Validate PHPSESSID
                    bool valid = true;
                    for (char c : candidate) {
                        if (!isalnum((unsigned char)c) && c != '-' && c != '_') {
                            valid = false;
                            break;
                        }
                    }
                    if (!valid) continue;

                    if (foundPhpsessid.empty()) {
                        foundPhpsessid = candidate;
                        Logger::logInfo("CookieExtractor: Found PHPSESSID in Firefox cookies for angelfraud domain");
                        Logger::logInfo(std::format("CookieExtractor: PHPSESSID = {}", candidate));
                    }
                }
                else { // cf_clearance
                    if (foundCfClearance.empty()) {
                        foundCfClearance = candidate;
                        Logger::logInfo("CookieExtractor: Found cf_clearance in Firefox cookies for angelfraud domain");
                        Logger::logInfo(std::format("CookieExtractor: cf_clearance = {}... ({} chars)", candidate.substr(0, 20), candidate.size()));
                    }
                }

                // If we found both, we can stop searching
                if (!foundPhpsessid.empty() && !foundCfClearance.empty()) break;
            }
            // If we found both cookies for this domain hit, stop
            if (!foundPhpsessid.empty() && !foundCfClearance.empty()) break;
        }
        // If we found both, stop iterating domain variants
        if (!foundPhpsessid.empty() && !foundCfClearance.empty()) break;
    }

    // Store cf_clearance for later retrieval
    CookieExtractor::extractedCfClearance = foundCfClearance;

    if (foundPhpsessid.empty()) {
        Logger::logWarning("CookieExtractor: PHPSESSID not found in cookies DB");
        Logger::logInfo("Make sure you are logged in at the panel website in your browser");
        return "";
    }

    return foundPhpsessid;
}

std::string CookieExtractor::ExtractFromFirefox() {
    std::string profilePath = GetFirefoxProfilePath();
    if (profilePath.empty()) {
        Logger::logWarning("CookieExtractor: Firefox profile not found");
        return "";
    }
    return ExtractFirefoxCookies(profilePath);
}

// ─── Chrome/Edge extraction (DPAPI + AES-GCM) ────────────────────────────────

// Initialize AES-GCM provider and import key. Returns handles or false.
static bool InitGcmKey(const std::vector<BYTE>& aesKey,
    BCRYPT_ALG_HANDLE& hAlg, BCRYPT_KEY_HANDLE& hKey)
{
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, NULL, 0) < 0)
        return false;

    BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
        (PBYTE)BCRYPT_CHAIN_MODE_GCM, sizeof(BCRYPT_CHAIN_MODE_GCM), 0);

    DWORD tagLen = 16;
    BCryptSetProperty(hAlg, BCRYPT_AUTH_TAG_LENGTH,
        (PBYTE)&tagLen, sizeof(tagLen), 0);

    BCRYPT_KEY_DATA_BLOB_HEADER keyHdr;
    keyHdr.dwVersion = BCRYPT_KEY_DATA_BLOB_VERSION;
    keyHdr.cbKeyData = (ULONG)aesKey.size();
    std::vector<BYTE> keyBlob(sizeof(keyHdr) + aesKey.size());
    memcpy(keyBlob.data(), &keyHdr, sizeof(keyHdr));
    memcpy(keyBlob.data() + sizeof(keyHdr), aesKey.data(), aesKey.size());

    if (BCryptImportKey(hAlg, NULL, BCRYPT_KEY_DATA_BLOB, &hKey,
        NULL, 0, keyBlob.data(), (ULONG)keyBlob.size(), 0) < 0)
    {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    return true;
}

// Decrypt a single Chrome cookie blob with pre-created key handles.
// blob points to "v10" + nonce + ciphertext + tag.
static bool TryDecryptOne(BCRYPT_KEY_HANDLE hKey,
    const BYTE* blob, size_t blobLen, std::string& out)
{
    if (blobLen < 31) return false;

    ULONG ctSize = (ULONG)blobLen - 3 - 12 - 16;
    if (ctSize < 1) return false;

    BCRYPT_AUTHENTICATED_MODE_MODE_INFO authInfo = {};
    authInfo.dwVersion = 1;
    authInfo.cbNonce   = 12;
    authInfo.pbNonce   = const_cast<BYTE*>(blob + 3);
    authInfo.cbTag     = 16;
    authInfo.pbTag     = const_cast<BYTE*>(blob + blobLen - 16);
    authInfo.cbAuthData = 0;
    authInfo.pbAuthData = NULL;

    std::vector<BYTE> plaintext(ctSize + 16);
    ULONG resultLen = 0;
    NTSTATUS status = BCryptDecrypt(hKey,
        const_cast<BYTE*>(blob + 15), ctSize,
        &authInfo, NULL, 0,
        plaintext.data(), (ULONG)plaintext.size(),
        &resultLen, 0);

    if (status < 0) return false;
    out.assign((const char*)plaintext.data(), resultLen);

    // Validate: should be alphanumeric (plus hyphens/underscores/dots)
    bool valid = true;
    for (char c : out) {
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') {
            valid = false; break;
        }
    }
    if (!valid || out.size() < 16 || out.size() > 64) return false;
    return true;
}

// Scan for "v10"/"v11" after startPos and try AES-GCM decrypt with
// pre-created key. Uses a quick pre-filter to skip non-binary data.
static std::string FindAndDecryptChromeCookie(const BYTE* data, size_t dataSize,
    size_t startPos, size_t maxForward, const std::vector<BYTE>& aesKey)
{
    BCRYPT_ALG_HANDLE hAlg = NULL;
    BCRYPT_KEY_HANDLE hKey = NULL;
    if (!InitGcmKey(aesKey, hAlg, hKey)) return "";

    std::string result;
    size_t searchEnd = std::min(startPos + maxForward, dataSize);

    for (size_t p = startPos; p < searchEnd - 3; p++) {
        if (data[p] != 'v') continue;
        if ((data[p+1] != '1' && data[p+1] != '2') || data[p+2] != '0')
            continue;

        // Quick pre-filter: nonce (bytes 3-14) should have high binary bytes
        bool hasHighByte = false;
        for (size_t i = 3; i < 15 && p + i < dataSize; i++) {
            if (data[p + i] > 0x7E) { hasHighByte = true; break; }
        }
        if (!hasHighByte) continue;

        // Try blob lengths 31-100 (typical cookie sizes)
        for (size_t blobLen = 31; blobLen <= 100 && p + blobLen <= dataSize; blobLen++) {
            if (TryDecryptOne(hKey, data + p, blobLen, result)) {
                BCryptDestroyKey(hKey);
                BCryptCloseAlgorithmProvider(hAlg, 0);
                return result;
            }
        }
    }

    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return "";
}



std::vector<std::string> CookieExtractor::GetChromeCookiePaths() {
    std::vector<std::string> paths;
    char* localAppData = nullptr;
    size_t len = 0;

    if (_dupenv_s(&localAppData, &len, "LOCALAPPDATA") != 0 || !localAppData)
        return paths;

    std::string base(localAppData);
    free(localAppData);

    // Chrome
    paths.push_back(base + "\\Google\\Chrome\\User Data\\Default\\Cookies");
    paths.push_back(base + "\\Google\\Chrome\\User Data\\Default\\Network\\Cookies");
    paths.push_back(base + "\\Google\\Chrome\\User Data\\Default\\Network");
    // Edge
    paths.push_back(base + "\\Microsoft\\Edge\\User Data\\Default\\Cookies");
    paths.push_back(base + "\\Microsoft\\Edge\\User Data\\Default\\Network\\Cookies");
    // Brave
    paths.push_back(base + "\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Cookies");
    paths.push_back(base + "\\BraveSoftware\\Brave-Browser\\User Data\\Default\\Network\\Cookies");
    // Opera
    paths.push_back(base + "\\Opera Software\\Opera Stable\\Cookies");
    paths.push_back(base + "\\Opera Software\\Opera Stable\\Network\\Cookies");
    // Vivaldi
    paths.push_back(base + "\\Vivaldi\\User Data\\Default\\Cookies");
    paths.push_back(base + "\\Vivaldi\\User Data\\Default\\Network\\Cookies");
    // Chromium
    paths.push_back(base + "\\Chromium\\User Data\\Default\\Cookies");
    paths.push_back(base + "\\Chromium\\User Data\\Default\\Network\\Cookies");
    // Yandex
    paths.push_back(base + "\\Yandex\\YandexBrowser\\User Data\\Default\\Cookies");
    paths.push_back(base + "\\Yandex\\YandexBrowser\\User Data\\Default\\Network\\Cookies");

    return paths;
}

std::string CookieExtractor::ExtractFromChrome() {
    auto paths = GetChromeCookiePaths();

    for (const auto& cookiePath : paths) {
        if (!fs::exists(cookiePath)) continue;

        Logger::logInfo(std::format("CookieExtractor: Trying Chrome cookie DB: {}", cookiePath));

        // Read the Local State file for the encryption key
        std::string localStatePath = fs::path(cookiePath).parent_path().parent_path().string()
            + "\\Local State";

        DATA_BLOB aesKeyBlob = { 0, nullptr };
        bool hasKey = false;

        if (fs::exists(localStatePath)) {
            std::ifstream lsFile(localStatePath);
            if (lsFile.is_open()) {
                try {
                    nlohmann::json ls;
                    lsFile >> ls;
                    lsFile.close();

                    std::string encKeyB64 = ls["os_crypt"]["encrypted_key"];
                    if (!encKeyB64.empty()) {
                        // Base64 decode
                        DWORD decodedLen = 0;
                        CryptStringToBinaryA(encKeyB64.c_str(), encKeyB64.size(),
                            CRYPT_STRING_BASE64, nullptr, &decodedLen, nullptr, nullptr);
                        if (decodedLen > 5) {
                            std::vector<BYTE> decoded(decodedLen);
                            CryptStringToBinaryA(encKeyB64.c_str(), encKeyB64.size(),
                                CRYPT_STRING_BASE64, decoded.data(), &decodedLen, nullptr, nullptr);

                            // Strip "DPAPI" prefix (5 bytes)
                            DATA_BLOB encryptedKey = { (DWORD)(decodedLen - 5), decoded.data() + 5 };
                            if (CryptUnprotectData(&encryptedKey, nullptr, nullptr, nullptr,
                                nullptr, 0, &aesKeyBlob)) {
                                hasKey = (aesKeyBlob.cbData > 0);
                            }
                        }
                    }
                }
                catch (...) {
                    // JSON parse failed, try without key
                }
            }
        }

        // Read the Cookies DB and find the encrypted value
        HANDLE hFile = CreateFileA(cookiePath.c_str(), GENERIC_READ,
            FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile == INVALID_HANDLE_VALUE) {
            if (hasKey) { LocalFree(aesKeyBlob.pbData); }
            continue;
        }

        DWORD size = GetFileSize(hFile, NULL);
        if (size == 0 || size == INVALID_FILE_SIZE) {
            CloseHandle(hFile);
            if (hasKey) LocalFree(aesKeyBlob.pbData);
            continue;
        }

        HANDLE hMap = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMap) { CloseHandle(hFile); if (hasKey) LocalFree(aesKeyBlob.pbData); continue; }

        const BYTE* data = (const BYTE*)MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
        if (!data) {
            CloseHandle(hMap); CloseHandle(hFile);
            if (hasKey) LocalFree(aesKeyBlob.pbData);
            continue;
        }

        std::string result;
        std::string domain = cfg->angelfraudSiteUrl;

        // Search for the domain in the SQLite file
        for (size_t pos = 0; pos + domain.size() <= (size_t)size; pos++) {
            // Case-insensitive domain search
            bool domainMatch = true;
            for (size_t i = 0; i < domain.size(); i++) {
                if ((data[pos + i] | 0x20) != (BYTE)(domain[i] | 0x20)) {
                    domainMatch = false;
                    break;
                }
            }
            if (!domainMatch) continue;

            // Found domain, search backwards for "phpsessid" (within same cell)
            size_t searchStart = (pos > 2048) ? pos - 2048 : 0;
            size_t namePos = SIZE_MAX;
            for (size_t sp = searchStart; sp < pos; sp++) {
                bool nameMatch = true;
                for (int i = 0; i < 9; i++) {
                    if ((data[sp + i] | 0x20) != (BYTE)"phpsessid"[i]) {
                        nameMatch = false;
                        break;
                    }
                }
                if (nameMatch) { namePos = sp; break; }
            }
            if (namePos == SIZE_MAX) continue;

            // ── AES-GCM (new Chrome v80+, Edge, Brave, Opera, etc.) ──
            if (hasKey) {
                Logger::logInfo("CookieExtractor: Trying AES-GCM decrypt for PHPSESSID");
                result = FindAndDecryptChromeCookie(data, (size_t)size,
                    namePos + 9, 1024,
                    std::vector<BYTE>(aesKeyBlob.pbData, aesKeyBlob.pbData + aesKeyBlob.cbData));
                if (!result.empty()) {
                    Logger::logInfo("CookieExtractor: Found PHPSESSID in Chrome cookies (AES-GCM)");
                }

                // Also try to find cf_clearance from the same domain
                if (!result.empty() && extractedCfClearance.empty()) {
                    // Search forward from after phpsessid for "cf_clearance"
                    for (size_t cfp = namePos + 9; cfp < pos + 512 && cfp + 12 <= (size_t)size; cfp++) {
                        bool cfMatch = true;
                        for (int i = 0; i < 12; i++) {
                            if ((data[cfp + i] | 0x20) != (BYTE)"cf_clearance"[i]) {
                                cfMatch = false; break;
                            }
                        }
                        if (cfMatch) {
                            std::string cfVal = FindAndDecryptChromeCookie(data, (size_t)size,
                                cfp + 12, 512,
                                std::vector<BYTE>(aesKeyBlob.pbData, aesKeyBlob.pbData + aesKeyBlob.cbData));
                            if (!cfVal.empty()) {
                                extractedCfClearance = cfVal;
                                Logger::logInfo("CookieExtractor: Found cf_clearance in Chrome cookies (AES-GCM)");
                                Logger::logInfo(std::format("CookieExtractor: cf_clearance = {}... ({} chars)", cfVal.substr(0, 20), cfVal.size()));
                            }
                            break;
                        }
                    }
                }
            }

            // ── DPAPI fallback (old Chrome pre-v80) ──
            if (result.empty()) {
                for (size_t scan = (namePos > 256 ? namePos - 256 : 0);
                    scan < std::min(pos + 256, (size_t)size) - 16; scan++) {
                    for (DWORD blobLen = 32; blobLen < 256 && scan + blobLen <= (size_t)size; blobLen++) {
                        if (data[scan] == 0x01 && data[scan + 1] == 0x00 && blobLen >= 32) {
                            DATA_BLOB inBlob = { blobLen, const_cast<BYTE*>(data + scan) };
                            DATA_BLOB outBlob = { 0, nullptr };
                            if (CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr,
                                nullptr, 0, &outBlob)) {
                                std::string decrypted((char*)outBlob.pbData, outBlob.cbData);
                                LocalFree(outBlob.pbData);
                                if (decrypted.size() >= 20 && decrypted.size() <= 64) {
                                    bool valid = true;
                                    for (char c : decrypted) {
                                        if (!isalnum(c) && c != '-' && c != '_') { valid = false; break; }
                                    }
                                    if (valid) {
                                        result = decrypted;
                                        Logger::logInfo("CookieExtractor: Found PHPSESSID in Chrome cookies (DPAPI)");
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (!result.empty()) break;
                }
            }

            if (!result.empty()) break;
        }

        UnmapViewOfFile((LPCVOID)data);
        CloseHandle(hMap);
        CloseHandle(hFile);
        if (hasKey) LocalFree(aesKeyBlob.pbData);

        if (!result.empty()) return result;
    }

    return "";
}


// ─── Main extraction entry point ─────────────────────────────────────────────

std::string CookieExtractor::Extract() {
    // Reset cf_clearance at start of each extraction
    extractedCfClearance.clear();

    // Try Firefox first (most reliable, plaintext cookies)
    std::string phpsessid = ExtractFromFirefox();
    if (!phpsessid.empty()) return phpsessid;

    // Try Chrome/Edge
    phpsessid = ExtractFromChrome();
    if (!phpsessid.empty()) return phpsessid;

    Logger::logWarning("CookieExtractor: Could not find PHPSESSID in any browser");
    return "";
}
